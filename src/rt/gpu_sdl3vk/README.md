# SDL3 + Vulkan GPU renderer

This directory contains the runtime's original Vulkan renderer. It does not
reuse PPSSPP's GPU implementation. The software GE in `ge.c` remains the
pixel oracle and normally performs vertex decode, transformation, clipping,
primitive acceptance, and cull-order handling. The Vulkan backend receives
accepted screen-space primitives through `GeBackend` and owns rasterization,
persistent render targets, texture caching, and presentation.

> **AI disclosure:** this renderer was written with substantial assistance
> from an LLM (Anthropic Claude). It reproduces PSP/GE semantics derived from
> the PPSSPP-informed software renderer and is GPL-2.0-or-later. See
> [`../../../CREDITS.md`](../../../CREDITS.md).

## Runtime model

Enable the backend with `SR_GPU=vk`. The renderer uses:

- a 512x512 logical RGBA8 color envelope for each PSP framebuffer, with a
  512x272 logical writable region, scaled physically from 1x through 8x;
- D16 depth images keyed by depth address and stride;
- persistent GPU ownership, with watched guest-VRAM reads/writes providing
  explicit materialization and invalidation;
- direct GPU aliases for compatible sibling framebuffer textures and ordered
  snapshots for self-feedback, including compatible CLUT32 textures sourced
  from RGBA8888 framebuffers;
- the software decoder for ordinary RAM textures, followed by a bounded
  Vulkan texture cache and optional xBRZ upload scaling. Cache misses copy an
  immutable source/CLUT snapshot on the GE thread, decode and scale it on a
  bounded SDL worker pool, and wait before the first pending upload. An
  opt-in compute path can decode scale-1 snapshots directly into the upload
  buffer while preserving the same cache identity and invalidation rules; and
- direct swapchain presentation of GPU-resident targets.

The 512x512 logical color envelope is intentional. PSP titles commonly render
to a 512x272 region and later sample the same address as a 512x512 texture.
Color, depth, destination snapshots, framebuffer feedback, block transfers,
scissors, and presentation use the same configured internal-resolution scale.
Only the visible scaled 480x272 area is copied to the swapchain.

`SR_VULKAN_SYNC_TEXTURE_DECODE=1` restores in-line CPU decode, while
`SR_PERF_ALTERNATE=texdecode` alternates the selected optimized path and
synchronous CPU decode at presentation boundaries. The normal optimized path
is the asynchronous CPU worker pool. `SR_VULKAN_GPU_TEXTURE_DECODE=1` instead
selects compute decode when texture scale is 1x and the selected queue and
storage-buffer limits support it. `SR_PERF_ALTERNATE=gputexdecode` alternates
that compute path with the asynchronous CPU snapshot decoder.
`SR_VULKAN_VERIFY_TEXTURE_DECODE=1` compares either optimized path with the CPU
oracle for every miss and aborts on any pixel difference.

The shared runtime also defines the fixed 64-byte `GeTextureRawJob` and
read-only snapshot payload accessors used by compute decoders. A
validation-only Vulkan shader currently matches the CPU oracle for linear and
swizzled 5650, 5551, 4444 and 8888 fixtures and for CLUT4/8/16/32 with every
palette entry format, shared/per-mip selection and shift/mask/start state. It
also matches DXT1/3/5 across partial blocks, endpoint modes and every color
and alpha selector. The opt-in runtime path dispatches that shader only before
the first pending batch that samples a cache entry, then copies the complete
RGBA8 result into the existing sampled image. Same-key content replacement,
LRU eviction, framebuffer aliases and guest-memory invalidation continue to
use the existing cache paths. Sources larger than 1 MiB, xBRZ-scaled textures,
unsupported devices and A/B-off intervals fall back to CPU decode. Compute
decode remains disabled by default until protected-title correctness and
timing gates pass.

At scaled internal resolutions, a capability-gated upload path transfers the
logical 512x512 RGBA image once and expands it with a nearest GPU blit. The
explicit CPU fallback expands into the physical staging buffer when required
format or blit support is unavailable. `SR_VULKAN_GPU_UPLOAD_SCALE=0` forces
that fallback at startup, while `SR_PERF_ALTERNATE=gpuupload` alternates the
same fallback at presentation boundaries for output-identical comparisons.
CSV v10 reports logical and framebuffer-specific host bytes, expanded physical
pixels, upload and fallback counts, and upload synchronization time.

## Performance-path status

| Path | Normal state | Diagnostic fallback | Promotion status |
|---|---|---|---|
| Scaled framebuffer upload expansion | Capability-gated GPU blit above 1x | `SR_VULKAN_GPU_UPLOAD_SCALE=0` or `gpuupload` A/B-off half | Reconciled shared baseline. The available 2x movie route proved exact output and a 75% framebuffer-transfer reduction, but timing stayed within variance and the gameplay window performed no framebuffer uploads. |
| Texture snapshot/decode scheduling | Bounded asynchronous CPU workers | `SR_VULKAN_SYNC_TEXTURE_DECODE=1` or `texdecode` A/B-off half | CPU baseline; independent of compute decode. |
| Model-space GPU T&L | Disabled | Existing CPU transform path | Opt-in with `SR_GPU_TNL=1`; exact Killzone routes pass on MoltenVK and KosmicKrisp, but second-title and later-driver gates are incomplete. |
| Compute texture decode | Disabled | Asynchronous CPU decoder | Opt-in with `SR_VULKAN_GPU_TEXTURE_DECODE=1`; exact protected verification passed, but synchronous submission regressed frontend tails and the CPU baseline stayed below the action threshold. |
| Through-sprite expansion | Disabled | Existing CPU sprite expansion | Validation-only with `SR_VULKAN_GPU_PRIMITIVE_EXPANSION=1`; timing did not beat variance. |

Do not infer a promotion from reduced upload bytes alone. A qualifying default
change must improve the complete guest-presentation distribution on named,
repeatable title states and preserve both logical and full-resolution output.

`SR_GPU_TNL=1` opts conservatively accepted transform-mode triangle draws into
a model-space vertex path. The shared frontend still decodes packed vertices,
morphing, and skinning and snapshots immutable GE state. Directly safe draws
run through the vertex shader. Finite triangle draws that need PSP
screen-grid acceptance or near-plane clipping run through two compute passes:
one transforms each deduplicated canonical vertex into the shared clip ABI,
and one emits fixed screen-space triangle slots with exact whole-primitive
rejection and near clipping. Canonical vertices, 32-bit submission indices,
packed state, clip vertices, and computed screen vertices remain in bounded
buffers. Unsupported inputs or backend declines preserve the complete CPU
path. `SR_PERF_ALTERNATE=gputnl` forces that CPU path at presentation
boundaries for A/B comparison. Preprocessing is enabled only when the selected
queue supports compute and the device exposes at least five per-stage and
per-set storage-buffer descriptors; direct GPU T&L remains available and
clipped draws fall back to the CPU on lower-limit devices.
Both the screen-space and canonical pipelines fetch packed RGBA8 as integers
and normalize explicitly in the vertex shader. This avoids driver-specific
UNORM conversion differences between the CPU and GPU T&L routes.
`SR_VULKAN_GPU_TNL_CLASSES` can restrict a diagnostic run to any
comma-separated subset of `direct`, `preprocess`, `points`, and `sprites`;
unselected classes take the CPU fallback, and normal runs enable all classes.
`SR_VULKAN_GPU_TNL_DIRECT_COMPUTE=1` instead transforms each canonical vertex
for direct draws once in the existing compute stage and consumes the projected
screen stream through an indexed graphics pipeline. It remains an opt-in
diagnostic for recovering performance with strict non-contracted arithmetic;
shader-output validation continues to exercise the direct vertex path. The
exact 50-frame Killzone route passed, but complete-frame mean and wall time
matched the CPU path, so this variant is not a promotion candidate.
Safe transform-mode points use the same canonical stream and a dedicated
vertex-ID path that expands each compact point control into two one-pixel
triangles. Points that need clipping, shader-output validation runs, and
backends without point expansion decline to the unchanged CPU path.
Safe transform-mode sprites similarly retain two compact controls and expand
to six vertices in the vertex shader. Expansion preserves screen-coordinate
flooring, second-control depth/color/fog, and the nearest-versus-linear
half-texel UV rule. Unsafe controls and validation runs use CPU fallback.
`SR_VULKAN_GPU_PRIMITIVE_EXPANSION=1` independently applies the same compact
submission idea to already decoded through-mode sprite controls. Its separate
vertex-ID shader preserves the through-mode depth/color/fog and UV rules while
reducing host vertex upload from six 32-byte expanded vertices to two 36-byte
controls. `SR_PERF_ALTERNATE=gpuprim` switches only this path at presentation
boundaries. It remains validation-only and disabled by default because the
measured Killzone route saved upload bytes but did not improve complete-frame
timing beyond run variance.
Transform-mode lines deliberately remain on CPU T&L. Their PSP oracle uses
discrete DDA steps with integer coordinate truncation and per-step attribute
evaluation, which cannot be represented exactly by vertex-only quad
expansion. The existing backend line quad remains a raster approximation, not
an eligibility basis for GPU T&L.
`SR_GPU_TNL_VALIDATE=1` requires `SR_GPU_TNL=1` and a device with
`vertexPipelineStoresAndAtomics`. It selects a separate validation shader,
writes each transformed result to a host-visible storage buffer, waits for the
submission, and compares it with the packed CPU reference. Normal runs use the
original shader and do not enable that optional Vulkan feature. The public
headless validation target also reads back all six generated point corners,
both nearest and linear six-corner sprite variants, and both preprocessing
compute shaders, comparing every result with the CPU oracle.
`SR_GPU_TNL_VALIDATE_EXACT=1` tightens live validation to numerical equality
for every non-padding component so sub-tolerance differences can be localized
without changing the normal cross-driver validation threshold.
`SR_GPU_TNL_VALIDATE_START=<present>` leaves validation writes and waits off
until the requested presentation count so a late deterministic route segment
can be diagnosed directly.

## Exact fragment ordering

Vulkan fixed-function state is used only when
`ge_vk_shader_direct_compatible()` proves that the PSP state is representable.
All other draws use a destination-aware fragment path:

1. End the current render pass.
2. Snapshot the prior RGBA8 color and D16 depth.
3. Restart rendering with blending disabled and depth comparison set to
   `ALWAYS`.
4. Evaluate texture combine, alpha/color tests, stencil fail/depth-fail/pass,
   PSP blend factors and equations, dithering, logic operations, native
   framebuffer packing, bit write masks, and depth preservation in
   `shaders/psp.frag`.

The manual path writes both the final color and either the source or preserved
destination depth. Batches that need destination state merge only within one
GE draw identity, preserving ordered behavior between draws. Clear operations
also use this path so partial color, alpha, and depth clears remain exact.

Destination alpha stores the PSP stencil value. RGB565, RGBA5551, RGBA4444,
and RGBA8888 packing occurs before native bit masks are applied. Direct-path
alpha writes are admitted only for destination-independent stencil
operations.

## Synchronization and ownership

Color targets are identified by address, stride, and format. Their framebuffer
binding is rebuilt whenever the depth address or stride changes. Compatible
framebuffer aliases remain on the GPU; incompatible format or stride
reinterpretations are materialized before software decode. CPU/DMA writes
invalidate every overlapping 512-row texture envelope after first
materializing GPU-newer render rows.

Framebuffer reads consider the 272 writable rows, while CPU writes also
invalidate the 512-row sampling envelope. Indexed texture cache keys include
the palette, and GPU-direct CLUT32 framebuffer reads use a separate palette
image. GE texture flushes invalidate cached state without forcing an
immediate submission; mutable texture uploads and cache eviction still wait
for pending draws that reference them.

Destination-aware and self-feedback draws use portable core Vulkan image
copies and barriers; no framebuffer-feedback-loop extension is required.
The first feedback snapshot in a submission copies the full texture envelope.
Later snapshots copy conservative accumulated color-write bounds, or reuse the
snapshot when the sampled region is unchanged. Presentation reuses an unchanged
GPU frame while still polling input; window and display events force a redraw,
as does a periodic swapchain revalidation. One
render-finished semaphore is used per swapchain image so a semaphore is not
reused while the presentation engine still owns it. All Vulkan objects are
destroyed before the SDL device shuts down.

## Build and shader checks

The backend requires SDL3 and the Vulkan SDK. `psp.vert` and `psp.frag` are
compiled with `glslangValidator`; checked-in SPIR-V initializers keep normal
runtime builds independent of the shader compiler.

```powershell
python tools\compile_vulkan_shaders.py
python tools\compile_vulkan_shaders.py --check
```

Equivalent Makefile targets are `shaders-vulkan` and
`check-shaders-vulkan`. The pure state classifier/packer test is:

```powershell
mingw32-make test-vulkan-shader-state
```

On a host with a Vulkan device and the Khronos validation layer, the headless
real-shader readback test is:

```text
make test-vulkan-tnl-validation
```

Title CMake builds can run the corresponding
`mga2_vulkan_shader_state` or `kzl_vulkan_shader_state` CTest.

## Validation

Set `SR_VK_VALIDATION=1` to require
`VK_LAYER_KHRONOS_validation` and route warning/error messages to stderr. The
runtime fails initialization if validation was requested but the layer is not
installed.

The Metal Gear Acid 2 integration has a deterministic visual gate:

```powershell
python tools\mga2\validate_vulkan.py `
  --memstick build\mga2\arena-d3d12-shadow-fix\memstick `
  --include-d3d12
```

It captures the Dead Cell cinematic at presentation 28000 and Arena gameplay
at presentation 38000, then compares whole frames plus character, stage,
map-preview, and card-UI regions against the software renderer. The fixed
thresholds are calibrated against the native-resolution D3D12 cross-check;
Vulkan does not receive a wider error budget. The gate sets `SR_VK_SCALE=1`
so exact native-resolution comparisons are independent of the user's quality
setting. Separate 4x scene and 8x boot runs validate high-resolution resource
scaling with the Khronos validation layer.

## Files

- `sdl3vk.c` / `sdl3vk.h`: SDL window/input, Vulkan instance/device,
  swapchain, validation, and presentation.
- `ge_gpu.c` / `ge_gpu.h`: persistent targets/depth, batching, descriptors,
  texture cache, feedback, guest-VRAM coherence, and GE hooks.
- `shader_state.c` / `shader_state.h`: pure direct/manual classification and
  the packed 112-byte push-constant ABI.
- `shaders/psp.vert` / `shaders/psp.frag`: Vulkan GE shaders.
- `psp_vert.inc` / `psp_frag.inc`: generated checked-in SPIR-V.

## Environment variables

- `SR_GPU=vk`: select Vulkan.
- `SR_GPU=sw`: select the software oracle.
- `SR_GPU_LOG=1`: log each Vulkan GE submit.
- `SR_GPU_SAFE=1`: materialize a presented target into guest VRAM; intended
  for guest-frame captures and diagnostics.
- `SR_GPU_FULL_FEEDBACK=1`: copy the complete feedback image on every
  self-feedback draw for differential diagnostics.
- `SR_GPU_PRESENT_REPEATS=1`: present unchanged GPU frames instead of reusing
  the last display for differential diagnostics.
- `SR_VK_SCALE=1` through `8`: override the configured internal resolution
  for diagnostics and native-resolution pixel gates.
- `SR_GPU_NORTT=1`: force framebuffer textures through materialization and
  software decode for alias diagnostics.
- `SR_GPU_PSP_SAMPLER=0`: disable integer-quantized PSP texture filtering.
- `SR_NOCULL=1`: disable Vulkan culling for diagnostics.
- `SR_VK_VALIDATION=1`: enable and require Khronos validation.

## Remaining renderer work

CPU-side GE transformation remains shared with the software reference; moving
T&L into a Vulkan vertex shader is a separate optimization, not a correctness
requirement.
