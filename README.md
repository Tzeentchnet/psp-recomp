# Standalone PSP Static Recompiler Toolkit

This toolkit lets you translate a PSP game's relocatable executable (PRX/ELF) into C, link it with a native runtime (scheduler, HLE kernel, audio/video decoder, GPU renderer), and compile it into a native Windows executable.

## License & attribution (read this first)

This project is **GPL-2.0-or-later** because it is a derivative/combined work of
**[PPSSPP](https://github.com/hrydgard/ppsspp)** (Henrik Rydgård and
contributors, GPL-2.0-or-later). See [`../LICENSE`](../LICENSE) and
[`../CREDITS.md`](../CREDITS.md).

To be clear about what is and is not this project's own work:

- **Rendering is this project's own backend.** The active renderer is the
  from-scratch SDL3 + Vulkan backend in `src/rt/gpu_sdl3vk/`. It does **not**
  reuse PPSSPP's GPU. (An earlier experiment, `src/rt/gpu_vk/`, *did* wrap
  PPSSPP's `GPU_Vulkan` behind a C ABI, but it was never activated and has been
  removed.) The GE semantics it implements were validated against PPSSPP's
  software renderer, but the code is original.
- **PPSSPP is still used in substantial ways**, which is why the GPL applies:
  it is the verification oracle (`PPSSPPHeadless.exe`), it supplies the bundled
  system fonts, the sceMpeg/PSMF runtime (`src/rt/mpeg.c`) is a direct port of
  PPSSPP's `Core/HLE/sceMpeg.cpp` (GPLv2+), and the ATRAC3/ATRAC3+ path uses
  PPSSPP's standalone FFmpeg-derived decoder (LGPL-2.1-or-later; see
  `third_party/at3_standalone/COPYING.LGPLv2.1`).

The novel part here is the offline MIPS-to-C static recompiler, the runtime, and
the GPU backend; significant HLE behaviour is ported from or modelled on PPSSPP.

If you redistribute anything built with this toolkit, the GPL requires you to
provide complete corresponding source to your recipients.

## Layout

- `tools/`: Offline compilation scripts (ELF loader, control flow analysis, MIPS-to-C code generator, and HLE import-stub mapper).
- `src/`: Native runtime libraries and the from-scratch SDL3 + Vulkan GPU renderer (`src/rt/gpu_sdl3vk`).
- `font/`: Replacement system fonts from PPSSPP (needed for native text dialogs).
- `SDL3.dll`: Prebuilt SDL3 library.
- `Makefile`: Build driver for compiling the runtime and your recompiled game.

## Prerequisites

- **OS**: Windows 10/11 x64 with a Vulkan 1.1- or D3D12-capable GPU.
- **Python 3**: For running the offline compilation scripts.
- **DirectX Shader Compiler**: `dxc.exe` from the Windows SDK when regenerating the
  checked-in D3D12 shader includes.
- **MSYS2 (UCRT64)**:
  - GCC: `pacman -S mingw-w64-ucrt-x86_64-gcc`
  - SDL3: `pacman -S mingw-w64-ucrt-x86_64-sdl3`
  - Vulkan Loader: `pacman -S mingw-w64-ucrt-x86_64-vulkan-loader`
  - LLD Linker (Recommended; prevents MinGW's ld from crashing on huge generated files): `pacman -S mingw-w64-ucrt-x86_64-lld`
- **Game Executable**: A decrypted ELF file of the game (obtained from `EBOOT.PBP` using PPSSPP or PSDecrypter).

## Recompiling a Game

### 1. Relocate the ELF
Since PSP executables are relocatable PRXs, you first need to rebase the game's ELF to its target virtual base address (usually `0x08804000`) and dump it as a flat memory image:
```bash
python tools/prxload.py <path_to_eboot.elf> 0x08804000 --out=build/mygame/mygame_image.bin
```

### 2. Map Library Imports
Resolve MIPS NIDs (Name IDs) in the ELF to HLE function names:
```bash
python tools/imports.py <path_to_eboot.elf> 0x08804000 --toml=build/mygame/mygame_imports.toml
```

### 3. Generate C Code
Run the code generator to translate MIPS machine instructions into native C code:
```bash
python tools/codegen.py <path_to_eboot.elf> build/mygame/mygame_recomp.c --base=0x08804000
```
This produces a large C file containing translated functions mapped to their guest memory locations.

### 4. Compile the Runtime & Game
Build the game and compile the runtime objects:
```bash
mingw32-make GAME_NAME=mygame GAME_ELF=<path_to_eboot.elf> GAME_BASE=0x08804000 GAME_ENTRY=<entry_point_address>
```
*Note: Make sure `GAME_ENTRY` matches the entry point address of your specific game ELF.*

The output executable will land at `build/mygame/mygame.exe`.

### KZL Standalone Boot

The KZL branch can boot its pre-relocated `prxload.py` image without a PPSSPP trace:
```bash
<driver> --boot <image.bin> 08804000 08804000 none --gui
```
`--boot` enables the scheduler, creates the PSP-compatible `module_start` thread, and supplies KZL's
`"modulename"` argument. Keep using `--image` with a PPSSPP reference trace for differential tracing.

### 5. Setup Game Assets & Run
1. Place your dumped game UMD ISO in the directory containing `mygame.exe` (name it `game.iso`, or set the `PSP_ISO` environment variable to point to the ISO path).
2. Run the game from a command prompt:
```bash
build/mygame/mygame.exe --image build/mygame/mygame_image.bin 0x08804000 <entry_point_address> none none --gui
```

## Runtime Controls

If you have a gamepad connected, SDL3 will map it automatically. Otherwise, use these keyboard mappings:
- **Arrow Keys**: PSP D-pad and analog stick
- **X / Z**: Cross (A) / Circle (B)
- **A / S**: Square (X) / Triangle (Y)
- **Q / W**: L-Trigger / R-Trigger
- **Enter / Shift**: Start / Select

On first windowed launch, the runtime writes
`%LOCALAPPDATA%\KZLRecomp\input.ini`. Edit that file and restart to rebind
keyboard/gamepad controls or change the radial analog response:

```ini
keyboard.cross = X
keyboard.analog_up = up
gamepad.cross = south
gamepad.l = left_shoulder, left_trigger
analog.deadzone = 0.24
analog.curve = 1.0
analog.invert_y = false
```

Bindings accept up to four comma-separated inputs or `none`. Keyboard inputs
are `A`-`Z`, `0`-`9`, `enter`, `left_shift`, `right_shift`, `up`, `right`,
`down`, `left`, `space`, `tab`, `backspace`, `left_ctrl`, and `right_ctrl`.
Gamepad inputs are `south`, `east`, `west`, `north`, `start`, `back`,
`left_shoulder`, `right_shoulder`, `left_trigger`, `right_trigger`, the four
`dpad_*` names, `left_stick`, and `right_stick`. Invalid settings stop startup
with a line-specific error instead of silently falling back. Set
`SR_INPUT_CONFIG=<path>` to use another file, or `SR_INPUT_CONFIG=0` to use
built-in defaults without reading or writing a file.

## Environment Settings

Set these environment variables before running the game to control runtime features:
- Audio uses the native Windows XAudio2 2.9 backend at 44.1 kHz stereo. Four
  256-frame source buffers bound the device queue to 23.22 ms; PSP output
  channels still mix through the shared saturation ring before submission.
- `SR_AUDIO_LOG=1`: Print device-buffer lead, guest queue/resync/drop counters,
  played time, and host-clock drift every three seconds and at shutdown.
- `SR_NOAUDIO=1`: Disable host playback. Blocking PSP audio calls retain their
  virtual-duration scheduler fallback, which is useful for deterministic tests.
- `SR_GPU=vk`: Use the Vulkan GE backend (SDL3/Vulkan build).
- `SR_GPU=sw`: Use the reference software rasterizer.
- `SR_GPU=d3d12`: Use the SDL3/D3D12 device backend when compiled with `SR_D3D12`.
  The software GE remains authoritative while native batches are staged. Each batch captures a 160-byte
  destination-aware fragment block; the HLSL path reads ordered BGRA8 color and R16 depth snapshots to reproduce
  PSP stencil, depth, blending, dithering, logic operations, native packing, and component masks. The resizable
  swapchain preserves the exact 480:272 display aspect and clears letterbox or pillarbox space to black. The shared
  CPU frontend blends all morph attributes before eight-bone skinning and tessellates Bezier/B-spline commands with
  the GE's quantized scalar evaluator before emitting ordinary backend triangles, lines, or points. The vertex
  shader raw-loads that canonical 32-byte stream through a root SRV; primitives from one logical GE command share
  one captured state record, and unchanged fragment blocks reuse their previous root constants.
- `SR_D3D12_FRAMEBUFFER_LOG=1`: Print persistent color/depth target allocation and cache statistics.
- `SR_D3D12_FRAMEBUFFER_SAFE=1`: Read cached color targets back after presentation to validate GPU/CPU ownership.
- `SR_D3D12_DESTINATION_SAFE=1`: Copy and bind color/depth destination snapshots for every staged batch. This is
  an intentionally expensive resource-transition diagnostic; native draw submission remains disabled.
- `SR_D3D12_PSO_LOG=1`: Print ubershader, asynchronous GE PSO, pipeline-library, and final cache statistics.
- `SR_D3D12_PSO_CACHE=<path>`: Override the default
  `%LOCALAPPDATA%\KZLRecomp\cache\d3d12-pipelines.bin` pipeline-library path. Set it to `0` to disable disk
  persistence; the exact ubershader fallback and asynchronous in-memory cache remain active.
- `SR_D3D12_WARP=1`: Select Microsoft WARP instead of a hardware adapter.
- **F10** (D3D12 window): Write a complete textual GE state dump in the working directory.
  PIX for Windows can display nested list/draw events embedded in each D3D12 command list;
  install it with `winget install --id Microsoft.PIX --exact`.
- `PSPRECOMP_CAPTURE_MANIFEST=<path>` and `PSPRECOMP_CAPTURE_DIR=<directory>`: Capture the
  strict 50-frame golden manifest. With the D3D12 backend, frames are read back from the
  480x272 GPU display texture after presentation conversion; other presenters use the
  authoritative guest framebuffer. A successful sequence writes `capture.complete`.
- `SR_PADSCRIPT=<path>`: Inject deterministic controller pulses. Golden pad-script v3 entries
  contain a PPSSPP vblank index, a runtime presentation index, a shared PSP button mask, and
  backend-specific widths; the runtime consumes presentation-timed values so turbo scheduler
  variance cannot consume inputs before a menu appears. Legacy three-column and v2 scripts retain
  runtime-vblank timing.
- `PSP_VFPU_TABLES=<directory>`: Override the `assets/vfpu` lookup-table directory. The golden
  harness sets this explicitly so runtime execution does not depend on its working directory.
- `SR_GPU_GE` / `SR_VIDEO=gdi`: Legacy Vulkan/software and presenter overrides.
- `SR_GPU_LOG=1`: Print GPU cache flushes and batch information to the console. D3D12
  includes list/draw identity, command PC/opcode, per-draw batch/vertex counts, presents,
  and a shutdown summary.
- `SR_IOLOG=1`: Print all file system access logs.
