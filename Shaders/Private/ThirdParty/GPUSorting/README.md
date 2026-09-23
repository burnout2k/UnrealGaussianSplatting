# Vendored GPUSorting shaders

`SortCommon` and `DeviceRadixSort` implement Thomas Smith's DeviceRadixSort: an
8-bit LSD radix sort of key/value pairs, reduce-then-scan, stable, with no
inter-workgroup waiting. The plugin uses it as `r.GaussianSplat.SortMode 2`.

## Provenance

| | |
|---|---|
| Original | https://github.com/b0nes164/GPUSorting (Thomas Smith, MIT) |
| Copied from | https://github.com/aras-p/UnityGaussianSplatting, `package/Shaders/`, repository revision `2c6fed37` |
| `SortCommon` last changed upstream | `d508f39` (2024-11-27, author "Constantin": "Fix sorting on Quest") |
| `DeviceRadixSort` last changed upstream | `994c09d` (2024-09-07, Thomas Smith) |
| sha1 of the verbatim copies | `SortCommon.hlsl` 55a74aed682d4bb561e697ea560a25e4c5c2ab10, `DeviceRadixSort.hlsl` 670bcd350e116a52bbda8f2c999afcfd28c36a69 |

aras-p's copy was chosen over upstream because it carries the Vulkan wave-size
workaround (the wave size is read through a ballot, not `WaveGetLaneCount()`)
and later fixes by its contributors.

## Licence

MIT. `LICENSE.txt` holds both notices: Thomas Smith's (the files' own header)
and aras-p's repository licence, which covers the later changes made there.

## How to see what the plugin changed

The first commit that added this directory holds the files byte-for-byte as
`.hlsl`. The next commit renames them to `.ush` (Unreal's shader virtual paths
need `.usf`/`.ush`) and applies the port edits, each marked `// GS-PORT:`.
`git diff -M <first>..<second> -- .` shows exactly the port.
