#pragma once

// DLSS 输入捕获层（离线 DLSS 5 转换管线 · 采集端）
//
// 职责：在 DLSS 超分被求值之前，把神经渲染所需的原始 GPU 资源只读回读并落盘。
//      不做任何转换、不生成、不干预游戏内容。
//
// 默认关闭：仅当 OptiScaler.ini 的 [Capture] Enabled=true 且收到 CET 的 START 命令后才激活。
// 未激活时 OnEvaluate 只做一次布尔判断，对正常 DLSS 超采样路径零影响。

#include <d3d12.h>
#include <nvsdk_ngx.h>
#include <nvsdk_ngx_defs.h>

#include <string>

namespace DlssCapture
{
    // 每帧从 IFeature 提取的上下文。调用方（NVNGX_DLSS_Dx12.cpp）负责填充，
    // 这样本模块不需要依赖 upscalers/IFeature_Dx12.h。
    struct FrameContext
    {
        unsigned int renderWidth = 0;
        unsigned int renderHeight = 0;
        unsigned int targetWidth = 0;
        unsigned int targetHeight = 0;
        long engineFrameCount = 0;

        bool isHdr = false;
        bool lowResMV = false;
        bool jitteredMV = false;
        bool depthInverted = false;
        bool autoExposure = false;

        std::string featureName;
    };

    // [Capture] Enabled 为 true 时才返回 true
    bool IsEnabled();

    // 在 feature->Evaluate() 之前调用。InCmdList 是游戏正在录制的主命令列表。
    // 未激活捕获时立即返回。
    void OnEvaluate(ID3D12GraphicsCommandList* cmdList, NVSDK_NGX_Parameter* params, const FrameContext& ctx);

    // NGX Shutdown 时调用，停止后台线程并回收资源。
    void Shutdown();
}
