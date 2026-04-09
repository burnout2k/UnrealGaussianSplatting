#include "GaussianSplatComponent.h"

#include "GaussianSplatAsset.h"
#include "GaussianSplatWorldSubsystem.h"
#include "Render/GaussianSplatSceneProxy.h"

void UGaussianSplatComponent::SyncWorldSubsystemRegistration()
{
    // 这个函数负责把“当前组件是否应该参与 billboard 渲染”同步到当前 World 的 subsystem。
    // 现在的规则很简单：
    // 1. 组件必须已经注册进世界；
    // 2. PreviewRenderMode 必须是 Billboards。
    UWorld* World = GetWorld();
    if (World == nullptr)
    {
        return;
    }

    UGaussianSplatWorldSubsystem* WorldSubsystem = World->GetSubsystem<UGaussianSplatWorldSubsystem>();
    if (WorldSubsystem == nullptr)
    {
        return;
    }

    if (IsRegistered() && PreviewRenderMode == EGaussianPreviewRenderMode::Billboards)
    {
        WorldSubsystem->RegisterComponent(this);
        return;
    }

    WorldSubsystem->UnregisterComponent(this);
}

FPrimitiveSceneProxy* UGaussianSplatComponent::CreateSceneProxy()
{
    // 没有 Asset 就没有可绘制内容。
    if (!Asset)
    {
        return nullptr;
    }

    // Billboards 模式不走传统 Primitive 渲染路径。
    // 这种情况下会由 SceneViewExtension 在后处理阶段接管绘制，所以这里返回 nullptr。
    if (PreviewRenderMode == EGaussianPreviewRenderMode::Billboards)
    {
        return nullptr;
    }

    // Points / Boxes 这类调试模式仍然走标准 SceneProxy。
    return new FGaussianSplatSceneProxy(this);
}

FBoxSphereBounds UGaussianSplatComponent::CalcBounds(const FTransform& LocalToWorld) const
{
    // 没有 Asset 时返回空包围体。
    if (!Asset)
    {
        return FBoxSphereBounds(EForceInit::ForceInitToZero);
    }

    // 基础包围体直接来自 Asset 预先计算好的局部 Bounds，再乘组件变换。
    FBoxSphereBounds PrimitiveBounds = Asset->Bounds.TransformBy(LocalToWorld);
    if (PreviewRenderMode != EGaussianPreviewRenderMode::Billboards)
    {
        // 调试绘制模式下会额外画点精灵/盒子，给包围体留一点安全边界，
        // 避免视觉上还在范围内，但整体 primitive 已经被裁掉。
        PrimitiveBounds = PrimitiveBounds.ExpandBy(FMath::Max(1.0f, PointSize));
    }

    return PrimitiveBounds;
}

void UGaussianSplatComponent::OnRegister()
{
    Super::OnRegister();

    // 组件进世界后，立即同步自己在 billboard 注册表中的状态。
    SyncWorldSubsystemRegistration();

    // 通知渲染线程，这个组件的渲染状态可能需要重建。
    MarkRenderStateDirty();
}

void UGaussianSplatComponent::OnUnregister()
{
    // 反注册前先把自己从 subsystem 里移掉，避免留下无效引用。
    SyncWorldSubsystemRegistration();
    Super::OnUnregister();
}

void UGaussianSplatComponent::SendRenderDynamicData_Concurrent()
{
    // 当前还没有额外的逐帧动态数据上传逻辑。
    // 这个入口保留着，后面如果要做 streaming / LOD / runtime 参数更新，可以从这里扩展。
    Super::SendRenderDynamicData_Concurrent();
}

void UGaussianSplatComponent::SetGaussianAsset(UGaussianSplatAsset* InAsset)
{
    // 替换 Asset 后，下一帧渲染需要重新读取资源。
    Asset = InAsset;
    MarkRenderStateDirty();
}

void UGaussianSplatComponent::SetPreviewRenderMode(EGaussianPreviewRenderMode InPreviewRenderMode)
{
    // 统一的运行时模式切换入口。
    // 外部如果要从蓝图或 C++ 改 PreviewRenderMode，应该走这个函数，而不是直接写字段。
    if (PreviewRenderMode == InPreviewRenderMode)
    {
        return;
    }

    PreviewRenderMode = InPreviewRenderMode;

    // 模式变化会影响这个组件是否应该进入 billboard 注册表。
    SyncWorldSubsystemRegistration();
    MarkRenderStateDirty();
}

#if WITH_EDITOR
void UGaussianSplatComponent::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
    Super::PostEditChangeProperty(PropertyChangedEvent);

    static const FName PreviewRenderModeName = GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, PreviewRenderMode);
    if (PropertyChangedEvent.GetPropertyName() == PreviewRenderModeName)
    {
        // 编辑器里从 Details 面板直接改 PreviewRenderMode 时，不会走自定义 setter，
        // 所以要在这里补一次 subsystem 同步。
        SyncWorldSubsystemRegistration();
    }

    MarkRenderStateDirty();
}
#endif
