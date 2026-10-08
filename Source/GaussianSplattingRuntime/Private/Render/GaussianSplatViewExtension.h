#pragma once

#include "PostProcess/PostProcessMaterialInputs.h"
#include "Render/GaussianSplatPasses.h"
#include "SceneView.h"           // ISceneViewFamilyExtentionData; SceneViewExtension.h does not pull it in
#include "SceneViewExtension.h"
#include "ScreenPass.h"

// SceneViewExtension 是 UE 允许插件插入 ViewFamily / PostProcess 阶段逻辑的标准接口。
// 这个插件用它来实现“billboard 模式不走 Primitive，而走后处理合成”的主渲染路径。
// Fix 5 Step 3 (plan D7, review M2): one frame's batches, owned by the view family
// they were snapshotted for.
//
// Why not a member and a lock, which is what this used to be: the game thread runs
// ahead of the render thread, so a single overwritten array is not necessarily the
// one a view draws with. For CARLA's six cameras that was invisible -- they are not
// deferred captures (SceneCaptureSensor.cpp:98 sets bCaptureEveryFrame false and
// calls CaptureScene() explicitly), so their render commands are FIFO against their
// own snapshots. The editor is a different story: the main viewport path calls every
// extension's BeginRenderViewFamily (SceneRendering.cpp:5666-5669), THEN renders the
// deferred every-frame captures (:5692), each enqueuing its own capture command, and
// only then enqueues the main family's draw (:5713) -- so a single slot hands the
// main view the last capture's snapshot.
//
// Every splat actor is static today, so all of this was latent. It stops being latent
// the moment one moves, which is why D9 wants it fixed BEFORE a moving splat actor,
// not after.
//
// ISceneViewFamilyExtentionData is the engine's own slot for exactly this
// (SceneView.h:1992-1997, :2399-2427). The family's copy constructor is defaulted
// (SceneView.cpp:3046), so the shared array rides into the renderer's copy, and
// FViewInfo::Family points at that copy -- which is what View.Family is inside the
// post-process callback.
class FGaussianSplatFamilyData final : public ISceneViewFamilyExtentionData
{
public:
    inline static const TCHAR* const GSubclassIdentifier = TEXT("FGaussianSplatFamilyData");
    virtual const TCHAR* GetSubclassIdentifier() const override { return GSubclassIdentifier; }

    TArray<FGaussianSplatRenderBatch> Batches;
};

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
    //
    // Fix 5 Step 3 (review M2): the snapshot is stored ON THE VIEW FAMILY, so each
    // family draws with the batches snapshotted for IT.
    void BuildPointSnapshot_GameThread(const UWorld* TargetWorld, FSceneViewFamily& InViewFamily);

    // 真正执行 RDG 绘制和合成的后处理回调。
    FScreenPassTexture PostProcessPass_RenderThread(FRDGBuilder& GraphBuilder, const FSceneView& View, const FPostProcessMaterialInputs& Inputs);
};
