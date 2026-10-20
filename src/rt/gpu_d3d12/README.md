# D3D12 device backend

This directory contains the Windows D3D12 implementation of the runtime `GeBackend`.

`device.cpp` currently provides the phase-4 device foundation:

- SDL3 window creation and Win32 `HWND` extraction through
  `SDL_PROP_WINDOW_WIN32_HWND_POINTER`;
- high-performance hardware adapter selection, with `SR_D3D12_WARP=1` as an explicit
  software-adapter override;
- a D3D12 device, direct and copy queues, timeline fences, and a three-buffer
  flip-discard swapchain;
- shader-visible CBV/SRV/UAV descriptor rings plus RTV and DSV heaps;
- a 16-DWORD `b0` GE-state root block, `t0` texture/`t1` CLUT table, dynamic
  `s0` sampler table, and static clamp-nearest `s1`/clamp-linear `s2` samplers;
- a shader-visible dynamic sampler ring partitioned by fenced frame;
- a persistently mapped, three-frame upload ring;
- capture of `ge.c`'s fully decoded primitive stream into the HLSL shader's exact
  32-byte vertex ABI, including PSP flat-shading colors, 1/16-pixel snapping, sprite
  expansion, and line/point quad expansion;
- a collision-safe 512-entry PSO cache keyed by a canonical 16-byte projection of
  blend, color-write, depth, cull, and attachment-format state, with synchronous
  creation on first use;
- a collision-safe 512-entry texture cache with canonical metadata/CLUT keys,
  immutable encoded-content versions, up to eight decoded RGBA8 mip uploads,
  per-frame SRV/sampler reuse, bounded staging, and queue-safe LRU eviction;
- a persistent 480×272 display texture and fullscreen-triangle presentation pass
  that preserve the exact PSP aspect ratio through window resizing;
- software-GE framebuffer upload so `SR_GPU=d3d12` remains usable while native
  draw submission and GPU-resident render targets are being integrated.

`vertex.cpp` deliberately consumes the existing `GeVtx` backend seam instead of
duplicating PSP vertex decoding. `ge.c` remains authoritative for all raw attribute
formats, skinning, through mode, indices, transform/lighting, clipping, and primitive
assembly. Captured vertices and primitive-boundary records are copied into the active
fenced upload-ring partition during presentation. The hooks still return zero, so the
reference software rasterizer remains authoritative until native batches are submitted to
GPU render targets.

`pipeline_key.cpp` maps only PSO-static GE fields; shader constants, descriptors,
scissor, and blend constants remain per-draw state and intentionally do not multiply the
cache. Each captured batch stores its key before later GE commands mutate the live state.
`pipeline.cpp` uses FNV-1a to select an open-addressed slot, confirms the full key on
hits, compiles the embedded HLSL synchronously on misses, and binds the resulting PSO to
the live command list. Draw submission is intentionally deferred while software
presentation remains the reference path.

`texture_key.cpp` separates resource identity from sampling state. The 96-byte texture
key retains the legal PSP mip chain, aligned buffer widths, format/swizzle state, and
complete CLUT identity; the 8-byte sampler key retains min/mag/mip filters, U/V wrap,
and signed LOD bias. `texture.cpp` combines that metadata with quick and full hashes of
the encoded source ranges. It creates immutable D3D12 resource versions so batches
captured before and after a RAM write keep the correct contents. CPU, DMA, and block
transfer dirty ranges invalidate every overlapping mip. Each large texture is fully
verified once per presented frame even without an explicit notification.

The cache decodes through the shared `ge.c` implementation, which supports 5650, 5551,
4444, 8888, CLUT4/8/16/32, and PSP-layout DXT1/3/5, including swizzling and shared or
per-level palettes. Unique SRVs and sampler descriptors are reused within a fenced frame.
The cache is bounded to 512 entries and 64 MiB of pending decoded data; a full cache
waits for the graphics queue before evicting its 128 least-recently used versions.
`TEXFLUSH` is treated as a synchronization hint, while actual CPU/DMA/block-transfer
writes drive invalidation.

`shaders/psp_vs.hlsl` and `shaders/psp_ps.hlsl` are the native Shader Model 6 ports of
the Vulkan GE shaders. Their checked-in `*.inc` files contain stripped DXIL consumed by
`device.cpp`. Regenerate or verify them with:

```text
python tools/compile_d3d12_shaders.py [--dxc path/to/dxc]
python tools/compile_d3d12_shaders.py [--dxc path/to/dxc] --check
```

The Makefile exposes the same operations as `shaders-d3d12` and
`check-shaders-d3d12`, plus the focused `test-d3d12-vertex` target. D3D12-enabled
root CMake builds expose `kzl_d3d12_shaders`, `kzl_d3d12_shaders_check`, and the
`kzl_d3d12_vertex` CTest.

Set `SR_D3D12_DEBUG=1` to request the D3D12 debug layer. Add
`SR_D3D12_GPU_VALIDATION=1` to enable GPU-based validation. Both settings fail
explicitly when the requested debug interface is unavailable.

Set `SR_D3D12_VERTEX_LOG=1` to print compact capture counts and content hashes for the
first three presents and every 120th present, plus a shutdown summary.
Set `SR_D3D12_PSO_LOG=1` to print synchronous PSO creations and a shutdown cache
summary.
Set `SR_D3D12_TEXTURE_LOG=1` to print texture-version misses and a shutdown summary of
lookups, hits, uploads, invalidations, evictions, and live entries.

Later phase-4 tasks submit the uploaded batches, populate the remaining per-draw state,
and add GPU-resident framebuffer aliases behind the same descriptor and invalidation
model.

The root project keeps this dependency optional:

```text
cmake -S . -B out/build/d3d12 -DKZL_ENABLE_D3D12=ON -DSDL3_DIR=<SDL3 package>/cmake
```

The fork Makefile enables `SR_D3D12` together with its existing SDL3/Vulkan build.
