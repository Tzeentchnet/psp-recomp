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
- software-GE framebuffer upload so `SR_GPU=d3d12` is usable before the HLSL renderer
  lands.

`shaders/psp_vs.hlsl` and `shaders/psp_ps.hlsl` are the native Shader Model 6 ports of
the Vulkan GE shaders. Their checked-in `*.inc` files contain stripped DXIL consumed by
`device.cpp`. Regenerate or verify them with:

```text
python tools/compile_d3d12_shaders.py [--dxc path/to/dxc]
python tools/compile_d3d12_shaders.py [--dxc path/to/dxc] --check
```

The Makefile exposes the same operations as `shaders-d3d12` and
`check-shaders-d3d12`; D3D12-enabled root CMake builds expose
`kzl_d3d12_shaders` and `kzl_d3d12_shaders_check`.

Set `SR_D3D12_DEBUG=1` to request the D3D12 debug layer. Add
`SR_D3D12_GPU_VALIDATION=1` to enable GPU-based validation. Both settings fail
explicitly when the requested debug interface is unavailable.

Later phase-4 tasks bind these shaders through root signatures, vertex decoding, PSO and
texture caches, and GPU-resident framebuffer presentation behind the same descriptor.

The root project keeps this dependency optional:

```text
cmake -S . -B out/build/d3d12 -DKZL_ENABLE_D3D12=ON -DSDL3_DIR=<SDL3 package>/cmake
```

The fork Makefile enables `SR_D3D12` together with its existing SDL3/Vulkan build.
