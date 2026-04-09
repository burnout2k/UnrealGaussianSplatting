#pragma once

#include "CoreMinimal.h"
#include "GaussianSplatAsset.generated.h"

class FGaussianSplatRenderResources;

// 这个 Asset 是插件里最核心的数据容器：
// 1. 编辑器导入 PLY 后，原始 Gaussian 数据先落在这里的 CPU 数组里。
// 2. 运行时它再把这些数组转换成 GPU StructuredBuffer，供计算着色器和光栅化着色器读取。
// 3. Component / SceneProxy / ViewExtension 都只通过它拿数据，不直接关心文件格式。
UCLASS(BlueprintType)
class GAUSSIANSPLATTINGRUNTIME_API UGaussianSplatAsset : public UObject
{
    GENERATED_BODY()

public:
    // 每个高斯中心的位置。这里存的是 Asset 本地空间坐标，真正绘制前还会乘 Component 的 LocalToWorld。
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Gaussian Splat")
    TArray<FVector3f> Positions;

    // 每个高斯的旋转，用四元数表示椭球主轴方向。
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Gaussian Splat")
    TArray<FQuat4f> Rotations;

    // 每个高斯的局部尺度。插件把高斯视作各向异性椭球，三个分量对应三个主轴半径。
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Gaussian Splat")
    TArray<FVector3f> Scales;

    // RGB + Alpha。这里的 Alpha 不是最终屏幕 alpha，而是 splat 自身的基础不透明度。
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Gaussian Splat")
    TArray<FVector4f> ColorsOpacity;

    // 球谐系数，当前运行时着色器按每个高斯 15 组 float3 的布局来读取。
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Gaussian Splat")
    TArray<float> SHCoefficients;

    // UE 原生包围体。用于 Primitive 的裁剪、可见性判断和编辑器选中反馈。
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Gaussian Splat")
    FBoxSphereBounds Bounds;

    // 手动序列化，保证这些自定义数组会随 uasset 一起存盘。
    virtual void Serialize(FArchive& Ar) override;

    // Asset 从磁盘加载完后重新推导 Bounds 和 GPU 资源。
    virtual void PostLoad() override;

    // UObject 销毁前释放渲染资源，避免渲染线程仍引用旧 Buffer。
    virtual void BeginDestroy() override;

    // 让 UE 的资源统计能看到这个 Asset 占用了多少 CPU 内存。
    virtual void GetResourceSizeEx(FResourceSizeEx& CumulativeResourceSize) override;
#if WITH_EDITOR
    // 编辑器里手改数组后，同样要刷新派生数据。
    virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif

    // 便捷接口，供 Details 面板和日志输出读取点数。
    int32 GetPointCount() const;

    // 根据 Position/Rotation/Scale 重建包围盒。
    // 每个高斯先转成旋转后的局部 AABB，再并成整个 Asset 的粗包围体。
    void RebuildBounds();

    // “派生数据”在这里主要指 Bounds 和 GPU RenderResources。
    void RefreshDerivedData();

    // 渲染侧只拿只读指针，避免外部随意改内部资源状态。
    const FGaussianSplatRenderResources* GetRenderResources() const;

private:
    // 把 CPU 数组烘焙成 FRenderResource，真正的 RHI Buffer 初始化会在渲染线程发生。
    void BuildRenderResources();

    // 释放顺序要经过 BeginReleaseResource + FlushRenderingCommands，
    // 因为底层 Buffer 可能还在被上一帧的渲染命令使用。
    void ReleaseRenderResources();

    // 封装 GPU 侧结构化缓冲和 SRV。
    TUniquePtr<FGaussianSplatRenderResources> RenderResources;
};
