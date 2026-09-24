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

## Depth occlusion

Splats read Unreal's opaque scene depth and reject pixels behind nearer
geometry, so vehicles and props stay visible inside a splat map. Splats never
write depth, so they cannot occlude Unreal geometry, only be occluded by it.

`r.GaussianSplat.PerPixelDepth` (default 1) controls how the splat's own depth
is derived:

- `0` -- one depth per splat, its projected centre. Occlusion is all-or-nothing
  per splat.
- `1` -- depth varies across the footprint. For a Gaussian the expected depth
  conditioned on a screen offset is *linear* in that offset
  (`Cov(z,xy) * inverse(Cov2D) * offset`), so the vertex shader gives each quad
  corner its own depth and the rasterizer interpolates. Measured cost: neutral.

Per-pixel is clearest on large hard surfaces intersecting a lot of splat volume.
It does **not** help where splats are genuinely in front of an object -- capture
floaters, or an object embedded in a splat surface such as a car resting on a
splat-reconstructed road. That blending is correct behaviour, not a depth bug.

Known limitation: occlusion uses the Gaussian's depth, not a per-pixel surface
intersection, so boundaries against hard geometry stay soft.

## Depth sorting

Splats are semi-transparent, so they must be drawn in depth order every frame.
Three implementations, selected by `r.GaussianSplat.SortMode`:

| Mode | Sort | Passes at the default 24-bit key |
|---|---|---|
| `0` | bitonic network | 253 dispatches at 3.9M splats |
| `1` | UE GPU radix sort (`SortGPUBuffers`), 4 bits per pass | 6 |
| `2` (default) | plugin DeviceRadixSort, 8 bits per pass | 3 |

Bitonic needs `½·log₂N·(log₂N+1)` stages, each a separate dispatch with a full
pipeline drain between them, and pads the array to a power of two. The radix
sorts need no padding. Mode 2, the default, is described below; where it cannot
run, the plugin falls back to mode 1.

### Key width

`EncodeDepthKey()` is `asuint()` of the view-space depth. That is monotonic for
positive floats, so the key can be shifted right to drop low mantissa bits
without breaking ordering -- and fewer significant bits can mean fewer radix
passes. `r.GaussianSplat.SortKeyBits` controls this:

| Bits | Mode 1 passes | Mode 2 passes | Depth step at 10 m / 100 m |
|---|---|---|---|
| 32 | 8 | 4 | exact |
| **24 (default)** | **6** | **3** | 0.16 mm / 2.5 mm |
| 20 | 6 | 3 | 2.5 mm / 4 cm -- no visible difference from 32 on tartu_demo |
| 16 | 4 | 2 | 4 cm / 64 cm -- visibly wrong; do not use |

Mode 1 rounds an odd pass count up to even (see the notes below), so 20 bits
costs it the same 6 passes as 24. Both modes therefore get 24 bits for the price
of 20, with 16 times finer depth steps, which is why 24 is the default.

Ties are drawn in cull order rather than true depth order. Both radix sorts are
stable, so the error is consistent frame to frame rather than flickering, which
makes it easy to miss on a still image -- move the camera when evaluating a new
value.

### Measurements (mode 1)

On tartu_demo (3,885,113 splats, 8 GB GPU, one viewport), measured before the
even-pass rule, when 20 bits still took 5 passes:

| | Frame |
|---|---|
| bitonic, 32-bit keys | 55.1 ms (18 fps) |
| radix, 32-bit keys | 32.8 ms (30 fps) |
| radix, 20-bit keys | 27.9 ms (36 fps) |
| no sort at all (`r.GaussianSplat.SkipSort 1`) | 22.9 ms |

So the sort went from ~32 ms to ~5 ms. It does not reach the ~3 ms the pass
count suggests, most likely because `GPUSort.cpp` caps itself at
`MAX_GROUP_COUNT 64` (8,192 threads in flight) -- tuned for particle counts, not
millions of splats. The cost grows with the key count: about 32 ms at 20.8M
keys. Mode 2 is the fix.

### DeviceRadixSort (mode 2)

Mode 2 is Thomas Smith's DeviceRadixSort
([GPUSorting](https://github.com/b0nes164/GPUSorting), MIT), vendored from
aras-p's UnityGaussianSplatting into `Shaders/Private/ThirdParty/GPUSorting/`
and ported to Unreal (each change is marked `GS-PORT`). Compared with mode 1 it:

- sorts 8 bits per pass, so 20- and 24-bit keys take 3 passes instead of 6;
- dispatches enough thread groups to fill the GPU (about 5,400 per pass at 20M
  keys, against `SortGPUBuffers`' cap of 64);
- sorts exactly the visible splats: the key count comes from the cull's visible
  counter on the GPU, not a CPU-side prediction, and the draw uses the same count.

For the same input it produces the same stable ascending order as mode 1.

**Where it runs.** Mode 2 needs Vulkan with wave operations, and by default runs
only on NVIDIA GPUs with wave size 32, the only hardware it has been validated
on. Anywhere else, or if its shaders are missing, the plugin falls back to mode 1
and logs the reason once; the profile line shows the effective mode.
`r.GaussianSplat.RadixAllowAnyVendor 1` lifts the vendor restriction for testing.
Non-cell assets always use mode 1.

| CVar | Default | Effect |
|---|---|---|
| `r.GaussianSplat.SortGpuCount` | 1 | 1 = sort the GPU visible count; 0 = sort the CPU-known selected count |
| `r.GaussianSplat.RadixSafeBarriers` | 1 | 1 = aras-p's barrier layout; 0 = upstream's (one barrier before ranking instead of one per bit) |
| `r.GaussianSplat.SortRepeat` | 1 | Re-sort N times (modes 1 and 2), for timing |
| `r.GaussianSplat.RadixForceUnsupported` | 0 | Force the fallback, to test it |
| `r.GaussianSplat.RadixAllowAnyVendor` | 0 | Allow mode 2 on non-NVIDIA or non-wave-32 GPUs |

**Validation.** `r.GaussianSplat.SortValidate N` sorts every Nth frame with both
modes and compares the results element by element on the GPU; any difference is
logged at Error level, with running totals of validated frames and failures.
`SortValidateView`, `SortValidateBatch`, `SortValidateCount` and
`SortValidateGpuCap` pick the view, the splat actor and boundary cases.
`r.GaussianSplat.SortSelfCheck N` checks without a reference that whatever sort
ran left the drawn keys non-decreasing; it works in every mode, including the
fallback. Neither is meant to stay on.

### Measurements (mode 2)

On "Uno" (20,920,240 splats, all resident), RTX 3070 8 GB, Vulkan on Linux, an
empty level with one viewport, `PointSize` 3. Frame time is the median of three
rounds of 10 s each:

| View | No sort | Mode 1 | Mode 2 |
|---|---|---|---|
| whole capture, ~20.8M visible | 32.9 ms | 58.5 ms (17 fps) | 44.8 ms (22 fps) |
| street level | 32.3 ms | 55.3 ms (18 fps) | 39.7 ms (25 fps) |

The sort alone, from `SortRepeat 5` against 1: about 17.6 ms in mode 1 and 4.3 ms
in mode 2. The rest of the gap to "no sort" (about 7.6 ms) is not sorting:
drawing in depth order is slower than drawing in cull order. Mode 2 uses no more
VRAM than mode 1. `SortGpuCount 0` is 0.6-1.6 ms slower than 1.
`RadixSafeBarriers 0` measured 0.66 ms faster, but it has not had the longer
validation run it would need, so the default stays 1.

Validation on the same GPU: 5,999 frames compared against mode 1 with no
difference, covering key widths 8-32 (1-4 passes), repeats 1/2/5, 17 boundary
counts from 1 to 495,360 keys, zero keys, `Lod 0` and a second splat actor. The
forced fallback, mode 1 and mode 0 passed `SortSelfCheck` (307 frames).
Screenshots of mode 2 differ from mode 1 no more than mode 1 differs from itself.

### Setting these

All of these are plain CVars: the console for the session, or
`[ConsoleVariables]` (what the CARLA project uses) or `[SystemSettings]` in
`Config/DefaultEngine.ini` to persist. Both sections apply at the same priority
and work in packaged builds, which have no console. `-ExecCmds=` on the command
line works only in non-Shipping builds. `carla-DefaultEngine.ini.snippet` in this
repository is the reference copy of the CARLA project's sort settings.

Notes for anyone changing this:
- The key/value buffers are typed (`PF_R32_UINT`, `Buffer<uint>`), not
  structured, because that is what `SortGPUBuffers` binds. Mode 2 uses the same
  buffers, so switching modes reuses them.
- Mode 1: the rasterizer is bound at record time, so which ping-pong buffer holds
  the result is *predicted* on the CPU from the pass count. `GetGPUSortPassCount()`
  is not `ENGINE_API`, so that logic is mirrored locally and cross-checked against
  the sort's actual return value at runtime -- a mismatch logs an error under
  `LogGaussianSplatProfile`. The pass count is forced even, so the result lands in
  the buffer the cull filled.
- Mode 2: its passes are ordinary RDG passes and the host swaps the buffers
  itself, so the result buffer is known, not predicted. RDG does **not** reject a
  buffer bound as SRV and UAV in the same pass (it silently merges the states to
  UAV), so the host asserts after every swap that source and destination differ.
- Under `-rdgdebug`, a one-pass mode-2 sort (`SortKeyBits 8`) logs that
  `GaussianSplat.KeyBufferAlt` is produced but never used: the last pass writes
  keys that only the validator reads. Expected and harmless; with two or more
  passes every key buffer is read, so the default 24 bits never logs it.

## Large captures

Two hard limits used to make big PLYs impossible to import; both are fixed.

**Files over 2 GB.** The reader used `TArray<uint8>`, which is int32-indexed, so
`LoadFileToArray` refused with *"too large for 32-bit reader, use TArray64"*.
Now `TArray64`. The binary reader already walked a raw pointer with int64
offsets, so only the container changed. The ascii path still converts the body
to an `FString` and so is bounded by int32 regardless -- it now fails cleanly
instead of silently truncating.

**Captures over ~47M splats.** `SHCoefficients` is a `TArray<float>` holding 45
floats per splat, which overflows int32 at 47,721,859 splats and hard-crashes
the editor mid-import (*"Trying to resize TArray to an invalid size"*).

The second is worse than it sounds, because a capture with **no** spherical
harmonics still paid for it: the parser appended 45 zeros per splat regardless.
It now detects the absence of `f_rest_*` properties and skips SH entirely.
`BuildFromAssetData` already reads SH through `IsValidIndex()` with a `0.0f`
fallback, so an empty array needs no renderer change. A capture that genuinely
has SH and is too large now fails with an error rather than crashing.

Measured on an 85,843,930-splat capture (4.5 GiB PLY, no SH):

| | Before | After |
|---|---|---|
| RAM during import | crashed at ~15.5 GB of zero SH | ~10 GB |
| Resulting `.uasset` | never completed | 4.2 GB |

The GPU buffer is skipped too when the asset has no SH. `BuildFromAssetData`
sets `bHasSH` from whether `SHCoefficients` is empty, builds a single dummy
element instead of `PointCount * 15` (a null SRV cannot be bound), and the
rasterizer branches past SH evaluation on a `HasSH` flag.

Measured at 8M splats uploaded, editor VRAM via `nvidia-smi`:

| | VRAM | Frame |
|---|---|---|
| zero SH still uploaded | 6,975 MiB | 21.71 ms |
| SH skipped | **5,126 MiB** | **20.37 ms** |

1,849 MiB freed -- 242 B/splat, so ~64 B/splat instead of ~304. The frame time
also improved: the rasterizer had been fetching 240 bytes of zeros per splat in
depth-sorted order, which is cache-hostile. No visual change, since those
coefficients were already zero.

No re-import is needed for an asset imported after the parser fix: the empty
`SHCoefficients` array is already serialised, and `bHasSH` is derived from it
at load.

**Captures with SH over ~17.9M splats.** The GPU copy used to be 15 `float4`
per splat -- 240 B, a quarter of it padding -- in one buffer. The RHI sizes a
buffer as uint32, so past 17,895,697 splats `TResourceArray::GetResourceDataSize`
hit a Fatal and took the editor down the moment `MaxGpuPointCount` was raised.

SH is now a palette: each distinct set of 45 coefficients is uploaded once (45
floats, no padding) and every splat carries a uint index into it. Sets are
matched by their bytes -- a 64-bit hash, confirmed by a byte compare, so a
collision costs a duplicate entry, never a wrong one -- which makes it
bit-exact. Captures converted from SOG already share their SH through per-chunk
palettes, and there the saving is large. Measured on "Uno" (20,920,240 splats,
SH degree 3, SOG-derived):

| | Before | After |
|---|---|---|
| distinct SH sets | -- | 2,293,747 |
| SH on the GPU, all splats | 4,788 MiB (never fit) | 474 MiB |
| whole asset on the GPU | Fatal above 17.9M | 873 MiB (43.7 B/splat) |

A capture whose SH is unique per splat gains nothing from sharing but still
drops the padding (240 -> 184 B/splat including the index). If a palette would
still exceed one 4 GiB buffer, the upload drops SH with a warning instead of
the Fatal -- every splat stays, only view-dependent colour is lost. The
`SH palette:` log line reports the set count and size at each upload.

## Profiling

The renderer declares its own GPU stats, so `stat GPU` breaks splat cost down by phase:

| Stat | Covers |
|---|---|
| `GaussianSplat/Cull` | frustum rejection + visible-list compaction |
| `GaussianSplat/Sort` | the depth sort, whichever mode runs |
| `GaussianSplat/Raster` | billboard/point rasterization |
| `GaussianSplat/Composite` | blending the splat layer back onto scene colour |

Before these scopes existed, the cost was attributed to whatever engine bucket
happened to be open (it appeared under `SortLights`), which made the profile misleading.

The cull pass also reports how many splats survived rejection. Watch the log
category `LogGaussianSplatProfile`, printed once per 60 renders of each view for
the first splat actor. It shows the LOD selection, the visible count and the
effective sort mode. Modes 0 and 1 sort the whole LOD selection; mode 2 sorts only
the visible splats.

Frame time from the log: the timestamp difference between two consecutive lines
of the same view divided by the difference of the engine frame numbers in
brackets. Use only line pairs exactly 60 frames apart, and measure with
`r.VSync 0` (the editor otherwise caps at 16.7 ms).

## Dense-capture levers

Millions of faint, overlapping Gaussians accumulate into a milky veil that washes
out the scene -- visible on a 6M-splat capture where halving `MaxRenderPoints`
made the image *clearer*, not worse, because it removed haze rather than detail.
Three CVars attack that, all defaulting to previous behaviour:

| CVar | Default | Effect |
|---|---|---|
| `r.GaussianSplat.AlphaCutoff` | `0.004` (1/255) | discards splat *pixels* below this alpha before blending, trimming each Gaussian's faint tails |
| `r.GaussianSplat.MinSplatOpacity` | `0` (off) | rejects whole splats during culling when their effective opacity is below this -- cheaper than AlphaCutoff, since they never rasterise |
| `r.GaussianSplat.MaxSplatDistance` | `0` (off) | rejects splats beyond this view depth in world units; frustum culling drops what is off to the sides, this drops what is too far ahead |

`MinSplatOpacity` is the sharper instrument for haze: a splat that is 2% opaque
contributes nothing but veil *everywhere*, so dropping it in the cull pass skips
its whole rasterisation. `MaxSplatDistance` bounds street-level views, which
otherwise draw the far side of the capture at full density -- note it is a hard
cut with no fade, so raise it until the boundary stops being visible.

**Not yet measured.** These compiled and the CVars are live, but their effect on
frame time and image quality has not been recorded. Unlike the sort work, treat
the numbers as unknown until someone sweeps them.

## Measured non-wins

Tried and reverted, so they are not re-attempted:

**Spherical-harmonic degree.** A CVar clamping evaluated SH bands (3/2/1/0) made
no measurable difference: 20.09 / 19.55 / 19.25 / 19.38 ms, with degree 0 --
which skips *every* SH read -- landing between 1 and 2. All noise.

The 932 MB SH buffer looks alarming but only the visible splats are read each
frame: ~512 K visible x 240 B is ~123 MB, roughly 0.4 ms of bandwidth, and even
at 1.7 M visible it is only ~1.4 ms. **SH is a memory-footprint problem, not a
bandwidth one.** Packing to half3 (932 MB -> ~350 MB) is still worth doing if
VRAM is tight -- the renderer sits at ~6.7 GB of 8 GB -- but it will not buy
frames. (The footprint is now handled losslessly by the SH palette, see Large
captures; half3 would be lossy and only matters for captures with unique SH.)

**Alpha cutoff.** Raising the discard threshold from 1/255 to 0.02 and 0.05 to
cut overdraw: 19.4 / 19.62 / 19.26 ms. Also noise.

So the remaining raster cost is fragment and blend work, not buffer reads, and
has no cheap knob. Reducing it means drawing fewer or smaller splats -- LOD, or
a tile-based rasterizer with early alpha termination.

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

## Third-party notices

`Shaders/Private/ThirdParty/GPUSorting/` holds MIT-licensed code by Thomas
Smith, taken from aras-p's UnityGaussianSplatting. That licence covers only those
files. The changes made in this fork are MIT-licensed as well; the upstream
plugin publishes no licence. `ThirdPartyNotices.txt` states the terms of each
part of this plugin; packaged builds stage it.
