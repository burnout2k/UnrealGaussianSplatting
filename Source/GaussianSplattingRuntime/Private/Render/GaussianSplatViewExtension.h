#pragma once

#include "PostProcess/PostProcessMaterialInputs.h"
#include "Render/GaussianSplatPasses.h"
#include "SceneViewExtension.h"
#include "ScreenPass.h"

// SceneViewExtension 是 UE 允许插件插入 ViewFamily / PostProcess 阶段逻辑的标准接口。
// 这个插件用它来实现“billboard 模式不走 Primitive，而走后处理合成”的主渲染路径。
class FGaussianSplatViewExtension final : public FSceneViewExtensionBase
{
public:
    FGaussianSplatViewExtension(const FAutoRegister& AutoRegister);

    // 只要返回 true，UE 每帧都会回调这个扩展。
    virtual bool IsActiveThisFrame_Internal(const FSceneViewExtensionContext& Context) const override;
    virtual void SetupViewFamily(FSceneViewFamily& InViewFamily) override;
    virtual void SetupView(FSceneViewFamily& InViewFamily, FSceneView& InView) override;

    // 游戏线程入口：在真正开始渲染一个 ViewFamily 前，快照当前世界里需要参与
    // billboard 渲染的 Gaussian 组件。
    virtual void BeginRenderViewFamily(FSceneViewFamily& InViewFamily) override;

    // 渲染线程入口：当前实现本身不直接追加绘制，真正的后处理注册发生在
    // SubscribeToPostProcessingPass 里。
    virtual void PreRenderViewFamily_RenderThread(FRDGBuilder& GraphBuilder, FSceneViewFamily& InViewFamily) override;

    // 把我们的后处理回调插入 UE 的后处理链。
    virtual void SubscribeToPostProcessingPass(EPostProcessingPass PassId, const FSceneView& View, FAfterPassCallbackDelegateArray& InOutPassCallbacks, bool bIsPassEnabled) override;

private:
    // 从 UGaussianSplatComponent 抽取线程安全的只读快照，供渲染线程使用。
    // 这里不再全局扫描 UObject，而是只读取当前 World 对应 subsystem 里
    // 已注册的 billboard 组件。
    void BuildPointSnapshot_GameThread(const UWorld* TargetWorld);

    // 真正执行 RDG 绘制和合成的后处理回调。
    FScreenPassTexture PostProcessPass_RenderThread(FRDGBuilder& GraphBuilder, const FSceneView& View, const FPostProcessMaterialInputs& Inputs);

    // 游戏线程写、渲染线程读，需要读写锁保护。
    FRWLock CachedPointsLock;
    TArray<FGaussianSplatRenderBatch> CachedPoints;
};
