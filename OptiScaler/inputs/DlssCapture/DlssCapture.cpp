#include "pch.h"
#include "DlssCapture.h"

#include <dxgi1_4.h>
#include <json.hpp>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

// =====================================================================================
// DLSS 输入捕获层
//
// 每帧、每通道的回读流程：
//   1. 从 NVSDK_NGX_Parameter* 取源 ID3D12Resource*（双段式 Get）
//   2. GetDesc() 拿 D3D12_RESOURCE_DESC，GetCopyableFootprints() 算
//      rowPitch / tight 行宽 / 总字节 / 放置布局
//   3. 目标缓冲建在 D3D12_HEAP_TYPE_READBACK（初始态 COPY_DEST，之后不再转换）
//   4. 在【游戏主命令列表】上录制：
//         barrier(原状态 → COPY_SOURCE) → CopyTextureRegion → barrier(COPY_SOURCE → 原状态)
//      —— 必须录在游戏列表上：资源状态转换与渲染顺序必须由同一队列保证；
//         独立队列在 D3D12 legacy barrier 模型下跨队列转状态会产生数据竞争。
//   5. 录制 CopyBufferRegion 把标记值写入 READBACK 标记缓冲（当作 fence 用，
//      因为 OptiScaler 没有 hook ExecuteCommandLists，拿不到游戏队列）
//   6. 后台线程轮询标记缓冲，确认 GPU 完成后 Map 回读缓冲 → 按 rowPitch 逐行写紧凑数据
//
// 约束遵守：
//   · READBACK 堆（不是 UPLOAD）
//   · rowPitch 来自 GetCopyableFootprints，绝不自己算 width*bpp
//   · 复制前后正确管理资源状态，还原为配置的原始状态
//   · 环满时丢帧，绝不阻塞渲染线程
//   · 默认关闭；未激活时每帧只做一次布尔判断
// =====================================================================================

namespace DlssCapture
{
namespace
{
// ---------------------------------------------------------------------------------
// 常量
// ---------------------------------------------------------------------------------
constexpr int kRingSize = 6;
constexpr UINT64 kMarkerBytes = 256;

constexpr int kResCount = 4;
constexpr int RES_COLOR = 0;
constexpr int RES_DEPTH = 1;
constexpr int RES_MOTION = 2;
constexpr int RES_EXPOSURE = 3;

const char* const kResKeys[kResCount] = {
    NVSDK_NGX_Parameter_Color,
    NVSDK_NGX_Parameter_Depth,
    NVSDK_NGX_Parameter_MotionVectors,
    NVSDK_NGX_Parameter_ExposureTexture,
};

const char* const kResFileNames[kResCount] = { "color.bin", "depth.bin", "motion.bin", "exposure.bin" };
const char* const kResNames[kResCount] = { "color", "depth", "motion", "exposure" };

enum class SlotState
{
    Free,
    InFlight,
    Draining,
};

// ---------------------------------------------------------------------------------
// 小工具
// ---------------------------------------------------------------------------------
std::filesystem::path OwnModuleDirectory()
{
    // 用本模块内一个静态对象的地址反查所属 HMODULE（比取函数地址更稳妥）
    static const char kModuleAnchor = 0;

    wchar_t buffer[MAX_PATH] = {};
    HMODULE module = nullptr;

    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&kModuleAnchor), &module) == FALSE ||
        module == nullptr)
    {
        return {};
    }

    const DWORD length = GetModuleFileNameW(module, buffer, MAX_PATH);
    if (length == 0)
        return {};

    return std::filesystem::path(buffer).parent_path();
}

std::string WideToUtf8(const std::wstring& value)
{
    if (value.empty())
        return {};

    const int size =
        WideCharToMultiByte(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);

    if (size <= 0)
        return {};

    std::string result(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), result.data(), size, nullptr,
                        nullptr);
    return result;
}

std::string ZeroPad6(long value)
{
    char buffer[32] = {};
    snprintf(buffer, sizeof(buffer), "%06ld", value);
    return buffer;
}

std::string UtcTimestamp()
{
    const std::time_t now = std::time(nullptr);
    std::tm tmValue = {};
    gmtime_s(&tmValue, &now);

    char buffer[32] = {};
    strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &tmValue);
    return buffer;
}

std::string LocalStampForPath()
{
    const std::time_t now = std::time(nullptr);
    std::tm tmValue = {};
    localtime_s(&tmValue, &now);

    char buffer[32] = {};
    strftime(buffer, sizeof(buffer), "%Y%m%d_%H%M%S", &tmValue);
    return buffer;
}

const char* FormatName(DXGI_FORMAT format)
{
    switch (format)
    {
    case DXGI_FORMAT_UNKNOWN:
        return "DXGI_FORMAT_UNKNOWN";
    case DXGI_FORMAT_R32G32B32A32_FLOAT:
        return "DXGI_FORMAT_R32G32B32A32_FLOAT";
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
        return "DXGI_FORMAT_R16G16B16A16_FLOAT";
    case DXGI_FORMAT_R16G16B16A16_UNORM:
        return "DXGI_FORMAT_R16G16B16A16_UNORM";
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
        return "DXGI_FORMAT_R10G10B10A2_TYPELESS";
    case DXGI_FORMAT_R10G10B10A2_UNORM:
        return "DXGI_FORMAT_R10G10B10A2_UNORM";
    case DXGI_FORMAT_R11G11B10_FLOAT:
        return "DXGI_FORMAT_R11G11B10_FLOAT";
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
        return "DXGI_FORMAT_R8G8B8A8_TYPELESS";
    case DXGI_FORMAT_R8G8B8A8_UNORM:
        return "DXGI_FORMAT_R8G8B8A8_UNORM";
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        return "DXGI_FORMAT_R8G8B8A8_UNORM_SRGB";
    case DXGI_FORMAT_B8G8R8A8_UNORM:
        return "DXGI_FORMAT_B8G8R8A8_UNORM";
    case DXGI_FORMAT_R16G16_TYPELESS:
        return "DXGI_FORMAT_R16G16_TYPELESS";
    case DXGI_FORMAT_R16G16_FLOAT:
        return "DXGI_FORMAT_R16G16_FLOAT";
    case DXGI_FORMAT_R16G16_UNORM:
        return "DXGI_FORMAT_R16G16_UNORM";
    case DXGI_FORMAT_R32_TYPELESS:
        return "DXGI_FORMAT_R32_TYPELESS";
    case DXGI_FORMAT_D32_FLOAT:
        return "DXGI_FORMAT_D32_FLOAT";
    case DXGI_FORMAT_R32_FLOAT:
        return "DXGI_FORMAT_R32_FLOAT";
    case DXGI_FORMAT_R24G8_TYPELESS:
        return "DXGI_FORMAT_R24G8_TYPELESS";
    case DXGI_FORMAT_D24_UNORM_S8_UINT:
        return "DXGI_FORMAT_D24_UNORM_S8_UINT";
    case DXGI_FORMAT_R16_TYPELESS:
        return "DXGI_FORMAT_R16_TYPELESS";
    case DXGI_FORMAT_R16_FLOAT:
        return "DXGI_FORMAT_R16_FLOAT";
    case DXGI_FORMAT_D16_UNORM:
        return "DXGI_FORMAT_D16_UNORM";
    case DXGI_FORMAT_R8_UNORM:
        return "DXGI_FORMAT_R8_UNORM";
    default:
        return "DXGI_FORMAT_OTHER";
    }
}

// binary16 → float，建表一次（与离线 host 的 HalfLut 同源，保证两端解码一致）。
const float* HalfTable()
{
    static const std::vector<float> table = [] {
        std::vector<float> t(65536);
        for (int i = 0; i < 65536; ++i)
        {
            const uint32_t s = (static_cast<uint32_t>(i) >> 15) & 1u;
            const uint32_t e = (static_cast<uint32_t>(i) >> 10) & 0x1Fu;
            const uint32_t m = static_cast<uint32_t>(i) & 0x3FFu;
            float v;
            if (e == 0)
                v = static_cast<float>(m) * 5.9604644775390625e-08f; // 2^-24
            else if (e == 31)
                v = (m == 0) ? 3.0e38f : 0.0f;
            else
                v = (1.0f + static_cast<float>(m) / 1024.0f) * std::pow(2.0f, static_cast<float>(e) - 15.0f);
            t[i] = s ? -v : v;
        }
        return t;
    }();
    return table.data();
}

// float → DXGI_FORMAT_R11G11B10_FLOAT 的单个字段（无符号浮点，5 位指数 + mantBits 位尾数）。
// 负数 / 下溢 → 0，上溢 / inf / NaN → 字段最大值。颜色恒为非负，无需符号位。
uint32_t PackUnsignedFloat(float v, int mantBits)
{
    const uint32_t maxField = (0x1Fu << mantBits) | ((1u << mantBits) - 1u);
    if (!(v > 0.0f))
        return 0;

    uint32_t bits = 0;
    std::memcpy(&bits, &v, sizeof(bits));
    const uint32_t exp = (bits >> 23) & 0xFFu;
    const uint32_t mant = bits & 0x7FFFFFu;

    if (exp == 0xFFu)
        return maxField;

    int newExp = static_cast<int>(exp) - 127 + 15;
    if (newExp <= 0)
        return 0;
    if (newExp >= 31)
        return maxField;

    const int shift = 23 - mantBits;
    uint32_t rounded = mant + (1u << (shift - 1)); // 四舍五入到 mantBits 位
    if (rounded & 0x800000u)                       // 尾数进位 → 指数 +1
    {
        rounded = 0;
        if (++newExp >= 31)
            return maxField;
    }
    return (static_cast<uint32_t>(newExp) << mantBits) | (rounded >> shift);
}

// RGBA16F 的一个像素（R,G,B half）→ 打包的 R11G11B10_FLOAT。
uint32_t PackR11G11B10(const uint16_t* rgba16)
{
    const float* half = HalfTable();
    const uint32_t r = PackUnsignedFloat(half[rgba16[0]], 6);
    const uint32_t g = PackUnsignedFloat(half[rgba16[1]], 6);
    const uint32_t b = PackUnsignedFloat(half[rgba16[2]], 5);
    return r | (g << 11) | (b << 22);
}

void TransitionResource(ID3D12GraphicsCommandList* cmdList, ID3D12Resource* resource, D3D12_RESOURCE_STATES before,
                        D3D12_RESOURCE_STATES after)
{
    if (cmdList == nullptr || resource == nullptr || before == after)
        return;

    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    barrier.Transition.pResource = resource;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    cmdList->ResourceBarrier(1, &barrier);
}

bool CreateBuffer(ID3D12Device* device, D3D12_HEAP_TYPE heapType, UINT64 size, D3D12_RESOURCE_STATES initialState,
                  ID3D12Resource** out)
{
    if (device == nullptr || out == nullptr || size == 0)
        return false;

    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = heapType;
    heapProps.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    heapProps.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    heapProps.CreationNodeMask = 1;
    heapProps.VisibleNodeMask = 1;

    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Alignment = 0;
    desc.Width = size;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_UNKNOWN;
    desc.SampleDesc.Count = 1;
    desc.SampleDesc.Quality = 0;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    desc.Flags = D3D12_RESOURCE_FLAG_NONE;

    const HRESULT hr = device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &desc, initialState, nullptr,
                                                       IID_PPV_ARGS(out));

    if (FAILED(hr) || *out == nullptr)
    {
        LOG_ERROR("[Capture] CreateCommittedResource failed: {:X}", static_cast<unsigned int>(hr));
        *out = nullptr;
        return false;
    }

    return true;
}

// ---------------------------------------------------------------------------------
// 数据结构
// ---------------------------------------------------------------------------------
struct ReadbackTarget
{
    ID3D12Resource* buffer = nullptr;
    UINT64 capacity = 0;

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
    UINT rowPitch = 0;
    UINT rowCount = 0;
    UINT64 tightRowBytes = 0;
    UINT64 totalBytes = 0;

    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    D3D12_RESOURCE_STATES originalState = D3D12_RESOURCE_STATE_COMMON;

    bool captured = false;
    bool skipped = false;
    std::string skipReason;
};

struct Slot
{
    ReadbackTarget res[kResCount];

    ID3D12Resource* markerSrc = nullptr;
    void* markerSrcMap = nullptr;
    ID3D12Resource* markerDst = nullptr;
    void* markerDstMap = nullptr;

    UINT64 markerValue = 0;
    SlotState state = SlotState::Free;

    long frameIndex = 0;
    long engineFrameCount = 0;
    unsigned int width = 0;
    unsigned int height = 0;
    unsigned int targetWidth = 0;
    unsigned int targetHeight = 0;
    float jitterX = 0.0f;
    float jitterY = 0.0f;
    float motionScaleX = 1.0f;
    float motionScaleY = 1.0f;
    float frameTimeMs = 0.0f;
    bool isHdr = false;
    bool lowResMV = false;
    bool jitteredMV = false;
    bool depthInverted = false;
    bool autoExposure = false;
    std::string featureName;
};

// ---------------------------------------------------------------------------------
// 引擎
// ---------------------------------------------------------------------------------
class CaptureEngine
{
  public:
    static CaptureEngine& Instance()
    {
        static CaptureEngine engine;
        return engine;
    }

    // =============================================================================
    // 生命周期
    // =============================================================================
    void EnsureStarted()
    {
        bool expected = false;
        if (!_threadStarted.compare_exchange_strong(expected, true))
            return;

        RefreshConfig();

        _rootDir = OwnModuleDirectory() / L"plugins" / L"cyber_engine_tweaks" / L"mods" / L"optiscaler_capture";

        std::error_code ec;
        std::filesystem::create_directories(_rootDir, ec);

        LOG_INFO("[Capture] mod dir: {}", WideToUtf8(_rootDir.wstring()));

        _stop.store(false, std::memory_order_release);
        _worker = std::thread([this] { WorkerLoop(); });
    }

    void Shutdown()
    {
        if (!_threadStarted.load())
            return;

        _capturing.store(false, std::memory_order_release);
        _stop.store(true, std::memory_order_release);

        if (_worker.joinable())
            _worker.join();

        DrainSlots(true);

        for (Slot& slot : _slots)
            ReleaseSlotResources(slot);

        _threadStarted.store(false);
        LOG_INFO("[Capture] stopped. captured={} dropped={}", _capturedFrames.load(), _droppedFrames.load());
    }

    // =============================================================================
    // 渲染线程入口
    // =============================================================================
    void HandleFrame(ID3D12GraphicsCommandList* cmdList, NVSDK_NGX_Parameter* params, const FrameContext& ctx)
    {
        if (cmdList == nullptr || params == nullptr)
            return;

        // CET START 之后的第一帧：刷新配置并开启会话
        if (_pendingStart.exchange(false, std::memory_order_acq_rel))
        {
            RefreshConfig();
            if (_enabled)
                BeginSession(ctx);
            else
                SetMessage("START ignored: [Capture] Enabled is false in OptiScaler.ini");
        }

        if (!_capturing.load(std::memory_order_acquire))
            return;

        const int stride = std::max(1, _frameStride.load(std::memory_order_relaxed));
        if ((_strideCounter++ % stride) != 0)
            return;

        const long maxFrames = _maxFrames.load(std::memory_order_relaxed);
        if (maxFrames > 0 && _capturedFrames.load(std::memory_order_relaxed) >= maxFrames)
        {
            _stopRequested.store(true, std::memory_order_release);
            return;
        }

        Slot* slot = AcquireSlot();
        if (slot == nullptr)
        {
            _droppedFrames.fetch_add(1, std::memory_order_relaxed);
            return;
        }

        ID3D12Device* device = nullptr;
        if (FAILED(cmdList->GetDevice(IID_PPV_ARGS(&device))) || device == nullptr)
        {
            ReleaseSlot(*slot);
            _droppedFrames.fetch_add(1, std::memory_order_relaxed);
            return;
        }

        if (!EnsureSlotResources(device, *slot))
        {
            device->Release();
            ReleaseSlot(*slot);
            _droppedFrames.fetch_add(1, std::memory_order_relaxed);
            return;
        }

        FillSlotMetadata(*slot, params, ctx);

        bool anyCaptured = false;
        for (int i = 0; i < kResCount; ++i)
            anyCaptured |= RecordResource(cmdList, device, *slot, i, params);

        device->Release();

        if (!anyCaptured)
        {
            ReleaseSlot(*slot);
            _droppedFrames.fetch_add(1, std::memory_order_relaxed);
            return;
        }

        // 完成标记必须录在所有 CopyTextureRegion 之后。
        // 标记值已在 AcquireSlot 内分配好（与 state=InFlight 同一临界区）。
        *reinterpret_cast<UINT64*>(slot->markerSrcMap) = slot->markerValue;
        cmdList->CopyBufferRegion(slot->markerDst, 0, slot->markerSrc, 0, kMarkerBytes);

        _capturedFrames.fetch_add(1, std::memory_order_relaxed);
    }

  private:
    CaptureEngine() = default;

    // 若游戏没有调用 NGX Shutdown，静态析构时也必须把线程收掉，
    // 否则 joinable 的 std::thread 析构会触发 std::terminate。
    ~CaptureEngine()
    {
        _stop.store(true, std::memory_order_release);

        if (_worker.joinable())
            _worker.join();
    }

    CaptureEngine(const CaptureEngine&) = delete;
    CaptureEngine& operator=(const CaptureEngine&) = delete;

    // =============================================================================
    // 配置
    // =============================================================================
    void RefreshConfig()
    {
        const Config* cfg = Config::Instance();

        _enabled = cfg->CaptureEnabled.value_or_default();
        _frameStride.store(std::max(1, cfg->CaptureFrameStride.value_or_default()), std::memory_order_relaxed);
        _maxFrames.store(static_cast<long>(cfg->CaptureMaxFrames.value_or_default()), std::memory_order_relaxed);

        _channelEnabled[RES_COLOR] = cfg->CaptureColor.value_or_default();
        _channelEnabled[RES_DEPTH] = cfg->CaptureDepth.value_or_default();
        _channelEnabled[RES_MOTION] = cfg->CaptureMotion.value_or_default();
        _channelEnabled[RES_EXPOSURE] = cfg->CaptureExposure.value_or_default();

        _compact.store(cfg->CaptureCompact.value_or_default(), std::memory_order_relaxed);

        LOG_INFO("[Capture] config: enabled={} stride={} maxFrames={} color={} depth={} motion={} exposure={} compact={}",
                 _enabled, _frameStride.load(), _maxFrames.load(), _channelEnabled[RES_COLOR],
                 _channelEnabled[RES_DEPTH], _channelEnabled[RES_MOTION], _channelEnabled[RES_EXPOSURE],
                 _compact.load());
    }

    bool IsChannelEnabled(int index) const
    {
        if (index < 0 || index >= kResCount)
            return false;

        return _channelEnabled[index];
    }

    // 取该通道源资源"被复制前"所处的状态。未配置则返回 nullopt。
    std::optional<D3D12_RESOURCE_STATES> OriginalStateFor(int index) const
    {
        const Config* cfg = Config::Instance();

        switch (index)
        {
        case RES_COLOR:
            if (cfg->ColorResourceBarrier.has_value())
                return static_cast<D3D12_RESOURCE_STATES>(cfg->ColorResourceBarrier.value());
            break;

        case RES_DEPTH:
            if (cfg->DepthResourceBarrier.has_value())
                return static_cast<D3D12_RESOURCE_STATES>(cfg->DepthResourceBarrier.value());
            break;

        case RES_MOTION:
            if (cfg->MVResourceBarrier.has_value())
                return static_cast<D3D12_RESOURCE_STATES>(cfg->MVResourceBarrier.value());
            break;

        case RES_EXPOSURE:
            if (cfg->ExposureResourceBarrier.has_value())
                return static_cast<D3D12_RESOURCE_STATES>(cfg->ExposureResourceBarrier.value());
            break;

        default:
            break;
        }

        return std::nullopt;
    }

    // =============================================================================
    // 会话
    // =============================================================================
    void BeginSession(const FrameContext& ctx)
    {
        const std::filesystem::path sessionDir = _rootDir / ("session_" + LocalStampForPath());

        std::error_code ec;
        std::filesystem::create_directories(sessionDir, ec);
        if (ec)
        {
            LOG_ERROR("[Capture] can't create session dir {}: {}", WideToUtf8(sessionDir.wstring()), ec.message());
            return;
        }

        {
            std::lock_guard<std::mutex> lock(_stateMutex);

            _sessionDir = sessionDir;
            _sessionStartedUtc = UtcTimestamp();
            _sessionWidth = ctx.renderWidth;
            _sessionHeight = ctx.renderHeight;
            _sessionTargetWidth = ctx.targetWidth;
            _sessionTargetHeight = ctx.targetHeight;
            _sessionFeatureName = ctx.featureName;
            _sessionIsHdr = ctx.isHdr;
            _lastMessage = "capturing";
        }

        _nextFrameIndex = 0;
        _strideCounter = 0;
        _capturedFrames.store(0, std::memory_order_relaxed);
        _droppedFrames.store(0, std::memory_order_relaxed);
        _bytesWritten.store(0, std::memory_order_relaxed);

        _capturing.store(true, std::memory_order_release);
        _stopRequested.store(false, std::memory_order_release);
        _sessionActive.store(true, std::memory_order_release);

        LOG_INFO("[Capture] session started -> {}", WideToUtf8(sessionDir.wstring()));
        LOG_INFO("[Capture] render {}x{} target {}x{} hdr={} stride={} maxFrames={}", ctx.renderWidth, ctx.renderHeight,
                 ctx.targetWidth, ctx.targetHeight, ctx.isHdr, _frameStride.load(), _maxFrames.load());

        WriteManifest();
    }

    void EndSession(const std::string& reason)
    {
        if (!_sessionActive.exchange(false, std::memory_order_acq_rel))
            return;

        _capturing.store(false, std::memory_order_release);

        {
            std::lock_guard<std::mutex> lock(_stateMutex);
            _lastMessage = reason;
        }

        LOG_INFO("[Capture] session stopping: {}", reason);
        WriteManifest();
    }

    // =============================================================================
    // 槽位
    // =============================================================================
    Slot* AcquireSlot()
    {
        std::lock_guard<std::mutex> lock(_slotMutex);

        for (Slot& slot : _slots)
        {
            if (slot.state == SlotState::Free)
            {
                // 标记值必须在置为 InFlight 的同一临界区内分配。
                // 否则后台线程可能读到该槽上一轮的旧标记值（旧值 == 旧标记 → 误判完成）。
                slot.markerValue = ++_markerCounter;
                slot.state = SlotState::InFlight;
                _inFlight.fetch_add(1, std::memory_order_relaxed);
                return &slot;
            }
        }

        return nullptr;
    }

    void ReleaseSlot(Slot& slot)
    {
        std::lock_guard<std::mutex> lock(_slotMutex);

        ClearSlotFrameState(slot);

        if (slot.state == SlotState::InFlight)
        {
            slot.state = SlotState::Free;
            _inFlight.fetch_sub(1, std::memory_order_relaxed);
        }
    }

    void ClearSlotFrameState(Slot& slot)
    {
        for (int i = 0; i < kResCount; ++i)
        {
            slot.res[i].captured = false;
            slot.res[i].skipped = false;
            slot.res[i].skipReason.clear();
        }
    }

    // =============================================================================
    // 资源准备
    // =============================================================================
    bool EnsureSlotResources(ID3D12Device* device, Slot& slot)
    {
        if (slot.markerSrc == nullptr || slot.markerSrcMap == nullptr)
        {
            // 半成品状态先清干净，避免留下非空但未映射的资源
            if (slot.markerSrc != nullptr)
            {
                slot.markerSrc->Release();
                slot.markerSrc = nullptr;
                slot.markerSrcMap = nullptr;
            }

            if (!CreateBuffer(device, D3D12_HEAP_TYPE_UPLOAD, kMarkerBytes, D3D12_RESOURCE_STATE_GENERIC_READ,
                              &slot.markerSrc))
                return false;

            void* mapped = nullptr;
            const D3D12_RANGE noRead = { 0, 0 };
            if (FAILED(slot.markerSrc->Map(0, &noRead, &mapped)) || mapped == nullptr)
            {
                slot.markerSrc->Release();
                slot.markerSrc = nullptr;
                return false;
            }

            slot.markerSrcMap = mapped;
            *reinterpret_cast<UINT64*>(mapped) = 0;
        }

        if (slot.markerDst == nullptr || slot.markerDstMap == nullptr)
        {
            if (slot.markerDst != nullptr)
            {
                slot.markerDst->Release();
                slot.markerDst = nullptr;
                slot.markerDstMap = nullptr;
            }

            if (!CreateBuffer(device, D3D12_HEAP_TYPE_READBACK, kMarkerBytes, D3D12_RESOURCE_STATE_COPY_DEST,
                              &slot.markerDst))
                return false;

            void* mapped = nullptr;
            const D3D12_RANGE noRead = { 0, 0 };
            if (FAILED(slot.markerDst->Map(0, &noRead, &mapped)) || mapped == nullptr)
            {
                slot.markerDst->Release();
                slot.markerDst = nullptr;
                return false;
            }

            slot.markerDstMap = mapped;
            *reinterpret_cast<UINT64*>(mapped) = 0;
        }

        return true;
    }

    bool PrepareTarget(ID3D12Device* device, ReadbackTarget& target, ID3D12Resource* source,
                       D3D12_RESOURCE_STATES originalState, std::string& reason)
    {
        const D3D12_RESOURCE_DESC desc = source->GetDesc();

        if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D)
        {
            reason = "not a Texture2D";
            return false;
        }

        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
        UINT numRows = 0;
        UINT64 rowSizeInBytes = 0;
        UINT64 totalBytes = 0;

        device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, &numRows, &rowSizeInBytes, &totalBytes);

        if (totalBytes == 0 || numRows == 0 || footprint.Footprint.RowPitch == 0)
        {
            reason = "GetCopyableFootprints returned zero";
            return false;
        }

        if (target.buffer == nullptr || target.capacity < totalBytes)
        {
            if (target.buffer != nullptr)
            {
                target.buffer->Release();
                target.buffer = nullptr;
                target.capacity = 0;
            }

            if (!CreateBuffer(device, D3D12_HEAP_TYPE_READBACK, totalBytes, D3D12_RESOURCE_STATE_COPY_DEST,
                              &target.buffer))
            {
                reason = "readback allocation failed";
                return false;
            }

            target.capacity = totalBytes;
        }

        target.footprint = footprint;
        target.rowPitch = footprint.Footprint.RowPitch;
        target.rowCount = numRows;
        target.tightRowBytes = rowSizeInBytes;
        target.totalBytes = totalBytes;
        target.format = desc.Format;
        target.originalState = originalState;
        return true;
    }

    bool RecordResource(ID3D12GraphicsCommandList* cmdList, ID3D12Device* device, Slot& slot, int index,
                        NVSDK_NGX_Parameter* params)
    {
        ReadbackTarget& target = slot.res[index];

        target.captured = false;
        target.skipped = false;
        target.skipReason.clear();

        if (!IsChannelEnabled(index))
        {
            target.skipped = true;
            target.skipReason = "disabled in config";
            return false;
        }

        ID3D12Resource* source = nullptr;
        if (params->Get(kResKeys[index], &source) != NVSDK_NGX_Result_Success)
            params->Get(kResKeys[index], reinterpret_cast<void**>(&source));

        if (source == nullptr)
        {
            // Exposure 常为 null，属正常情况
            target.skipped = true;
            target.skipReason = "null";
            return false;
        }

        const std::optional<D3D12_RESOURCE_STATES> originalState = OriginalStateFor(index);
        if (!originalState.has_value())
        {
            target.skipped = true;
            target.skipReason = "missing [Hotfix] barrier config";
            WarnOnce(index, target.skipReason);
            return false;
        }

        std::string reason;
        if (!PrepareTarget(device, target, source, originalState.value(), reason))
        {
            target.skipped = true;
            target.skipReason = reason;
            WarnOnce(index, reason);
            return false;
        }

        TransitionResource(cmdList, source, originalState.value(), D3D12_RESOURCE_STATE_COPY_SOURCE);

        D3D12_TEXTURE_COPY_LOCATION srcLocation = {};
        srcLocation.pResource = source;
        srcLocation.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        srcLocation.SubresourceIndex = 0;

        D3D12_TEXTURE_COPY_LOCATION dstLocation = {};
        dstLocation.pResource = target.buffer;
        dstLocation.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dstLocation.PlacedFootprint = target.footprint;

        cmdList->CopyTextureRegion(&dstLocation, 0, 0, 0, &srcLocation, nullptr);

        TransitionResource(cmdList, source, D3D12_RESOURCE_STATE_COPY_SOURCE, originalState.value());

        target.captured = true;
        return true;
    }

    void WarnOnce(int index, const std::string& reason)
    {
        if (_barrierWarned.find(index) != _barrierWarned.end())
            return;

        _barrierWarned.insert(index);
        LOG_WARN("[Capture] channel '{}' unavailable: {}. Set the matching [Hotfix] barrier key in OptiScaler.ini.",
                 kResNames[index], reason);
    }

    // =============================================================================
    // 后台线程
    // =============================================================================
    void WorkerLoop()
    {
        auto lastCommandPoll = std::chrono::steady_clock::now() - std::chrono::seconds(10);
        auto lastStatusWrite = std::chrono::steady_clock::now() - std::chrono::seconds(10);

        while (!_stop.load(std::memory_order_acquire))
        {
            const auto now = std::chrono::steady_clock::now();

            if (now - lastCommandPoll >= std::chrono::milliseconds(100))
            {
                lastCommandPoll = now;
                PollCommandFile();
            }

            DrainSlots(false);

            if (_stopRequested.exchange(false, std::memory_order_acq_rel))
                EndSession("stop requested");

            if (now - lastStatusWrite >= std::chrono::seconds(1))
            {
                lastStatusWrite = now;
                WriteStatusFile();
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }

    void PollCommandFile()
    {
        const std::filesystem::path commandPath = _rootDir / L"command.txt";

        HANDLE handle = CreateFileW(commandPath.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);

        if (handle == INVALID_HANDLE_VALUE)
            return;

        char buffer[64] = {};
        DWORD bytesRead = 0;
        ReadFile(handle, buffer, sizeof(buffer) - 1, &bytesRead, nullptr);
        CloseHandle(handle);

        // 立即清空，避免重复执行
        DeleteFileW(commandPath.c_str());

        if (bytesRead == 0)
            return;

        std::string command(buffer, bytesRead);

        while (!command.empty() &&
               (command.back() == '\r' || command.back() == '\n' || command.back() == ' ' || command.back() == '\t'))
            command.pop_back();

        const size_t start = command.find_first_not_of(" \t");
        command = (start == std::string::npos) ? std::string() : command.substr(start);

        std::transform(command.begin(), command.end(), command.begin(),
                       [](unsigned char c) { return static_cast<char>(std::toupper(c)); });

        if (command.empty())
            return;

        LOG_INFO("[Capture] command: {}", command);

        if (command == "START")
        {
            if (_capturing.load(std::memory_order_acquire))
            {
                SetMessage("already capturing");
            }
            else
            {
                _pendingStart.store(true, std::memory_order_release);
                SetMessage("start pending");
            }
        }
        else if (command == "STOP")
        {
            if (_capturing.load(std::memory_order_acquire))
                _stopRequested.store(true, std::memory_order_release);
            else
                SetMessage("not capturing");
        }
        else if (command == "STATUS")
        {
            WriteStatusFile();
        }
        else
        {
            SetMessage("unknown command: " + command);
        }
    }

    void SetMessage(const std::string& message)
    {
        std::lock_guard<std::mutex> lock(_stateMutex);
        _lastMessage = message;
    }

    void DrainSlots(bool force)
    {
        for (Slot& slot : _slots)
        {
            {
                std::lock_guard<std::mutex> lock(_slotMutex);

                if (slot.state != SlotState::InFlight)
                    continue;

                bool done = false;

                if (!force && slot.markerDstMap != nullptr && slot.markerValue != 0)
                {
                    const UINT64 observed = *reinterpret_cast<volatile UINT64*>(slot.markerDstMap);
                    done = observed >= slot.markerValue;
                }

                if (!done && !force)
                    continue;

                if (force && !done)
                {
                    // 强制退出：不再等待 GPU，直接丢弃，避免卡住进程退出
                    ClearSlotFrameState(slot);
                    slot.state = SlotState::Free;
                    _inFlight.fetch_sub(1, std::memory_order_relaxed);
                    continue;
                }

                slot.state = SlotState::Draining;
            }

            WriteFrameToDisk(slot);

            {
                std::lock_guard<std::mutex> lock(_slotMutex);

                ClearSlotFrameState(slot);
                slot.state = SlotState::Free;
                _inFlight.fetch_sub(1, std::memory_order_relaxed);
            }
        }
    }

    // =============================================================================
    // 落盘
    // =============================================================================
    void WriteFrameToDisk(Slot& slot)
    {
        std::filesystem::path sessionDir;

        {
            std::lock_guard<std::mutex> lock(_stateMutex);
            sessionDir = _sessionDir;
        }

        if (sessionDir.empty())
            return;

        const std::filesystem::path frameDir = sessionDir / ("frame_" + ZeroPad6(slot.frameIndex));

        std::error_code ec;
        std::filesystem::create_directories(frameDir, ec);
        if (ec)
        {
            LOG_ERROR("[Capture] create frame dir failed: {}", ec.message());
            return;
        }

        nlohmann::json frameJson;
        frameJson["frame_index"] = slot.frameIndex;
        frameJson["width"] = slot.width;
        frameJson["height"] = slot.height;
        frameJson["render_width"] = slot.width;
        frameJson["render_height"] = slot.height;
        frameJson["target_width"] = slot.targetWidth;
        frameJson["target_height"] = slot.targetHeight;
        frameJson["jitter_x"] = slot.jitterX;
        frameJson["jitter_y"] = slot.jitterY;
        frameJson["frame_time_ms"] = slot.frameTimeMs;
        frameJson["delta_time"] = slot.frameTimeMs > 0.0f ? static_cast<double>(slot.frameTimeMs) / 1000.0 : 0.0;
        frameJson["engine_frame_count"] = slot.engineFrameCount;
        frameJson["feature"] = slot.featureName;

        frameJson["is_hdr"] = slot.isHdr;
        frameJson["low_res_mv"] = slot.lowResMV;
        frameJson["jittered_mv"] = slot.jitteredMV;
        frameJson["depth_inverted"] = slot.depthInverted;
        frameJson["auto_exposure"] = slot.autoExposure;

        frameJson["motion_scale_x"] = slot.motionScaleX;
        frameJson["motion_scale_y"] = slot.motionScaleY;
        frameJson["motion_units"] = "pixels";
        frameJson["motion_direction"] = "current_to_previous";
        frameJson["motion_resolution"] = slot.lowResMV ? "render" : "target";

        UINT64 frameBytes = 0;

        for (int i = 0; i < kResCount; ++i)
        {
            const ReadbackTarget& target = slot.res[i];
            const std::string prefix = kResNames[i];

            if (!target.captured || target.buffer == nullptr)
            {
                frameJson[prefix + "_present"] = false;
                if (target.skipped && !target.skipReason.empty())
                    frameJson[prefix + "_skip_reason"] = target.skipReason;
                continue;
            }

            UINT64 written = 0;
            DXGI_FORMAT onDiskFormat = target.format;
            UINT onDiskStride = static_cast<UINT>(target.tightRowBytes);
            if (!WriteReadbackRows(i, target, frameDir / kResFileNames[i], written, onDiskFormat, onDiskStride))
            {
                frameJson[prefix + "_present"] = false;
                frameJson[prefix + "_skip_reason"] = "write failed";
                continue;
            }

            frameBytes += written;

            frameJson[prefix + "_present"] = true;
            frameJson[prefix + "_format"] = FormatName(onDiskFormat);
            frameJson[prefix + "_format_value"] = static_cast<int>(onDiskFormat);
            frameJson[prefix + "_source_format"] = FormatName(target.format);
            frameJson[prefix + "_row_pitch"] = target.rowPitch;
            frameJson[prefix + "_tight_stride"] = onDiskStride;
            frameJson[prefix + "_rows"] = target.rowCount;
            frameJson[prefix + "_bytes"] = written;
            frameJson[prefix + "_layout"] = "tight";
            frameJson[prefix + "_file"] = kResFileNames[i];
        }

        frameJson["total_bytes"] = frameBytes;

        std::ofstream meta(frameDir / "frame.json", std::ios::binary | std::ios::trunc);
        if (meta.is_open())
        {
            const std::string dumped = frameJson.dump(2);
            meta.write(dumped.data(), static_cast<std::streamsize>(dumped.size()));
            meta.close();
        }

        _bytesWritten.fetch_add(frameBytes, std::memory_order_relaxed);
    }

    // 把回读缓冲按 tight 行写盘。index 决定通道的压缩策略（Compact=true 且源为 RGBA16F 时）：
    //   · motion → 只写 RG 两通道（DXGI_FORMAT_R16G16_FLOAT，4 B/px），无损
    //   · color  → 打包成 DXGI_FORMAT_R11G11B10_FLOAT（4 B/px），保留 HDR
    // 其余情况原样写出。onDiskFormat / onDiskStride 回报实际落盘格式，写进 frame.json。
    bool WriteReadbackRows(int index, const ReadbackTarget& target, const std::filesystem::path& path,
                           UINT64& written, DXGI_FORMAT& onDiskFormat, UINT& onDiskStride)
    {
        void* mapped = nullptr;
        const D3D12_RANGE readRange = { 0, static_cast<SIZE_T>(target.totalBytes) };

        if (FAILED(target.buffer->Map(0, &readRange, &mapped)) || mapped == nullptr)
        {
            LOG_ERROR("[Capture] Map failed for {}", WideToUtf8(path.wstring()));
            return false;
        }

        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        if (!out.is_open())
        {
            const D3D12_RANGE noWrite = { 0, 0 };
            target.buffer->Unmap(0, &noWrite);
            LOG_ERROR("[Capture] can't open {}", WideToUtf8(path.wstring()));
            return false;
        }

        const auto* base = static_cast<const uint8_t*>(mapped);
        const UINT64 tight = target.tightRowBytes > 0 ? target.tightRowBytes : target.rowPitch;

        const bool compact = _compact.load(std::memory_order_relaxed);
        const bool srcRGBA16 = target.format == DXGI_FORMAT_R16G16B16A16_FLOAT;
        const bool compactMotion = compact && srcRGBA16 && index == RES_MOTION;
        const bool compactColor = compact && srcRGBA16 && index == RES_COLOR;

        if (compactMotion || compactColor)
        {
            // 源是 RGBA16F：8 B/px，故每行像素数 = tight / 8
            const size_t pixelsPerRow = static_cast<size_t>(tight) / 8u;
            std::vector<uint32_t> line(pixelsPerRow);

            for (UINT row = 0; row < target.rowCount; ++row)
            {
                const auto* src = reinterpret_cast<const uint16_t*>(base + static_cast<UINT64>(row) * target.rowPitch);

                if (compactMotion)
                {
                    for (size_t x = 0; x < pixelsPerRow; ++x)
                        line[x] = static_cast<uint32_t>(src[x * 4 + 0]) | (static_cast<uint32_t>(src[x * 4 + 1]) << 16);
                }
                else
                {
                    for (size_t x = 0; x < pixelsPerRow; ++x)
                        line[x] = PackR11G11B10(&src[x * 4]);
                }

                out.write(reinterpret_cast<const char*>(line.data()),
                          static_cast<std::streamsize>(pixelsPerRow * sizeof(uint32_t)));
            }

            out.close();

            const D3D12_RANGE noWrite = { 0, 0 };
            target.buffer->Unmap(0, &noWrite);

            onDiskStride = static_cast<UINT>(pixelsPerRow * sizeof(uint32_t));
            onDiskFormat = compactMotion ? DXGI_FORMAT_R16G16_FLOAT : DXGI_FORMAT_R11G11B10_FLOAT;
            written = static_cast<UINT64>(target.rowCount) * onDiskStride;
            return true;
        }

        for (UINT row = 0; row < target.rowCount; ++row)
        {
            const auto* rowPtr = base + static_cast<UINT64>(row) * target.rowPitch;
            out.write(reinterpret_cast<const char*>(rowPtr), static_cast<std::streamsize>(tight));
        }

        out.close();

        const D3D12_RANGE noWrite = { 0, 0 };
        target.buffer->Unmap(0, &noWrite);

        onDiskFormat = target.format;
        onDiskStride = static_cast<UINT>(tight);
        written = static_cast<UINT64>(target.rowCount) * tight;
        return true;
    }

    void WriteManifest()
    {
        std::filesystem::path sessionDir;

        nlohmann::json manifest;
        // 2 = Compact 落盘契约（*_format 记落盘格式，*_source_format 记源格式）
        manifest["capture_version"] = 2;

        {
            std::lock_guard<std::mutex> lock(_stateMutex);

            sessionDir = _sessionDir;
            if (sessionDir.empty())
                return;

            manifest["created_utc"] = _sessionStartedUtc;
            manifest["width"] = _sessionWidth;
            manifest["height"] = _sessionHeight;
            manifest["render_width"] = _sessionWidth;
            manifest["render_height"] = _sessionHeight;
            manifest["target_width"] = _sessionTargetWidth;
            manifest["target_height"] = _sessionTargetHeight;
            manifest["feature"] = _sessionFeatureName;
            manifest["is_hdr"] = _sessionIsHdr;
            manifest["message"] = _lastMessage;
        }

        manifest["frame_stride"] = _frameStride.load();
        manifest["max_frames"] = _maxFrames.load();
        manifest["captured_frames"] = _capturedFrames.load();
        manifest["dropped_frames"] = _droppedFrames.load();
        manifest["bytes_written"] = _bytesWritten.load();
        manifest["capturing"] = _capturing.load();
        manifest["motion_direction"] = "current_to_previous";
        manifest["motion_units"] = "pixels";
        manifest["frame_layout"] = "frame_%06d/{color,depth,motion,exposure}.bin + frame.json";

        std::ofstream out(sessionDir / "manifest.json", std::ios::binary | std::ios::trunc);
        if (out.is_open())
        {
            const std::string dumped = manifest.dump(2);
            out.write(dumped.data(), static_cast<std::streamsize>(dumped.size()));
            out.close();
        }
    }

    void WriteStatusFile()
    {
        if (_rootDir.empty())
            return;

        nlohmann::json status;

        {
            std::lock_guard<std::mutex> lock(_stateMutex);

            status["session_dir"] = WideToUtf8(_sessionDir.wstring());
            status["message"] = _lastMessage;
            status["width"] = _sessionWidth;
            status["height"] = _sessionHeight;
        }

        status["state"] = _capturing.load() ? "capturing" : (_sessionActive.load() ? "idle" : "stopped");
        status["captured_frames"] = _capturedFrames.load();
        status["dropped_frames"] = _droppedFrames.load();
        status["in_flight"] = _inFlight.load();
        status["bytes_written"] = _bytesWritten.load();
        status["frame_stride"] = _frameStride.load();
        status["max_frames"] = _maxFrames.load();

        std::ofstream out(_rootDir / L"status.json", std::ios::binary | std::ios::trunc);
        if (out.is_open())
        {
            const std::string dumped = status.dump(2);
            out.write(dumped.data(), static_cast<std::streamsize>(dumped.size()));
            out.close();
        }
    }

    void FillSlotMetadata(Slot& slot, NVSDK_NGX_Parameter* params, const FrameContext& ctx)
    {
        slot.frameIndex = _nextFrameIndex++;
        slot.engineFrameCount = ctx.engineFrameCount;
        slot.width = ctx.renderWidth;
        slot.height = ctx.renderHeight;
        slot.targetWidth = ctx.targetWidth;
        slot.targetHeight = ctx.targetHeight;
        slot.isHdr = ctx.isHdr;
        slot.lowResMV = ctx.lowResMV;
        slot.jitteredMV = ctx.jitteredMV;
        slot.depthInverted = ctx.depthInverted;
        slot.autoExposure = ctx.autoExposure;
        slot.featureName = ctx.featureName;

        float value = 0.0f;
        slot.jitterX =
            params->Get(NVSDK_NGX_Parameter_Jitter_Offset_X, &value) == NVSDK_NGX_Result_Success ? value : 0.0f;
        value = 0.0f;
        slot.jitterY =
            params->Get(NVSDK_NGX_Parameter_Jitter_Offset_Y, &value) == NVSDK_NGX_Result_Success ? value : 0.0f;

        value = 1.0f;
        slot.motionScaleX =
            params->Get(NVSDK_NGX_Parameter_MV_Scale_X, &value) == NVSDK_NGX_Result_Success ? value : 1.0f;
        value = 1.0f;
        slot.motionScaleY =
            params->Get(NVSDK_NGX_Parameter_MV_Scale_Y, &value) == NVSDK_NGX_Result_Success ? value : 1.0f;

        value = 0.0f;
        slot.frameTimeMs =
            params->Get(NVSDK_NGX_Parameter_FrameTimeDeltaInMsec, &value) == NVSDK_NGX_Result_Success ? value : 0.0f;
    }

    void ReleaseSlotResources(Slot& slot)
    {
        for (int i = 0; i < kResCount; ++i)
        {
            if (slot.res[i].buffer != nullptr)
            {
                slot.res[i].buffer->Release();
                slot.res[i].buffer = nullptr;
                slot.res[i].capacity = 0;
            }
        }

        if (slot.markerSrc != nullptr)
        {
            slot.markerSrc->Unmap(0, nullptr);
            slot.markerSrc->Release();
            slot.markerSrc = nullptr;
            slot.markerSrcMap = nullptr;
        }

        if (slot.markerDst != nullptr)
        {
            slot.markerDst->Unmap(0, nullptr);
            slot.markerDst->Release();
            slot.markerDst = nullptr;
            slot.markerDstMap = nullptr;
        }
    }

    // =============================================================================
    // 状态
    // =============================================================================
    bool _enabled = false;
    std::atomic<bool> _compact { true };
    bool _channelEnabled[kResCount] = { true, true, true, false };

    std::atomic<int> _frameStride { 2 };
    std::atomic<long> _maxFrames { 0 };

    std::filesystem::path _rootDir;
    std::filesystem::path _sessionDir;
    std::string _sessionStartedUtc;

    unsigned int _sessionWidth = 0;
    unsigned int _sessionHeight = 0;
    unsigned int _sessionTargetWidth = 0;
    unsigned int _sessionTargetHeight = 0;
    bool _sessionIsHdr = false;
    std::string _sessionFeatureName;
    std::string _lastMessage;

    std::atomic<bool> _capturing { false };
    std::atomic<bool> _pendingStart { false };
    std::atomic<bool> _stopRequested { false };
    std::atomic<bool> _sessionActive { false };
    std::atomic<bool> _threadStarted { false };
    std::atomic<bool> _stop { false };

    std::atomic<long> _capturedFrames { 0 };
    std::atomic<long> _droppedFrames { 0 };
    std::atomic<long> _inFlight { 0 };
    std::atomic<UINT64> _bytesWritten { 0 };
    std::atomic<UINT64> _markerCounter { 0 };

    long _nextFrameIndex = 0;
    int _strideCounter = 0;

    Slot _slots[kRingSize];
    std::mutex _slotMutex;
    std::mutex _stateMutex;

    std::set<int> _barrierWarned;

    std::thread _worker;
};
} // namespace

// =====================================================================================
// 公共接口
// =====================================================================================
bool IsEnabled() { return Config::Instance()->CaptureEnabled.value_or_default(); }

void OnEvaluate(ID3D12GraphicsCommandList* cmdList, NVSDK_NGX_Parameter* params, const FrameContext& ctx)
{
    if (!IsEnabled())
        return;

    CaptureEngine& engine = CaptureEngine::Instance();
    engine.EnsureStarted();
    engine.HandleFrame(cmdList, params, ctx);
}

void Shutdown() { CaptureEngine::Instance().Shutdown(); }
} // namespace DlssCapture
