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
- a 40-DWORD `b0` GE-state root block, `t0` texture, `t1` CLUT, and `t2`/`t3`
  destination tables, dynamic `s0` sampler table, static clamp-nearest
  `s1`/clamp-linear `s2` samplers, and a vertex-visible space-1 `t0` raw-buffer
  root SRV;
- a shader-visible dynamic sampler ring partitioned by fenced frame;
- a persistently mapped, three-frame upload ring;
- capture of `ge.c`'s fully decoded primitive stream into the HLSL shader's exact
  32-byte raw-load ABI, including PSP flat-shading colors, 1/16-pixel snapping, sprite
  expansion, line/point quad expansion, and one state record per logical GE draw;
- immutable GE list/draw context on every captured batch, emitted as nested PIX events
  when the deferred D3D12 command list is recorded;
- a collision-safe 512-entry asynchronous PSO cache keyed by a canonical 16-byte
  projection of blend, color-write, depth, cull, and attachment-format state, backed by
  `ID3D12PipelineLibrary` and an exact no-cull ubershader fallback;
- a collision-safe 512-entry texture cache with canonical metadata/CLUT keys,
  immutable encoded-content versions, up to eight decoded RGBA8 mip uploads,
  per-frame SRV/sampler reuse, bounded staging, and queue-safe LRU eviction;
- persistent 16-entry color and depth caches with serial-protected handles,
  format/stride reinterpretation, direct render-to-texture aliases, feedback
  snapshots, and compatible `TRXKICK` image copies;
- lazy CPU visibility for GPU-newer eDRAM color targets, including pre-write
  preservation for generated stores, DMA, MPEG output, and block transfers;
- a persistent `(480×scale)×(272×scale)` display texture and fullscreen-triangle
  presentation pass that preserve the exact PSP aspect ratio through window resizing;
- direct presentation from compatible cached targets, with a scaled 512×512 color
  envelope for PSP power-of-two sampling and a scaled 512×272 render region;
- Tier-1/2 Variable Rate Shading for direct scaled draws, keeping geometry,
  coverage and D16 depth at internal resolution while matching fragment density
  to the PSP grid; exact destination-aware draws and presentation stay 1×1;
- guest-VRAM upload retained for CPU-written movie frames.

`vertex.cpp` deliberately consumes the existing `GeVtx` backend seam instead of
duplicating PSP vertex decoding. `ge.c` remains authoritative for all raw attribute
formats, morph-before-skin decoding, through mode, indices, GE-exact CPU Bezier/B-spline
tessellation, transform/lighting, clipping, and primitive assembly. Generated patch
triangles, lines, and points cross the same `GeBackend` seam as ordinary primitives.
Captured vertices are copied into the active fenced upload-ring partition during
presentation. Their diagnostic FNV hash is computed only when vertex/GPU logging
will consume it. `psp_vs.hlsl` indexes the upload GPU address with `SV_VertexID` and performs
two aligned raw loads, so PSOs need no fixed input layout or per-frame IA vertex-buffer bind.
Accepted primitives with the same immutable list/draw identity, kind and perspective mode
append to one batch. Successful hooks return one, suppress the software rasterizer, and
submit `DrawInstanced` calls into the captured GPU framebuffer.

`pipeline_key.cpp` maps only PSO-static GE fields; shader constants, descriptors,
scissor, and blend constants remain per-draw state and intentionally do not multiply the
cache. Each logical draw stores its key and canonical 160-byte shader block before later
GE commands mutate the live state. Command recording compares that block exactly with the
previous binding and skips an identical 40-DWORD root write while still restoring scissor.
`pipeline.cpp` creates one no-cull ubershader PSO during device initialization and never
compiles on the command-recording thread. The pixel shader uses `SV_IsFrontFace` for exact
fallback culling, while a worker prewarms the hardware-cull variant and promotes later
misses after a full-key check. A versioned, shader-hashed, key-sized, adapter-LUID-scoped
`ID3D12PipelineLibrary` is capped at 64 MiB and published through a flushed temporary file
plus atomic replace. Draw submission uses the exact ubershader while specialized cull
pipelines compile asynchronously.

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
rectangles. `framebuffer.cpp` owns BGRA8 color resources and D16 depth resources
at the configured 1x-8x integer scale, their RTV/DSV/SRV descriptors, explicit
resource states, immediate copy/readback commands, and serial-protected LRU
lifetime. Guest uploads replicate logical texels into scaled storage; CPU
readback center-samples each logical pixel, and `TRXKICK` rectangles scale only
for the GPU copy while retaining PSP coordinates in guest memory. Multiple views
of the same address survive within one captured generation; an older GPU-newer
view is materialized before a format or stride reinterpretation is uploaded.
Depth stays GPU-owned across capture generations and is uploaded again only
after creation or a real CPU/DMA invalidation.

Exact unswizzled, single-level texture views can alias a cached target when their
format, stride, offset, and bounds match. Color resources retain a scaled 512×512
texture envelope so the PSP's common 512×272 framebuffer-as-512×512-texture pattern
stays GPU-resident with correct normalized coordinates. Same-target feedback samples from
a snapshot copied before the target is rebound. The first snapshot for a target
copies its complete resource. Later draws accumulate dirty PSP-space rectangles;
a feedback refresh copies the scaled bounding union of the sampled source and
those dirty rectangles, then clears the dirty set because the snapshot is
globally current again. This preserves draw ordering and safe snapshot reuse
without copying the complete 2048x2048 resource for every small sampled region.
Set `SR_D3D12_LEGACY_FEEDBACK_SNAPSHOTS=1` to force a complete copy at every
refresh for differential diagnosis. CLUT32 views of compatible live 8888
targets and CLUT16 views of compatible 565, 5551, or 4444 targets follow the
same ordered snapshot path instead of decoding stale guest VRAM. `texture.cpp` captures and
deduplicates the draw's 2 KiB palette and binds it as an R32 buffer at `t1`. The pixel
shader center-samples the scaled target, repacks it to the target's native PSP format,
then applies the PSP shift/mask/start lookup and nearest or linear filtering. These
framebuffer views bypass content hashing, CPU decode, texture-cache allocation, and
xBRZ; xBRZ remains limited to ordinary decoded textures. Presentation copies only the
scaled visible 480×272 rectangle from a compatible cached target.

Incompatible framebuffer texture reinterpretations materialize an overlapping
GPU-newer target into guest VRAM before the fallback texture is hashed and decoded.
That decoder synchronizes each complete texture level once, then reads the bounded
host span directly. It does not invoke the watched VRAM callback once per texel while
another target remains GPU-newer.
The shader derives logical pixel coordinates from `SV_Position / scale` so the
PSP dither matrix remains stable while destination color/depth loads use physical
scaled coordinates.

GPU-newer color targets activate fast-gated VRAM callbacks in `recomp.h`. CPU
reads synchronously materialize overlapping color data; CPU writes materialize
it before the store and invalidate overlapping framebuffer/texture views after
the store. DMA, MPEG, and CPU block-transfer bulk writes follow the same
pre-write contract. A full CPU-content hash catches unnotified edits to clean
targets before GPU reuse. GPU-owned color is materialized lazily; depth remains
GPU-resident because the guest CPU path does not consume the cached depth image.

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
component masks, and `SV_Depth` preservation manually for states that require destination
access. Channel-aligned clear/source-replace and hardware-mappable blend/depth states take
a direct branch and use specialized fixed-function PSOs, avoiding all destination copies.
Always-pass stencil states can also use the direct branch when their depth-fail operation
is a no-op and their depth-pass result is destination-independent. Hardware depth then
orders fragments while the direct pixel shader writes the known packed stencil value.
Masked, destination-dependent, and source-alpha-blended stencil writes retain the exact
snapshot path.
Until an asynchronous specialized PSO is ready, the draw clears its direct flag and falls
back to the exact snapshot ubershader. Draw submission and the GPU-newer ownership mark
occur together before presentation.

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
Set `SR_GPU_LOG=1` to print every D3D12 GE flush, one line per logical GE draw, each
present, and a shutdown summary. Draw lines include the list address, command PC/opcode,
captured batch/vertex counts, framebuffer/texture handles, shader state, scissor, and
screen/texture coordinates. `SR_GPU_LOG_START` and `SR_GPU_LOG_END` restrict verbose
draw/present lines to an inclusive presentation range.
Set `SR_D3D12_PSO_LOG=1` to print ubershader setup, asynchronous PSO sources, disk-cache
writes, and a shutdown cache summary. `SR_D3D12_PSO_CACHE=<path>` opts into
pipeline-library persistence. It is disabled by default because some drivers reject
a library they previously serialized; the in-memory asynchronous cache remains active.
The serialized library is unstable, but the pipeline *keys* are plain state, so the backend
also keeps a small key list (`pso_keys.bin` beside the executable, or `SR_SETTINGS_DIR`).
Every key a run requests is recorded; the next run queues them on the PSO worker at startup,
so a scene's pipelines are compiled before it draws instead of during its first frames with
the ubershader fallback. `SR_D3D12_PSO_KEYS=<path>` relocates the list and
`SR_D3D12_NO_PSO_KEYS=1` disables it. A stale or damaged file is ignored.
Set `SR_D3D12_TEXTURE_LOG=1` to print texture-version misses and a shutdown summary of
lookups, hits, uploads, invalidations, evictions, background decodes, and live entries.
A texture first captured during a frame is decoded and xBRZ-scaled on worker threads: the GE
thread copies the level bytes, palette and decode state (`ge_tex_snapshot_*`), and texture
preparation waits for the result before recording the upload. `SR_D3D12_SYNC_TEXTURE_DECODE=1`
decodes in line instead, and `SR_D3D12_VERIFY_TEXTURE_DECODE=1` decodes every texture both
ways and aborts on any difference.
Set `SR_D3D12_FRAMEBUFFER_LOG=1` to print target creation and a shutdown summary of
captures, uploads, readbacks, aliases, invalidations, transfers, presentation hits,
and live targets. Set `SR_D3D12_FRAMEBUFFER_SAFE=1` to force every cached target
through a GPU readback after cached presentation; this is a diagnostic mode and
intentionally stalls the graphics queue. Set
`SR_D3D12_DISABLE_FEEDBACK_ALIASES=1` to route format-reinterpreted framebuffer
textures through the synchronized CPU fallback for differential testing.

Install PIX for Windows with `winget install --id Microsoft.PIX --exact`. The command
list always carries nested `GE list` and `GE draw` events using D3D12's PIX-compatible
ANSI event ABI, so no PIX header or event-runtime DLL is required to build or deploy the executable. Press **F10** in
the D3D12 window to write `ge_state_f<frame>_p<pid>_<index>.txt` in the working
directory. The dump includes the current list/draw identity, decoded raster/texture/
geometry/transfer state, exact matrix values, and a complete raw `GeState` byte image.

The root project keeps this dependency optional:

```text
cmake -S . -B out/build/d3d12 -DKZL_ENABLE_D3D12=ON -DSDL3_DIR=<SDL3 package>/cmake
```

The fork Makefile enables `SR_D3D12` together with its existing SDL3/Vulkan build.
