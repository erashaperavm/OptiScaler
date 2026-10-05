-- ============================================================================
-- optiscaler_capture · CET 控制端
--
-- 与 OptiScaler 的 DLSS 输入捕获层通过「文件轮询」通信。
-- CET 是 Lua 环境，OptiScaler 是 C++ DX12 层，两者无法直接调用，
-- 因此在固定目录里用两个文件握手。
--
-- 通信目录（相对游戏 bin\x64）：
--   plugins/cyber_engine_tweaks/mods/optiscaler_capture/
--     ├── command.txt   ← 本脚本写入 START / STOP / STATUS，OptiScaler 读完即删
--     └── status.json   ← OptiScaler 写入，本脚本读取
--
-- 捕获输出目录：
--   plugins/cyber_engine_tweaks/mods/optiscaler_capture/capture/session_<时间戳>/
--
-- 前置条件：
--   1. OptiScaler.ini 的 [Capture] Enabled 设为 true
--   2. [Hotfix] 的 ColorResourceBarrier / DepthResourceBarrier /
--      MotionVectorResourceBarrier / ExposureResourceBarrier 已按本游戏设置好
--      （捕获层需要知道源资源被复制前所处的状态，未配置的通道会被跳过）
--
-- 控制台命令：
--   OptiCaptureStart   开始捕获
--   OptiCaptureStop    停止捕获
--   OptiCaptureStatus  查询一次状态
--   OptiCaptureWatch   开关「捕获中每 5 秒自动打印状态」
-- ============================================================================

local captureDir  = "plugins/cyber_engine_tweaks/mods/optiscaler_capture/"
local commandFile = captureDir .. "command.txt"
local statusFile  = captureDir .. "status.json"

local watchEnabled = false
local watchTimer = 0.0
local WATCH_INTERVAL = 5.0

-- ---------------------------------------------------------------------------
-- 底层：发命令 / 读状态
-- ---------------------------------------------------------------------------
local function sendCommand(cmd)
    local f = io.open(commandFile, "w")
    if not f then
        print("[OptiScaler Capture] 无法写入 " .. commandFile ..
              " — 请确认 OptiScaler.ini 的 [Capture] Enabled=true")
        return false
    end
    f:write(cmd)
    f:close()
    return true
end

local function readStatus()
    local f = io.open(statusFile, "r")
    if not f then
        return nil
    end
    local content = f:read("*a")
    f:close()
    return content
end

-- ---------------------------------------------------------------------------
-- 极简 JSON 取值（只处理本模块自己写出的扁平结构，避免引入依赖）
-- ---------------------------------------------------------------------------
local function jsonField(raw, name)
    if not raw then
        return nil
    end
    local str = raw:match('"' .. name .. '"%s*:%s*"([^"]*)"')
    if str then
        return str
    end
    local num = raw:match('"' .. name .. '"%s*:%s*([%-%d%.]+)')
    if num then
        return tonumber(num)
    end
    local boolean = raw:match('"' .. name .. '"%s*:%s*(%a+)')
    if boolean then
        return boolean
    end
    return nil
end

local function formatBytes(value)
    local n = tonumber(value) or 0
    local units = { "B", "KiB", "MiB", "GiB", "TiB" }
    local index = 1
    while n >= 1024 and index < #units do
        n = n / 1024
        index = index + 1
    end
    return string.format("%.2f %s", n, units[index])
end

local function printStatus()
    local raw = readStatus()
    if not raw then
        print("[OptiScaler Capture] 未找到 status.json — OptiScaler 可能未启用 [Capture]")
        return
    end

    local state = jsonField(raw, "state") or "?"
    local captured = jsonField(raw, "captured_frames") or 0
    local dropped = jsonField(raw, "dropped_frames") or 0
    local inflight = jsonField(raw, "in_flight") or 0
    local bytes = jsonField(raw, "bytes_written") or 0
    local width = jsonField(raw, "width") or 0
    local height = jsonField(raw, "height") or 0
    local session = jsonField(raw, "session_dir")
    local message = jsonField(raw, "message")

    print(string.format(
        "[OptiScaler Capture] state=%s | frames=%s dropped=%s inflight=%s | %s | %sx%s",
        tostring(state), tostring(captured), tostring(dropped), tostring(inflight),
        formatBytes(bytes), tostring(width), tostring(height)))

    if session and session ~= "" then
        print("[OptiScaler Capture] 输出目录: " .. session)
    end
    if message and message ~= "" then
        print("[OptiScaler Capture] message: " .. message)
    end
end

-- ---------------------------------------------------------------------------
-- 命令注册
-- ---------------------------------------------------------------------------
registerForEvent("onInit", function()
    registerConsoleCommand("OptiCaptureStart", "开始 DLSS 输入资源捕获", function()
        if sendCommand("START") then
            print("[OptiScaler Capture] 已发送 START，将在下一个 DLSS 求值帧开始采集")
        end
    end)

    registerConsoleCommand("OptiCaptureStop", "停止 DLSS 输入资源捕获", function()
        if sendCommand("STOP") then
            print("[OptiScaler Capture] 已发送 STOP")
        end
    end)

    registerConsoleCommand("OptiCaptureStatus", "查询 DLSS 输入捕获状态", function()
        sendCommand("STATUS")
        printStatus()
    end)

    registerConsoleCommand("OptiCaptureWatch", "开关捕获中的自动状态打印（每 5 秒）", function()
        watchEnabled = not watchEnabled
        watchTimer = 0.0
        if watchEnabled then
            print("[OptiScaler Capture] 自动状态打印已开启")
        else
            print("[OptiScaler Capture] 自动状态打印已关闭")
        end
    end)

    print("[OptiScaler Capture] CET 端已加载。命令：OptiCaptureStart / Stop / Status / Watch")
end)

-- ---------------------------------------------------------------------------
-- 可选的自动状态轮询
-- ---------------------------------------------------------------------------
registerForEvent("onUpdate", function(deltaTime)
    if not watchEnabled then
        return
    end

    watchTimer = watchTimer + deltaTime
    if watchTimer < WATCH_INTERVAL then
        return
    end

    watchTimer = 0.0
    printStatus()
end)
