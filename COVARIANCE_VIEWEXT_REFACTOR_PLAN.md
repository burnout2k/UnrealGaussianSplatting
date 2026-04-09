# Covariance + ViewExtension Refactor Plan

## Target

- Use covariance as the primary Gaussian shape representation in assets and GPU resources.
- Route both `Points` and `Billboards` through `SceneViewExtension`.
- Keep `Boxes` only as an enum placeholder for now, and defer any actual implementation until after `Points` and `Billboards` are stable.

## Stage 1: Asset Data Model

1. Change [Source/GaussianSplattingRuntime/Public/GaussianSplatAsset.h] to store covariance instead of `Rotations` and `Scales`.
2. Define a stable covariance layout.
   Suggested storage: 6 independent symmetric terms `xx xy xz yy yz zz`.
3. Update [Source/GaussianSplattingRuntime/Private/GaussianSplatAsset.cpp]:
   - `Serialize`
   - `GetResourceSizeEx`
   - `BuildRenderResources`
   - `RebuildBounds`

## Stage 2: Import Pipeline

1. Update [Source/GaussianSplattingEditor/Private/GaussianSplatAssetFactory.cpp].
2. `BuildImportedGaussian` should output covariance directly in UE space.
3. Remove the current decomposition back into `rotation + scale`.
4. Keep the existing position, color, and SH import path unchanged.
5. Do not add any old-asset compatibility path. Old assets have already been removed.

## Stage 3: GPU Resources and Shader Parameters

1. Update [Source/GaussianSplattingRuntime/Private/Render/GaussianSplatRenderResources.h] and [Source/GaussianSplattingRuntime/Private/Render/GaussianSplatRenderResources.cpp].
2. Replace `RotationBuffer` / `ScaleBuffer` with covariance buffer storage.
3. Update [Source/GaussianSplattingRuntime/Private/Render/GaussianSplatShaders.h] so both `CullSort` and `Raster` read covariance SRVs.
4. Update [Shaders/Private/GaussianSplatCommon.ush] to rebuild symmetric covariance directly from the uploaded buffer instead of reconstructing it from quaternion and scale.

## Stage 4: Move Points to ViewExtension

1. Extend [Source/GaussianSplattingRuntime/Private/Render/GaussianSplatViewExtension.cpp] so `Points` components are also collected into render batches.
2. Add a dedicated `Points` rendering path under `ViewExtension`.
3. The `Points` path does not need covariance.
   It can render directly from Gaussian centers projected into screen space.
4. Update [Source/GaussianSplattingRuntime/Private/GaussianSplatComponent.cpp] so `Points` no longer create `FGaussianSplatSceneProxy`.

## Stage 5: Switch Billboards to Direct Covariance

1. Update [Shaders/Private/GaussianSplatCullSort.usf] to consume covariance directly.
2. Update [Shaders/Private/GaussianSplatRaster.usf] to consume covariance directly.
3. Keep the current Cull / Sort / Raster structure intact.
4. Only replace the per-splat shape representation and the related shader math.

## Stage 6: Defer or Remove Boxes

1. Keep `Boxes` in `EGaussianPreviewRenderMode` for now if needed as a placeholder.
2. Do not implement a runtime `Boxes` path in this refactor.
3. Revisit `Boxes` only after `Points` and `Billboards` are stable under the new architecture.
4. If `SceneProxy` is no longer needed after that, remove:
   - [Source/GaussianSplattingRuntime/Private/Render/GaussianSplatSceneProxy.h]
   - [Source/GaussianSplattingRuntime/Private/Render/GaussianSplatSceneProxy.cpp]

## Validation

- Asset import still produces correct positions, colors, SH coefficients, and Gaussian shape.
- Bounds remain reasonable after switching to covariance storage.
- `Points` mode works entirely through `ViewExtension`.
- `Billboards` remain visually consistent after switching to direct covariance input.
- No code path still depends on `rotation + scale` as the primary asset representation.
