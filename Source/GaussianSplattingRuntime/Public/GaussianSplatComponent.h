#pragma once

#include "CoreMinimal.h"
#include "Components/PrimitiveComponent.h"
#include "GaussianSplatComponent.generated.h"

class UGaussianSplatAsset;
class UGaussianSplatPagedAsset;
class UTexture2D;
struct FPropertyChangedEvent;

// 这个枚举控制 Component 走哪条“预览/绘制路径”：
// - Points / Boxes：走传统 FPrimitiveSceneProxy，在主渲染流程里以调试几何的方式画出来。
// - Billboards：不创建 SceneProxy，而是改走 SceneViewExtension + 后处理 Pass，这才是更接近 3DGS 的路径。
UENUM(BlueprintType)
enum class EGaussianPreviewRenderMode : uint8
{
    Points UMETA(DisplayName = "Points"),
    Billboards UMETA(DisplayName = "Gaussian Billboards"),
};

UCLASS(ClassGroup = Rendering, meta = (BlueprintSpawnableComponent))
class GAUSSIANSPLATTINGRUNTIME_API UGaussianSplatComponent : public UPrimitiveComponent
{
    GENERATED_BODY()

public:
    // 指向真正存放高斯数据的 Asset。
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Gaussian Splat")
    TObjectPtr<UGaussianSplatAsset> Asset;

    // Fix 5: the same capture stored as packed pages, drawn from the process-wide
    // pool instead of from buffers of its own. Used only while
    // r.GaussianSplat.PagedAssets is 1, and only when Asset above is not set, so a
    // component can carry both through a migration and switch with one CVar.
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Gaussian Splat")
    TObjectPtr<UGaussianSplatPagedAsset> PagedAsset;

    // 密度缩放。实现上通过“抽样步长 Stride”实现，而不是随机删点。
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Gaussian Splat", meta = (ClampMin = "0.0"))
    float DensityScale = 1.0f;

    // 全局 alpha 乘子，用来统一调弱/调强整团高斯的可见度。
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Gaussian Splat", meta = (ClampMin = "0.0"))
    float OpacityScale = 1.0f;

    // 预览和 billboard shader 都会用到的基础尺寸参数。
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Gaussian Splat|Preview", meta = (ClampMin = "0.1", ClampMax = "32.0"))
    float PointSize = 2.0f;

    // SceneProxy 路径下是否按深度排序。透明点精灵通常远到近更稳定。
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Gaussian Splat|Preview")
    bool bDepthSort = true;

    // SceneProxy 路径下是否先做视锥裁剪。
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Gaussian Splat|Preview")
    bool bFrustumCull = true;

    // 防止调试绘制时把过多点一次性塞给 CPU 侧 PDI。
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Gaussian Splat|Preview", meta = (ClampMin = "1000", ClampMax = "100000000"))
    int32 MaxRenderPoints = 250000;

    // Fix 5 Step 4: a ceiling in METRES on this asset's full-detail distance, or 0 for none.
    //
    // d_full comes from the asset's own median splat size, so a drone capture reaches full detail far further out
    // than a street one: Vuores' 180 mm splats give d_full 280 m at PointSize 3, against Uno's 13 mm. That makes a
    // drone district ask for ~34M splats where a street district asks for ~20M, for detail nobody is close enough
    // to see. Capping d_full thins it at distance without touching PointSize, so the splats keep their SIZE and
    // only their density falls.
    //
    // It is applied to HalfFull BEFORE the budget multiplier, in both places that compute it -- the per-view
    // selection and the settle gate. Capping one alone either streams pages nothing will draw or selects pages the
    // gate never fetched.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Gaussian Splat|Preview", meta = (ClampMin = "0.0"))
    float LodMaxFullDistance = 0.0f;

    // 选择具体显示方式。Billboards 是运行时的主要目标模式。
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Gaussian Splat|Preview")
    EGaussianPreviewRenderMode PreviewRenderMode = EGaussianPreviewRenderMode::Billboards;

    // 决定是否为这个 Primitive 创建 SceneProxy。
    // 注意：Billboards 模式返回 nullptr，意味着不会走传统 Primitive 绘制。
    virtual FPrimitiveSceneProxy* CreateSceneProxy() override;

    // UE 查询这个 Primitive 的包围体时会走到这里。
    virtual FBoxSphereBounds CalcBounds(const FTransform& LocalToWorld) const override;

    // 注册到世界时确保默认纹理存在，并通知渲染线程重建状态。
    virtual void OnRegister() override;
    virtual void OnUnregister() override;

    // 当前没有额外动态数据上传，但保留入口方便后续扩展。
    virtual void SendRenderDynamicData_Concurrent() override;

#if WITH_EDITOR
    // 编辑器属性变化后刷新渲染状态。
    virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif

    // 蓝图友好的 Asset 设置接口。
    UFUNCTION(BlueprintCallable, Category = "Gaussian Splat")
    void SetGaussianAsset(UGaussianSplatAsset* InAsset);

    UFUNCTION(BlueprintCallable, Category = "Gaussian Splat|Preview")
    void SetPreviewRenderMode(EGaussianPreviewRenderMode InPreviewRenderMode);

private:
    void SyncWorldSubsystemRegistration();
};
