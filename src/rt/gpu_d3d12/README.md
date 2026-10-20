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
- a 28-DWORD `b0` GE-state root block, `t0` texture/`t1` CLUT table, dynamic
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
- persistent 16-entry color and depth caches with serial-protected handles,
  format/stride reinterpretation, direct render-to-texture aliases, feedback
  snapshots, and compatible `TRXKICK` image copies;
- lazy CPU visibility for GPU-newer eDRAM color targets, including pre-write
  preservation for generated stores, DMA, MPEG output, and block transfers;
- a persistent 480×272 display texture and fullscreen-triangle presentation pass
  that preserve the exact PSP aspect ratio through window resizing;
- direct presentation from compatible cached 512×272 targets, with software-GE
  framebuffer upload retained for uncached and movie frames.

`vertex.cpp` deliberately consumes the existing `GeVtx` backend seam instead of
duplicating PSP vertex decoding. `ge.c` remains authoritative for all raw attribute
formats, morph-before-skin decoding, through mode, indices, GE-exact CPU Bezier/B-spline
tessellation, transform/lighting, clipping, and primitive assembly. Generated patch
triangles, lines, and points cross the same `GeBackend` seam as ordinary primitives.
Captured vertices and primitive-boundary records are copied into the active fenced
upload-ring partition during presentation. The hooks still return zero, so the reference
software rasterizer remains authoritative until native batches are submitted to GPU
render targets.

`pipeline_key.cpp` maps only PSO-static GE fields; shader constants, descriptors,
scissor, and blend constants remain per-draw state and intentionally do not multiply the
cache. Each captured batch stores its key and canonical 160-byte shader block before later
GE commands mutate the live state.
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

`framebuffer_key.cpp` canonicalizes color/depth identity and exact PSP transfer
rectangles. `framebuffer.cpp` owns fixed-size BGRA8 color resources and D16 depth
resources, their RTV/DSV/SRV descriptors, explicit resource states, immediate
copy/readback commands, and serial-protected LRU lifetime. Multiple views of the
same address survive within one captured generation; an older GPU-newer view is
materialized before a format or stride reinterpretation is uploaded.

Exact unswizzled, single-level texture views can alias a cached target when their
format, stride, offset, and bounds match. Same-target feedback samples from a
snapshot copied before the target is rebound. Presentation copies only the
visible 480×272 rectangle from a compatible cached target. Because native draws
are not enabled yet, each captured target is currently refreshed from the
authoritative software framebuffer.

GPU-newer color targets activate fast-gated VRAM callbacks in `recomp.h`. CPU
reads synchronously materialize overlapping color data; CPU writes materialize
it before the store and invalidate overlapping framebuffer/texture views after
the store. DMA, MPEG, and CPU block-transfer bulk writes follow the same
pre-write contract. A full CPU-content hash catches unnotified edits to clean
targets before GPU reuse. GPU-owned depth readback remains inactive while native
draw submission is disabled and must be enabled before native depth becomes
authoritative.

Compatible `TRXKICK` operations between distinct cached targets use an exact
D3D12 image copy and update the guest-VRAM mirror. Format, stride, pixel-size,
aliasing, or bounds mismatches explicitly fall back to the CPU transfer path.

`shaders/psp_vs.hlsl` and `shaders/psp_ps.hlsl` are the native Shader Model 6 ports of
the Vulkan GE shaders. Their checked-in `*.inc` files contain stripped DXIL consumed by
`device.cpp`. `shader_state.cpp` snapshots texture functions, alpha/color tests, fog,
D16 `minz`/`maxz`, framebuffer aliases, stencil/depth state, exact blend and fixed-color
state, logic operations, native masks/formats, clear state, scissor, and the signed 4×4
dither matrix into the 40-DWORD root block. The pixel shader reads ordered destination
color/depth snapshots at `t2`/`t3` and performs PSP fragment ordering, native packing,
component masks, and `SV_Depth` preservation manually. Native draw replay remains disabled
until software raster suppression and GPU target ownership switch atomically.

Regenerate or verify the shaders with:

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
Set `SR_D3D12_FRAMEBUFFER_LOG=1` to print target creation and a shutdown summary of
captures, uploads, readbacks, aliases, invalidations, transfers, presentation hits,
and live targets. Set `SR_D3D12_FRAMEBUFFER_SAFE=1` to force every cached target
through a GPU readback after cached presentation; this is a diagnostic mode and
intentionally stalls the graphics queue.

Later phase-4 tasks add the destination-aware blend/logic path and submit the uploaded
batches behind the existing framebuffer, descriptor, invalidation, and shader-state model.

The root project keeps this dependency optional:

```text
cmake -S . -B out/build/d3d12 -DKZL_ENABLE_D3D12=ON -DSDL3_DIR=<SDL3 package>/cmake
```

The fork Makefile enables `SR_D3D12` together with its existing SDL3/Vulkan build.
