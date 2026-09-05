# UnrealGaussianSplatting

![UnrealGaussianSplatting Preview](readme-preview.png)

UnrealGaussianSplatting is a plugin for importing, loading, and rendering standard 3D Gaussian Splatting data in Unreal Engine.

This plugin is currently developed against **Unreal Engine 5.5.4**.

## Features

### Editor

- Import standard 3DGS `.ply` files as `Gaussian Splat Asset`
- Support both `ascii` and `binary_little_endian` PLY formats
- Custom Details panel for Gaussian assets and components
- Standalone editor window:
  - `Window -> Gaussian Splat Editor`

### Runtime

- Load external `.ply` files at runtime
- Create transient `UGaussianSplatAsset` instances from disk files
- Assign assets to `UGaussianSplatComponent` in Blueprint or C++
- Use `AGaussianSplatActor` as a ready-to-place scene actor

### Rendering

- `Points` mode for debugging and validation
- `Gaussian Billboards` mode as the main rendering path
- Support for SH color, opacity, density scaling, and basic runtime controls
- Depth-aware composition so nearer opaque Unreal objects remain visible in
  front of Gaussian splats

## CARLA RGB colour matching

CARLA's `SCS_FinalToneCurveHDR` RGB capture expects tone-mapped linear sRGB
at the plugin's after-tonemap composite pass. The normal SDR viewport expects
display-encoded colours. For this capture mode only, the compositor decodes the
accumulated splat layer from sRGB to linear before blending it with Unreal's
scene colour. It unpremultiplies and re-premultiplies alpha around the conversion
to preserve transparent edges. The viewport path, native scene colour, and
depth testing are unchanged; other capture/HDR output modes are not covered by
this correction.

Rebuild the plugin and recook/repackage shaders before testing a packaged
server. Existing splat assets do not need reimporting or point-count changes.
Compare the server viewport and an attached CARLA RGB camera at the same pose,
including both opaque scenery and splat edges beside a normal Unreal vehicle.
This corrects colour encoding, not differences in camera exposure or lighting.

## Project Layout

- `Source/GaussianSplattingRuntime`
  Runtime module containing assets, components, runtime loading, and rendering code.
- `Source/GaussianSplattingEditor`
  Editor module containing the asset factory, Details customizations, and the standalone editor panel.
- `Shaders`
  Shader implementations for points and Gaussian billboards.
- `Docs`
  Project notes and planning documents.

## Installation

Place the plugin inside your project's `Plugins` directory:

```text
<YourProject>/Plugins/UnrealGaussianSplatting
```

Then regenerate project files and compile, or rebuild the plugin from Unreal Editor.

## Editor Workflow

### 1. Import a Gaussian Asset

Import a standard 3DGS `.ply` file from the Content Browser. The plugin will create a `Gaussian Splat Asset`.

The current parser reads common 3DGS fields including:

- `x/y/z`
- `f_dc_0..2`
- `f_rest_*`
- `opacity`
- `scale_0..2`
- `rot_0..3`

During import, the plugin performs:

- COLMAP-to-UE coordinate conversion
- color and opacity decoding
- SH coefficient reordering
- bounds generation
- GPU resource initialization

Large assets retain all imported splats in the asset. `MaxGpuPointCount`
controls how many evenly sampled splats are uploaded to the GPU; it defaults to
1,000,000 so the renderer can coexist with a large Unreal/CARLA scene on an
8 GB GPU. Increase it only when the available GPU memory permits.

### 2. Display in a Level

The simplest workflow is:

1. Place an `AGaussianSplatActor` in the level
2. Assign a `Gaussian Splat Asset` to its `SplatComponent`

Main component parameters:

- `DensityScale`
- `OpacityScale`
- `PointSize`
- `MaxRenderPoints`
- `PreviewRenderMode`

`MaxRenderPoints` limits the number drawn per view, while the asset-level
`MaxGpuPointCount` limits the persistent GPU buffers. Both values must be at
least the desired render count: increasing only `MaxGpuPointCount` uploads more
splats but does not raise the per-view draw limit. `MaxRenderPoints` defaults to
250,000 and can be raised to 8,000,000, but multi-million-splat rendering has a
substantial sorting and GPU-memory cost.

### 3. Standalone Editor Panel

Open:

```text
Window -> Gaussian Splat Editor
```

The panel works with the currently selected:

- `AGaussianSplatActor`
- `UGaussianSplatComponent`
- `UGaussianSplatAsset`

Current actions:

- `Refresh Asset`
- `Rebuild Bounds`
- `Focus View On Actor`
- `Move Actor To Origin`
- `Move Actor To View`
- `Points`
- `Billboards`

This panel is currently a lightweight working panel, not a full interactive Gaussian editing mode.

## Runtime Workflow

The plugin currently exposes two basic Blueprint helpers:

- `LoadGaussianAssetFromFile`
- `SetGaussianAsset`

Typical runtime flow:

1. Call `LoadGaussianAssetFromFile(FilePath, OutError)`
2. Receive a transient `UGaussianSplatAsset`
3. Call `SetGaussianAsset(Component, Asset)`

Notes:

- The file path must be a normal disk path, not a Content Browser asset path
- Runtime-loaded assets are transient and are not automatically saved as `.uasset`

## Units and Scale

`AGaussianSplatActor` currently defaults to **100x scale** when created.

This is a practical compensation for the common size mismatch between Gaussian / COLMAP style data and Unreal's centimeter-based world scale.

If you are using older placed instances, or if your source data already matches Unreal scale, you may still need to adjust actor scale manually.

## Depth sorting

Splats are semi-transparent, so they must be drawn in depth order every frame.
Two implementations, selected by `r.GaussianSplat.SortMode`:

| Mode | Sort | Dispatches at 3.9M splats |
|---|---|---|
| `0` | bitonic network | 253 |
| `1` (default) | UE GPU radix sort (`SortGPUBuffers`) | 8 |

Bitonic needs `½·log₂N·(log₂N+1)` stages, each a separate dispatch with a full
pipeline drain between them, and pads the array to a power of two. Radix sorts
4 bits per pass, so a 32-bit key takes 8 passes regardless of element count, and
needs no padding.

Measured on tartu_demo (3,885,113 splats, 8 GB GPU, one viewport):

| | Frame |
|---|---|
| bitonic | 55.1 ms |
| radix | 32.8 ms |
| no sort at all (`r.GaussianSplat.SkipSort 1`) | 22.9 ms |

So the sort went from 32.2 ms to 9.9 ms. It does not reach the ~3 ms the pass
count suggests, most likely because `GPUSort.cpp` caps itself at
`MAX_GROUP_COUNT 64` (8,192 threads in flight) -- tuned for particle counts, not
millions of splats. Raising it further means porting a modern high-occupancy
`DeviceRadixSort` into the plugin rather than editing engine source.

Notes for anyone changing this:
- The key/value buffers are typed (`PF_R32_UINT`, `Buffer<uint>`), not
  structured, because that is what the radix shaders bind.
- The rasterizer is bound at record time, so which ping-pong buffer holds the
  result is *predicted* on the CPU from the pass count. `GetGPUSortPassCount()`
  is not `ENGINE_API`, so that logic is mirrored locally and cross-checked
  against the sort's actual return value at runtime -- a mismatch logs an error
  under `LogGaussianSplatProfile`.

## Profiling

The renderer declares its own GPU stats, so `stat GPU` breaks splat cost down by phase:

| Stat | Covers |
|---|---|
| `GaussianSplat/Cull` | frustum rejection + visible-list compaction |
| `GaussianSplat/Sort` | the bitonic depth sort |
| `GaussianSplat/Raster` | billboard/point rasterization |
| `GaussianSplat/Composite` | blending the splat layer back onto scene colour |

Without these the cost is attributed to whatever engine bucket happens to be open
(it appeared under `SortLights`), which makes the profile misleading.

The cull pass also reports how many splats survived rejection. Watch the log
category `LogGaussianSplatProfile` (printed once per 60 frames):

```
visible=1043xxx of 3885113 drawn (26.9%) | sort runs over 4194304 padded
```

Note the sort is sized from the **padded** count, not the visible count, so it
currently costs the same regardless of where the camera looks.

## Current Limitations

- Interactive editing tools are not connected yet
  - no cutout / box selection / partial splat deletion
  - no dedicated Gaussian `EdMode`
  - no full `InteractiveTool` workflow yet
- The current main rendering path is still a billboard-based 3DGS integration
- Rendering quality is still behind a full original 3DGS rasterizer
- More complete antialiasing / footprint / coverage alignment is still missing
- Billboard occlusion uses each Gaussian's center depth; splats do not write to
  Unreal's scene depth buffer
- Runtime experience is still basic
  - no full loader actor workflow
  - no streaming / LOD solution yet
