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
- software-GE framebuffer upload so `SR_GPU=d3d12` is usable before the HLSL renderer
  lands.

`vertex.cpp` deliberately consumes the existing `GeVtx` backend seam instead of
duplicating PSP vertex decoding. `ge.c` remains authoritative for all raw attribute
formats, skinning, through mode, indices, transform/lighting, clipping, and primitive
assembly. Captured vertices and primitive-boundary records are copied into the active
fenced upload-ring partition during presentation. The hooks still return zero, so the
reference software rasterizer remains authoritative until native PSOs are bound.

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

Later phase-4 tasks bind the uploaded stream through PSO and texture caches and add
GPU-resident framebuffer presentation behind the same descriptor.

The root project keeps this dependency optional:

```text
cmake -S . -B out/build/d3d12 -DKZL_ENABLE_D3D12=ON -DSDL3_DIR=<SDL3 package>/cmake
```

The fork Makefile enables `SR_D3D12` together with its existing SDL3/Vulkan build.
