# SDL3 + Vulkan GPU renderer

This directory contains the runtime's original Vulkan renderer. It does not
reuse PPSSPP's GPU implementation. The software GE in `ge.c` remains the
pixel oracle and continues to perform vertex decode, transformation,
clipping, primitive acceptance, and cull-order handling. The Vulkan backend
receives accepted screen-space primitives through `GeBackend` and owns
rasterization, persistent render targets, texture caching, and presentation.

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
  Vulkan texture cache and optional xBRZ upload scaling; and
- direct swapchain presentation of GPU-resident targets.

The 512x512 logical color envelope is intentional. PSP titles commonly render
to a 512x272 region and later sample the same address as a 512x512 texture.
Color, depth, destination snapshots, framebuffer feedback, block transfers,
scissors, and presentation use the same configured internal-resolution scale.
Only the visible scaled 480x272 area is copied to the swapchain.

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
