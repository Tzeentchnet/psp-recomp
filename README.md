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
The generated registration block is an immutable address-sorted dispatch table; computed calls use
a bounded binary search instead of populating a fixed-capacity runtime hash.
With `--split --hot-functions=<report>`, lines beginning with guest addresses in the report are
grouped into `_hot_NNN.c` files and the remainder into `_cold_NNN.c` files, allowing a target build
to apply different optimization levels without mixing hot and cold functions in one object.

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
The KZL root build supplies `SR_EXPECTED_ISO_SIZE` and `SR_EXPECTED_ISO_SHA1`;
standalone boot validates `PSP_ISO` against both before allocating guest memory.
Generic users of the fork omit those definitions and retain the toolkit's
game-agnostic behavior.

### 5. Setup Game Assets & Run
1. Place your dumped game UMD ISO in the directory containing `mygame.exe` (name it `game.iso`, or set the `PSP_ISO` environment variable to point to the ISO path).
2. Run the game from a command prompt:
```bash
build/mygame/mygame.exe --image build/mygame/mygame_image.bin 0x08804000 <entry_point_address> none none --gui
```

## Runtime Controls

SDL3 maps connected gamepads automatically, and gamepad, mouse, and keyboard
input can be mixed at any time. The default PC mappings are:
- **WASD**: PSP analog stick movement
- **Arrow Keys**: PSP D-pad
- **Left Mouse / F**: Square (fire)
- **Right Mouse / E**: R (aim)
- **Middle Mouse / R**: Triangle (reload)
- **Mouse X1 / G**: Circle (grenade)
- **Mouse X2 / Q**: L (strafe/roll modifier)
- **Space**: Cross (interact/confirm)
- **Enter / Shift**: Start / Select

Gamepads use the left stick, D-pad, standard south/east/west/north face
buttons, and either shoulder or trigger for L/R.

On first windowed launch, the runtime opens a native graphics settings dialog
and writes `graphics.ini` beside the executable. D3D12 and 2x internal
resolution (960x544) are the defaults:

```ini
renderer = d3d12
internal_resolution = 2
```

Run `kzl.exe --configure` to reopen the dialog without starting the game.
`renderer` accepts `auto`, `d3d12`, `sw`, or `vk`; unavailable backends fail
explicitly at startup. `internal_resolution` accepts every integer from 1
through 8 and applies to D3D12 color/depth targets, feedback snapshots,
copies, readback, and presentation. PSP-visible framebuffer addresses,
coordinates, and captures remain at their original logical resolution.
On Variable Rate Shading hardware, direct draws retain scaled geometry,
coverage and D16 depth while shading at approximately one fragment per logical
PSP pixel. Set `SR_D3D12_VRS=0` to force full-rate shading; on the measured RTX
5070, the default 2x path sustains 65.51-69.70 FPS while full-rate shading
drops moving gameplay to about 40 FPS.

The runtime also writes `input.ini` beside the executable. Edit that portable
file and restart to rebind keyboard/gamepad controls or change the radial
analog response:

```ini
keyboard.cross = space
keyboard.analog_up = W
mouse.square = left
mouse.r = right
gamepad.cross = south
gamepad.l = left_shoulder, left_trigger
analog.deadzone = 0.24
analog.curve = 1.0
analog.invert_y = false
twin_stick.enabled = false
movement.deadzone = 0.24
movement.curve = 1.0
movement.invert_y = false
aim.deadzone = 0.24
aim.curve = 1.0
aim.invert_x = false
aim.invert_y = false
```

`twin_stick.enabled = true` enables the experimental gamepad-only scheme.
While a controller is connected, the movement stick is converted to PSP D-pad
directions with hysteresis. The aim stick is carried through a native side
channel and replaces the base on-foot facing vector before the game's original
target and auto-aim correction. The runtime temporarily overlays the hidden
D-pad movement profile without marking it dirty and restores the original
mappings when the controller disconnects. This experiment has not yet passed
the full gameplay validation matrix, so back up saves before testing it.

Title integrations without the Killzone-specific control extension can compile
the runtime with `SR_ENABLE_KZL_TWIN_STICK=0`. That build keeps standard PSP
button and analog input, omits twin-stick/reversal controls from the native
dialog and generated `input.ini`, and ignores stale twin-stick settings in an
existing configuration. The generic fork default remains enabled for Killzone.

Run `kzl.exe --configure-input` to edit all twelve gamepad action bindings and
the twin-stick/reversal options in the native controller dialog. Each action
accepts the same comma-separated names as `input.ini`, including combinations
such as `west, right_trigger` for Square/fire. `kzl.exe --configure` opens the
graphics dialog followed by controller settings.
Turning twin-stick on from an unchecked state loads the tested default preset:
`left_shoulder` for PSP L/strafe, `left_trigger` for PSP R/aim,
`west, right_trigger` for Square/fire, and vertical aim reversal. It does not
change the built-in controls while twin-stick remains disabled.

The running game checks `input.ini` four times per second. Saving from a second
`kzl.exe --configure-input` process hot-reloads bindings, twin-stick enablement,
response settings, and aim reversal without restarting. Invalid edits are
reported in the runtime log and the last valid configuration remains active
until a corrected file is saved. The remapper remains open after saving so
multiple adjustments can be tested; select **Close** when finished.
Filesystem checks run on a sleeping watcher thread; the presentation path only
checks an atomic reload notification and performs no periodic file I/O.
Set `SR_INPUT_HOT_RELOAD=0` to disable the watcher for diagnostics.

Bindings accept up to four comma-separated inputs or `none`. Keyboard inputs
are `A`-`Z`, `0`-`9`, `enter`, `left_shift`, `right_shift`, `up`, `right`,
`down`, `left`, `space`, `tab`, `backspace`, `left_ctrl`, and `right_ctrl`.
Mouse inputs are `left`, `right`, `middle`, `x1`, and `x2`.
Gamepad inputs are `south`, `east`, `west`, `north`, `start`, `back`,
`left_shoulder`, `right_shoulder`, `left_trigger`, `right_trigger`, the four
`dpad_*` names, `left_stick`, and `right_stick`. Invalid settings stop startup
with a line-specific error instead of silently falling back. Set
`SR_INPUT_CONFIG=<path>` to use another file, or `SR_INPUT_CONFIG=0` to use
built-in defaults without reading or writing a file. An untouched generated
v1, v2, or v3 config is migrated automatically to the v4 PC defaults;
customized older files retain their bindings and inherit the disabled
twin-stick defaults.
Set `SR_GRAPHICS_CONFIG=<path>` or `SR_SETTINGS_DIR=<directory>` for isolated
automation. `SR_GRAPHICS_CONFIG=0` uses the built-in graphics defaults without
opening the first-run launcher or reading/writing a file.

## Environment Settings

Set these environment variables before running the game to control runtime features:
- Audio uses the native Windows XAudio2 2.9 backend at 44.1 kHz stereo. Eight
  256-frame source-buffer slots provide up to 46.44 ms of device coverage.
  The feeder submits only available guest PCM instead of pre-rendering silence,
  and Output2 preserves the PSP's two-buffer completion contract. PSP channels
  still mix through the shared saturation ring before submission.
- `SR_AUDIO_LOG=1`: Print device-buffer lead, guest queue/resync/drop counters,
  played time, and host-clock drift every three seconds and at shutdown.
- `SR_NOAUDIO=1`: Disable host playback. Blocking PSP audio calls retain their
  virtual-duration scheduler fallback, which is useful for deterministic tests.
- `SR_MPEGLOG=1`: Print PMF ring-buffer, H.264 negotiation, frame, audio, and
  A/V-drift diagnostics.
- `SR_NO_DIALOGS=1`: Suppress native message boxes and choose their configured
  default button. Netconf and Game Sharing remain deterministic offline cancels.
- `SR_SAVEDATA_MIGRATE=1`: After loading a legacy plaintext save produced by an
  older runtime build, rewrite it in PSP/PPSSPP-compatible secure format using
  the game's live key.
- `SR_MEMSTICK_STATE=inserted|removed`: Select the initial Memory Stick state.
  The default is `inserted`; `removed` matches PPSSPP's `--no-memstick`
  driver, device, FAT, callback, capacity, and write-protection responses.
- When `SR_GPU` is unset, the renderer comes from `graphics.ini`; `auto` uses
  D3D12 in a D3D12-enabled build and otherwise retains the build's default.
- `SR_GPU=vk`: Use the Vulkan GE backend (SDL3/Vulkan build).
- `SR_GPU=sw`: Explicitly use the reference software rasterizer.
- `SR_GPU=d3d12`: Use the SDL3/D3D12 device backend when compiled with `SR_D3D12`.
  Successful primitive hooks suppress CPU rasterization and submit native draws into GPU-owned targets. Each batch
  captures a 160-byte fragment block. Clear, opaque, source-alpha, additive, subtractive, min/max, and compatible
  fixed-factor draws use native D3D12 color/depth state without destination copies. Unusual doubled factors,
  per-bit masks, stencil, logic operations, and post-blend dithering retain the ordered BGRA8/R16 snapshot path.
  Cached color targets use a scaled 512x512 texture envelope for PSP power-of-two framebuffer sampling while
  rendering the visible 512x272 region at the selected 1x-8x internal resolution. CLUT32 reinterpretations of a live 8888 target remain
  GPU-resident: ordered feedback snapshots and per-draw palettes replace stale guest-memory texture decodes.
  The resizable swapchain preserves the exact 480:272 display aspect and clears letterbox or pillarbox space to
  black. The shared CPU frontend blends all morph attributes before eight-bone skinning and tessellates
  Bezier/B-spline commands with the GE's quantized scalar evaluator.
- `SR_D3D12_FRAMEBUFFER_LOG=1`: Print persistent color/depth target allocation and cache statistics.
- `SR_D3D12_FRAMEBUFFER_SAFE=1`: Read cached color targets back after presentation to validate GPU/CPU ownership.
- `SR_D3D12_SCALE=1` through `8`: Override `graphics.ini` for one process.
- `SR_GPU_PSP_SAMPLER=0`: Disable the default PSP-quantized nearest/bilinear
  shader path and use host hardware filtering for A/B diagnostics.
  Non-mipmapped GPU textures otherwise use the software renderer's 8-bit
  bilinear weights, truncation order, and explicit wrap/clamp coordinates on
  both D3D12 and Vulkan. The same path applies PSP integer texture-function
  and fog math. Logical D3D12 captures are canonicalized through the active
  PSP framebuffer format before golden comparison; full internal-resolution
  captures remain raw.
- `SR_D3D12_VRS=0`: Disable coarse shading for direct scaled draws. This keeps
  every fragment at the selected internal resolution at a substantial cost.
- `SR_D3D12_STAGE_LOG=1`: Report 120-frame CPU averages for fence/begin,
  vertex upload, framebuffer and texture preparation, draw recording,
  presentation, and submission.
- `SR_D3D12_PSO_LOG=1`: Print ubershader, asynchronous GE PSO, pipeline-library, and final cache statistics.
- `SR_D3D12_PSO_CACHE=<path>`: Request experimental pipeline-library disk persistence.
  Serialized libraries remove the device on at least one validated driver, so
  the additional `SR_D3D12_UNSAFE_PSO_CACHE=1` acknowledgement is required.
  The exact ubershader fallback and asynchronous in-memory cache remain active
  without disk persistence.
- `SR_D3D12_WARP=1`: Select Microsoft WARP instead of a hardware adapter.
- **F10** (D3D12 window): Write a complete textual GE state dump in the working directory.
  PIX for Windows can display nested list/draw events embedded in each D3D12 command list;
  install it with `winget install --id Microsoft.PIX --exact`.
- `PSPRECOMP_CAPTURE_MANIFEST=<path>` and `PSPRECOMP_CAPTURE_DIR=<directory>`: Capture the
  strict 50-frame golden manifest. With the D3D12 backend, frames are read back from the
  scaled GPU display texture after presentation conversion and center-sample it back to
  480x272; other presenters use the
  authoritative guest framebuffer. A successful sequence writes `capture.complete`.
- `SR_D3D12_CAPTURE_INTERNAL=1`: Keep D3D12 captures at the selected
  full internal resolution instead of center-sampling them to 480x272.
- `SR_PADSCRIPT=<path>`: Inject deterministic controller pulses. Golden pad-script v3 entries
  contain a PPSSPP vblank index, a runtime presentation index, a shared PSP button mask, and
  backend-specific widths; the runtime consumes presentation-timed values so turbo scheduler
  variance cannot consume inputs before a menu appears. Legacy three-column and v2 scripts retain
  runtime-vblank timing.
- `SR_PADLOG=1`: Print scripted button-mask edges with both presentation and
  vblank counters.
- `SR_INPUT_LOG=1`: Print live logical button/analog edges and whether a
  gamepad is connected.
- `SR_FPS_COUNTER=1`: Update the GDI, Vulkan, or D3D12 window title every
  500 ms with rolling presentation FPS and average frame time.
- `SR_FBSNAP_PRESENT=<interval>`: Write an eight-file rotating set of software
  framebuffer snapshots at the requested presentation interval.
- `SR_FBCAPTURE_PRESENTS=<p1,p2,...>`: Write software framebuffer snapshots at
  up to 32 exact positive presentation numbers. Duplicate or malformed values
  fail startup.
- `SR_FAST_FORWARD_PRESENT=<present>`: Run scheduler and presentation pacing as fast as the host
  allows until the requested presentation, keep host audio dormant during that phase, then align
  virtual time to the host clock and start real-time XAudio2 playback. This is intended for
  deterministic in-game testing rather than normal play.
- `SR_PERF_LOG=1`: Measure presentation intervals and split time between game/GE work and the
  graphics backend. `SR_PERF_START=<present>` selects the first measured presentation and
  `SR_PERF_WINDOW=<1..600>` selects the reporting window. `SR_PERF_MIN_FPS=<fps>` terminates with
  failure when a completed window falls below the required average, while
  `SR_EXIT_AFTER_PRESENT=<present>` makes unattended benchmarks exit cleanly with final audio
  synchronization statistics. `SR_PERF_FILE=<path>` additionally writes
  per-presentation CSV with host, backend, and fenced D3D12 GPU time;
  `SR_PERF_FRAME_BUDGET_MS=<milliseconds>` overrides its over-budget threshold.
- `PSP_VFPU_TABLES=<directory>`: Override the `assets/vfpu` lookup-table directory. The golden
  harness sets this explicitly so runtime execution does not depend on its working directory.
- `SR_INTERP_MISS_LOG=<path>`: Write each distinct guest target that missed the generated
  dispatch table once. Version 2 entries retain the generated module prefix so
  overlapping overlay addresses remain distinct. The runtime executes
  supported misses through the reference interpreter, hands nested targets
  back to generated code when they enter the active native table, and reuses
  the tested VFPU instruction engine for supported interpreted VFPU operations.
- `SR_INTERP_PROFILE=<path>`: Write aggregate calls, guest instructions,
  syscalls, native handoffs, chunk limits, returns, and failures for every
  module-qualified fallback target. This does not change the v2 miss log.
- `SR_INTERP_MAX_STEPS=<1..100000000>`: Bound one interpreter fallback invocation; the default
  is 1,000,000 instructions. A syscall, unsupported instruction, memory fault, or exhausted
  bound is a fatal error unless the diagnostic-only `SR_DISPATCH_PERMISSIVE=1` escape hatch is set.
- `SR_DISABLE_NATIVE_MODULES=*` or a comma-separated generated-table name
  list: Keep matching policy-selected modules interpreted for differential
  diagnostics. Normal validation must leave this unset.
- `SR_TRACE_RAM=<path>`: With bounded instruction tracing, write the
  `0x08800000..0x09ffffff` RAM snapshot when the trace reaches its step limit.
  The snapshot is game-derived and must not be committed.
- `SR_GPU_GE` / `SR_VIDEO=gdi`: Legacy Vulkan/software and presenter overrides.
- `SR_GPU_LOG=1`: Print GPU cache flushes and batch information to the console. D3D12
  includes list/draw identity, command PC/opcode, per-draw batch/vertex counts, presents,
  and a shutdown summary.
- `SR_GPU_LOG_START=<present>` / `SR_GPU_LOG_END=<present>`: Restrict verbose
  D3D12 draw/present logging to an inclusive presentation range.
- `SR_IOLOG=1`: Print all file system access logs.
