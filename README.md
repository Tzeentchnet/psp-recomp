# PSP Static Recompiler Toolkit and Runtime

This toolkit lets you translate a PSP game's relocatable executable (PRX/ELF)
into C, link it with a native runtime (scheduler, HLE kernel, audio/video
decoder, GPU renderer), and compile it into a native Windows or Apple Silicon
executable.

## Maintained branch and integrations

`main` is the maintained integration branch. It contains the former `kzl`
runtime history together with the title-neutral module loading, interpreter
fallback, renderer, input, diagnostics, and code-generation work added for
additional PSP recompilations. The `kzl` branch remains available as a
compatibility branch, but new integrations should use `main` in branch
metadata and continue pinning an exact commit for reproducible builds.

The shared runtime is currently used by:

- [Killzone: Liberation Recomp](https://github.com/Tzeentchnet/KillzoneLiberation-Recomp)
- [Metal Gear Acid Recomp](https://github.com/Tzeentchnet/MetalGearAcid-Recomp)

Compared with the original toolkit baseline, the integrated branch adds
standalone module boot, relocatable runtime PRX support, bounded
interpreter/native handoff, D3D12 rendering, PSP-quantized GPU sampling,
XAudio2 and native media decode, secure savedata, portable graphics/input
configuration, and deterministic validation/profiling tools.

## License & attribution (read this first)

This project is **GPL-2.0-or-later** because it is a derivative/combined work of
**[PPSSPP](https://github.com/hrydgard/ppsspp)** (Henrik Rydgård and
contributors, GPL-2.0-or-later). See [`LICENSE`](LICENSE) and
[`CREDITS.md`](CREDITS.md).

To be clear about what is and is not this project's own work:

- **Rendering uses this project's own backends.** The primary Windows path is
  the D3D12 backend in `src/rt/gpu_d3d12/`; the SDL3 + Vulkan backend in
  `src/rt/gpu_sdl3vk/` is the Apple Silicon path and runs over Metal through
  MoltenVK.
  Neither reuses PPSSPP's GPU. (An earlier experiment, `src/rt/gpu_vk/`, *did*
  wrap PPSSPP's `GPU_Vulkan` behind a C ABI, but it was never activated and has
  been removed.) Their GE semantics were validated against PPSSPP's software
  renderer, but the backend code is original.
- **PPSSPP is still used in substantial ways**, which is why the GPL applies:
  it is the verification oracle (`PPSSPPHeadless.exe`), it supplies the bundled
  system fonts, the sceMpeg/PSMF runtime (`src/rt/mpeg.c`) is a direct port of
  PPSSPP's `Core/HLE/sceMpeg.cpp` (GPLv2+), and the ATRAC3/ATRAC3+ path uses
  PPSSPP's standalone FFmpeg-derived decoder (LGPL-2.1-or-later; see
  `third_party/at3_standalone/COPYING.LGPLv2.1`).

The novel part here is the offline MIPS-to-C static recompiler, the runtime, and
the GPU backends; significant HLE behaviour is ported from or modelled on
PPSSPP.

If you redistribute anything built with this toolkit, the GPL requires you to
provide complete corresponding source to your recipients.

## Layout

- `tools/`: Offline compilation scripts (ELF loader, control flow analysis, MIPS-to-C code generator, and HLE import-stub mapper).
- `src/`: Native runtime libraries, the primary D3D12 renderer
  (`src/rt/gpu_d3d12`), and the SDL3 + Vulkan renderer
  (`src/rt/gpu_sdl3vk`).
- `font/`: Replacement system fonts from PPSSPP (needed for native text dialogs).
- `SDL3.dll`: Prebuilt SDL3 library.
- `Makefile`: Build driver for compiling the runtime and your recompiled game.

## Documentation and agent workflow

Start with the [PSP-to-PC recompilation knowledge base](docs/README.md) when
bringing up another title or changing shared runtime behavior. It consolidates:

- The proven [porting workflow](docs/PORTING_WORKFLOW.md).
- Cross-title [shared findings](docs/SHARED_FINDINGS.md).
- The [validation and evidence ladder](docs/VALIDATION.md).
- [Safe function recognition and exact local reuse](docs/FUNCTION_REUSE.md).
- A copyable [new-title project template](docs/TITLE_PROJECT_TEMPLATE.md).
- [Killzone: Liberation and Metal Gear Acid title findings](docs/titles/README.md).

Workspace-aware AI agents can invoke the
[`psp-recomp-port`](.github/skills/psp-recomp-port/SKILL.md) skill to follow
the same workflow, ownership boundaries, stop conditions, and validation
requirements.

## Prerequisites

- **Python 3.11 or newer**: For the offline compilation scripts.
- A decrypted game ELF (obtained from `EBOOT.PBP` using PPSSPP or
  PSDecrypter).

### Windows

- Windows 10/11 x64 with a Vulkan 1.1- or D3D12-capable GPU.
- DirectX Shader Compiler (`dxc.exe`) from the Windows SDK when regenerating
  the checked-in D3D12 shader includes.
- MSYS2 (UCRT64):
  - GCC: `pacman -S mingw-w64-ucrt-x86_64-gcc`
  - SDL3: `pacman -S mingw-w64-ucrt-x86_64-sdl3`
  - Vulkan Loader: `pacman -S mingw-w64-ucrt-x86_64-vulkan-loader`
  - LLD Linker (Recommended; prevents MinGW's ld from crashing on huge generated files): `pacman -S mingw-w64-ucrt-x86_64-lld`

### Apple Silicon

- macOS on arm64 with Xcode Command Line Tools and Homebrew.
- SDL3, the Vulkan loader and headers, MoltenVK, pkg-config, and a current
  Python:

```bash
brew install python@3.13 pkg-config sdl3 vulkan-loader vulkan-headers molten-vk
```

The Vulkan renderer uses MoltenVK's portability driver, so rendering is backed
by Metal without a separate native Metal renderer. Homebrew's Vulkan loader
normally discovers the MoltenVK ICD automatically. New macOS graphics
configurations use deterministic Automatic quality; legacy configurations
migrate to Manual and retain their exact resolution, texture, and
anti-aliasing values. If the loader does not find MoltenVK, set:

```bash
export VK_ICD_FILENAMES="$(brew --prefix molten-vk)/etc/vulkan/icd.d/MoltenVK_icd.json"
```

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
Optionally match reviewed library identities before generation:
```bash
python tools/function_catalog.py match <path_to_eboot.elf> --base 0x08804000 --output build/mygame/function_matches.toml
```
Normalized matches require review and never authorize generated-code reuse.

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
Set `PSP_RECOMP_CODEGEN_CACHE` or pass `--cache-dir=<private-path>` to reuse
generated functions only when their exact code, flow, generator, ABI, hints,
and options match. See [safe function reuse](docs/FUNCTION_REUSE.md).
Set `PSP_RECOMP_FUNCTION_CATALOG` to a private provenance catalog that should
be used by default across local projects.

### 4. Compile the Runtime & Game
On Windows, build the game and runtime objects with:

```bash
mingw32-make GAME_NAME=mygame GAME_ELF=<path_to_eboot.elf> GAME_BASE=0x08804000 GAME_ENTRY=<entry_point_address>
```

On Apple Silicon, use the same Makefile with the Homebrew Python:

```bash
make \
  PYTHON=/opt/homebrew/opt/python@3.13/bin/python3.13 \
  GAME_NAME=mygame \
  GAME_ELF=<path_to_eboot.elf> \
  GAME_BASE=0x08804000 \
  GAME_ENTRY=<entry_point_address>
```

The macOS output is `build/mygame/mygame`; Windows produces
`build/mygame/mygame.exe`. Make sure `GAME_ENTRY` matches the entry point of
the specific ELF.

On macOS SDK versions whose text stubs contain an unsupported
`arm64e.x1` architecture, select an installed older SDK for the build:

```bash
export SDKROOT=/Library/Developer/CommandLineTools/SDKs/MacOSX26.5.sdk
```

The output executable will land at `build/mygame/mygame.exe`.

### Standalone Module Boot

The maintained runtime can boot a pre-relocated `prxload.py` image without a
PPSSPP trace:
```bash
<driver> --boot <image.bin> 08804000 08804000 none --gui
```
`--boot` enables the scheduler, creates the PSP-compatible `module_start`
thread, and supplies the compile-time `SR_BOOT_ARG` value (`"modulename"` by
default). Keep using `--image` with a PPSSPP reference trace for differential
tracing. Title builds can define `SR_EXPECTED_ISO_SIZE` and
`SR_EXPECTED_ISO_SHA1` together to validate the `PSP_ISO` override or a
persisted native-launcher selection before guest memory is allocated; omitting
them retains game-agnostic behavior.

### 5. Setup Game Assets & Run
1. Set `PSP_ISO` to the full path of your dumped game UMD ISO. A title-specific
   native launcher may persist a verified path instead.
2. Run the game:

```bash
# Windows
build/mygame/mygame.exe --image build/mygame/mygame_image.bin 0x08804000 <entry_point_address> none none --gui

# macOS
build/mygame/mygame --image build/mygame/mygame_image.bin 0x08804000 <entry_point_address> none none --gui
```

## Runtime Controls

SDL3 maps connected gamepads automatically, and gamepad, mouse, and keyboard
input can be mixed at any time. The default PC mappings are:
- **WASD**: PSP analog stick movement
- **Arrow Keys**: PSP D-pad
- **Left Mouse / F**: Square
- **Right Mouse / E**: R
- **Middle Mouse / R**: Triangle
- **Mouse X1 / G**: Circle
- **Mouse X2 / Q**: L
- **Space**: Cross
- **Enter / Shift**: Start / Select

Gamepads use the left stick, D-pad, standard south/east/west/north face
buttons, and either shoulder or trigger for L/R.

On first windowed launch, Windows writes `graphics.ini` beside the executable;
the macOS app stores it under Application Support. Windows opens its native
settings dialog and defaults to D3D12; macOS defaults to Vulkan over MoltenVK
with Automatic quality:

```ini
# Windows
renderer = d3d12
quality = manual
internal_resolution = 2
anti_aliasing = off
skip_startup = off
```

Use `renderer = vk` on macOS. Run `mygame.exe --configure` to reopen the
Windows dialog without starting the game. The macOS app provides native
graphics and controller settings.
`renderer` accepts `auto`, `d3d12`, `sw`, or `vk`; unavailable backends fail
explicitly at startup. `quality` accepts `auto` or `manual`. On Apple Silicon,
Auto chooses a deterministic conservative GPU/display profile without
overwriting the retained manual values; legacy files without `quality` migrate
to Manual. M1/M2 start at 2x, their Pro/Max/Ultra variants and base M3/M4 at
3x, M3/M4 Pro/Max/Ultra and M5 at 4x, and unknown devices at 2x; drawable and
Vulkan limits can lower the scale. Auto keeps xBRZ off and enables SMAA only
for the measured M5 profile. `internal_resolution` accepts every integer from 1
through 8 and applies to D3D12 and Vulkan color/depth targets, feedback
snapshots, copies, readback, and presentation. PSP-visible framebuffer
addresses and coordinates remain at their original logical resolution.
`anti_aliasing` accepts `off` or `smaa`. SMAA selects the three-pass SMAA 1x
High post-process on D3D12 or Vulkan, runs at the selected internal resolution,
and is included in GPU captures. The launcher rejects SMAA with software
rendering.
Targets that define a startup-skip presentation expose a default-off
**Skip startup sequence** checkbox. `skip_startup = on` executes normal guest
initialization without real-time pacing, invokes the target's guarded startup
transition hook when provided, and restores real-time pacing at the target
presentation.
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

### Optional Killzone: Liberation twin-stick extension

When the Killzone-specific extension is compiled in,
`twin_stick.enabled = true` enables its experimental gamepad-only scheme.
While a controller is connected, the movement stick is converted to PSP D-pad
directions with hysteresis. The aim stick is carried through a native side
channel and replaces the base on-foot facing vector before the game's original
target and auto-aim correction. The runtime temporarily overlays the hidden
D-pad movement profile without marking it dirty and restores the original
mappings when the controller disconnects. This experiment has not yet passed
the full gameplay validation matrix, so back up saves before testing it.

Shared builds default `SR_ENABLE_KZL_TWIN_STICK` to `0`. That build keeps
standard PSP button and analog input, omits twin-stick/reversal controls from
the native dialog and generated `input.ini`, and ignores stale twin-stick
settings in an existing configuration. The Killzone parent build explicitly
sets `SR_ENABLE_KZL_TWIN_STICK=1`.

Run `mygame.exe --configure-input` to edit all twelve gamepad action bindings and
the twin-stick/reversal options in the native controller dialog. Each action
accepts the same comma-separated names as `input.ini`, including combinations
such as `west, right_trigger` for Square/fire. `mygame.exe --configure` opens the
graphics dialog followed by controller settings.
Turning twin-stick on from an unchecked state loads the tested default preset:
`left_shoulder` for PSP L/strafe, `left_trigger` for PSP R/aim,
`west, right_trigger` for Square/fire, and vertical aim reversal. It does not
change the built-in controls while twin-stick remains disabled.

The running game checks `input.ini` four times per second. Saving from a second
`mygame.exe --configure-input` process hot-reloads bindings, twin-stick enablement,
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
`dpad_*` names, `left_stick`, and `right_stick`. Inputs may also be separated
by `|`, and common spellings from launchers are accepted: SDL key names such
as `Return`, `Space` or `Left Shift`, and Xbox or PlayStation button names
such as `A`, `Cross`, `LB`, `R2` or `DPad Up` (case, spaces, `_` and `-` are
ignored). A key on a `gamepad.*` line or a button on a `keyboard.*` line is
skipped, so a launcher that stores both devices on one action does not break
the file. Other invalid settings stop startup
with a line-specific error instead of silently falling back. Set
`SR_INPUT_CONFIG=<path>` to use another file, or `SR_INPUT_CONFIG=0` to use
built-in defaults without reading or writing a file. An untouched generated
v1, v2, or v3 config is migrated automatically to the v4 PC defaults;
customized older files retain their bindings and inherit the disabled
twin-stick defaults.
Set `SR_GRAPHICS_CONFIG=<path>` or `SR_SETTINGS_DIR=<directory>` for isolated
automation. `SR_GRAPHICS_CONFIG=0` uses the built-in graphics defaults without
opening the first-run launcher or reading/writing a file.
On macOS, default mutable settings are stored in the title's Application
Support directory rather than beside the executable. `SR_RESOURCE_DIR`
overrides immutable app/developer resources such as fonts and VFPU tables.
Title-specific launchers may persist an ISO path in `launcher.ini`; `PSP_ISO`
always takes precedence.

## Player Options

Every mode of the driver also accepts these options anywhere on the command
line:

- `--game <disc.iso>` sets the disc image (`PSP_ISO`).
- `--data-dir <folder>` keeps settings, saves and logs in one folder
  (`SR_DATA_DIR`), in place of the per-user data folder: `graphics.ini` and
  `input.ini`, the Memory Stick in `memstick/` (both sceUtilitySavedata and
  `ms0:` files), other writable devices in `fs/<device>/`, and
  `logs/<exe>.log`. Program resources (VFPU tables, fonts) still come from the
  resource folder. `SR_SETTINGS_DIR`, `SR_MEMSTICK` and `SR_FSDIR` still
  override single locations.
- `--log <file>` copies everything printed to stderr into a log file, keeping
  the previous run's as `<name>.previous.log`. With a data folder the game logs
  to `logs/<exe>.log` unless `SR_LOG=0`; `SR_LOG_FILE` sets the path. The crash
  handler flushes the log before the process ends.
- `--fullscreen` starts full screen (`SR_FULLSCREEN=1`; `0` forces a window).
  `RECOMPLAUNCHER_MODE=bigpicture` also starts full screen. F11 toggles it in
  the GDI, D3D12 and Vulkan windows.

## Environment Settings

Set these environment variables before running the game to control runtime features:
- Audio uses XAudio2 2.9 on Windows and an SDL3 output stream on macOS, both at
  44.1 kHz stereo. The Windows backend's eight 256-frame source-buffer slots
  provide up to 46.44 ms of device coverage.
  The feeder submits only available guest PCM instead of pre-rendering silence,
  and Output2 preserves the PSP's two-buffer completion contract. PSP channels
  still mix through the shared saturation ring before submission.
- Windows decodes H.264 movie video with Media Foundation. macOS uses
  VideoToolbox and requires a verified hardware-accelerated H.264 decoder;
  it reports an error if one is unavailable instead of falling back to
  software. Both paths demultiplex ATRAC movie audio for the shared decoder.
  Movie audio timestamps advance for each ATRAC frame, including when a PES
  packet contains multiple frames.
  Other portable platforms currently report H.264 as unavailable. Terminal
  text entry is used as the portable OSK fallback.
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
- `SR_GPU_FULL_FEEDBACK=1`: Force full Vulkan feedback snapshots for
  differential diagnostics.
- `SR_GPU_PRESENT_REPEATS=1`: Force Vulkan presentation of unchanged GPU
  frames for differential diagnostics.
- `SR_VULKAN_PIPELINE_CACHE=0`: Disable Vulkan GE/SMAA pipeline-cache loading
  and persistence for cold-cache A/B diagnostics. By default the cache is
  stored at `cache/vulkan-pipelines.bin` below the settings directory, keyed
  by device, driver, pipeline-cache UUID, shader set, and cache schema, and
  published atomically at clean shutdown. Incompatible or corrupt files are
  reported and rebuilt.
- `SR_VULKAN_GPU_UPLOAD_SCALE=0`: Disable the capability-gated nearest GPU
  blit used to expand logical framebuffer uploads at scaled internal
  resolutions. The diagnostic fallback performs the same expansion on the
  CPU before transfer.
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
- `SR_GPU_PSP_SAMPLER=0`: Disable the PSP-quantized nearest/bilinear shader
  path and use host hardware filtering for A/B diagnostics. Non-mipmapped
  logical textures otherwise use the software renderer's 8-bit bilinear
  weights, truncation order, and explicit wrap/clamp coordinates on both
  D3D12 and Vulkan. Scaled D3D12 framebuffer aliases quantize against their
  physical texel grid, preserving every internal-resolution sample through
  post-processing instead of collapsing back to the PSP pixel grid. The same
  path applies PSP integer texture-function and fog math. Logical D3D12
  captures are canonicalized through the active PSP framebuffer format before
  golden comparison; full internal-resolution captures remain raw.
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
- `SR_ANALOG_SCRIPT=<path>`: Inject deterministic presentation-timed analog movement. The file
  begins with `# psp-recomp analog script v1`; each event is
  `<first-present> <duration> <lx> <ly>`, with both stick coordinates in `0..255`. Events are
  half-open, ordered, and non-overlapping. `SR_ANALOG` and `SR_ANALOG_SCRIPT` are mutually
  exclusive.
- `SR_PADLOG=1`: Print scripted button-mask edges with both presentation and
  vblank counters, plus analog-script transitions.
- `SR_INPUT_LOG=1`: Print live logical button/analog edges and whether a
  gamepad is connected.
- `SR_FPS_COUNTER=1`: Update the GDI, Vulkan, or D3D12 window title every
  500 ms with the rolling host display-update rate and average interval.
  This is not the game's presentation rate.
- `SR_FBSNAP_PRESENT=<interval>`: Write an eight-file rotating set of software
  framebuffer snapshots at the requested presentation interval.
- `SR_FBCAPTURE_PRESENTS=<p1,p2,...>`: Write rendered framebuffer snapshots at
  up to 32 exact positive presentation numbers. D3D12 and Vulkan capture their
  GPU display target; other presenters use the authoritative guest framebuffer.
  GPU captures default to the logical 480x272 display size for golden-frame
  compatibility. Duplicate or malformed values fail startup.
- `SR_VULKAN_CAPTURE_INTERNAL=1`: Keep Vulkan captures at the selected full
  internal resolution instead of center-sampling them to 480x272.
- `SR_FAST_FORWARD_PRESENT=<present>`: Run scheduler and presentation pacing as fast as the host
  allows until the requested presentation, keep host audio dormant during that phase, then align
  virtual time to the host clock and start real-time XAudio2 playback. This is intended for
  deterministic in-game testing rather than normal play.
- `SR_GPU_TNL=1`: Opt eligible transform-mode triangle lists, strips, and
  fans into the shared D3D12 or Vulkan model-space transform-and-lighting
  path. Unsupported draws, devices, allocation failures, and backend declines
  preserve the CPU transform path. The path remains opt-in until protected
  title validation is complete. `SR_GPU_TNL_REFERENCE=1` additionally times a
  shader-equivalent CPU transform for accepted draws while performance
  collection is enabled; use it for removed-work estimates, not normal play.
  `SR_GPU_TNL_VALIDATE=1` requires `SR_GPU_TNL=1`, selects a validation-only
  vertex-shader variant, reads back every submitted transform result, and
  compares screen position, reciprocal W, UV, fog, and color with the packed
  CPU reference. It deliberately stalls each affected submission and stops
  the backend on the first mismatch, so use it only for correctness runs.
  `SR_GPU_TNL_VALIDATE_EXACT=1` tightens that comparison to numerical equality
  for every rendered component and is intended for diagnosing image-level
  differences that remain inside the normal floating-point tolerance.
  `SR_GPU_TNL_VALIDATE_START=<present>` delays validation writes and
  submission stalls until that presentation count, allowing a later route
  segment to be checked without validating the entire startup path.
- `SR_VULKAN_GPU_TNL_CLASSES=<list>`: Restrict an enabled Vulkan GPU T&L
  diagnostic run to a comma-separated subset of `direct`, `preprocess`,
  `points`, and `sprites`. Unselected classes use the exact CPU fallback.
  The default is all classes; unknown or empty list elements fail startup.
- `SR_VULKAN_GPU_TNL_DIRECT_COMPUTE=1`: Transform each canonical vertex for
  direct Vulkan GPU T&L draws once in the existing compute preprocess stage,
  then render the resulting screen vertices through an indexed pipeline.
  This diagnostic path targets the cost of strict non-contracted vertex
  arithmetic. It remains opt-in because the exact Killzone route did not
  improve complete-frame mean or wall time beyond variance.
- `SR_VULKAN_GPU_PRIMITIVE_EXPANSION=1`: Opt Vulkan through-mode sprites into
  a validation-only compact-control path. The CPU still decodes the two PSP
  controls; the vertex shader expands them to six vertices with exact
  coordinate flooring, second-control attributes, and texture-filter UV
  rules. This switch is independent of model-space GPU T&L and remains off by
  default. Use `SR_PERF_ALTERNATE=gpuprim` for the exact CPU-expansion A/B
  fallback.

The Phase 7 promotion audit enables no additional experimental GPU path.
Scaled framebuffer upload expansion remains the reconciled capability-gated
baseline with `SR_VULKAN_GPU_UPLOAD_SCALE=0` as its forced fallback.
`SR_GPU_TNL`, Vulkan compute texture decode, and Vulkan through-sprite
expansion remain opt-in until both protected title gates and repeated
complete-frame timing pass.
- `SR_PERF_LOG=1`: Record performance samples at host display update cycles and split each sample
  between frontend and graphics-backend time. `SR_PERF_START=<present>` selects the first sample by
  `sceDisplaySetFrameBuf` call count, while `SR_PERF_WINDOW=<1..600>` selects the reporting window.
  D3D12 and Vulkan report GPU timestamp samples when they submit rendered work.
  The Vulkan presenter may reuse an unchanged frame without submitting a new swapchain presentation.
  `SR_PERF_MIN_FPS=<fps>` terminates with failure when a completed flip window falls below the
  required average. `SR_EXIT_AFTER_PRESENT=<present>` exits after the specified
  `sceDisplaySetFrameBuf` call count with final audio synchronization statistics.
  `SR_PERF_FILE=<path>` writes the CSV v10 performance record;
  `SR_PERF_FRAME_BUDGET_MS=<milliseconds>` overrides the over-budget threshold.
  In addition to guest-work and framebuffer fields, D3D12 and Vulkan report
  pipeline-key hits/creation time, renderer texture-cache
  hits/misses/uploads, texture decode/xBRZ time, and host upload/readback
  bytes. GPU T&L fields report eligible, accepted, and backend-declined draws;
  canonical and submitted vertices; canonical decode and optional CPU
  reference time; and backend upload bytes. Geometry-workload fields separate
  point, line, sprite, Bezier, and spline command counts; generated
  primitives/vertices; and exclusive setup, evaluation, and draw time.
  CSV v10 also records dynamic native dispatches and actual scheduler yields.
  Vulkan additionally reports
  logical framebuffer-upload bytes,
  framebuffer-specific host bytes, expanded physical pixels, upload and CPU
  fallback counts, and upload synchronization time. The record also includes
  the thermal state where the host exposes it
  (`0` nominal, `1` fair, `2` serious, `3` critical, or `-1` unavailable).
  Window summaries report the highest thermal state observed during that
  interval.
- `SR_VULKAN_STAGE_LOG=1`: Populate Vulkan GE/presenter stage timing and
  framebuffer-statistics columns in performance reports. Timestamp-based
  `gpu_ms` samples do not require this switch. Vulkan GE stages accumulate
  incrementally while the guest runs, so their sum can exceed the narrow
  `backend_ms` interval around final presentation; `begin_ms` is zero because
  Vulkan does not use a D3D12-style frame-allocator begin stage.
- `SR_TRACE_STALLS=1`: Print a `[slow-frame]` line for every host flip longer than
  `SR_SLOW_FRAME_MS` (default 1.25 x the frame budget) naming the three largest phases, scheduler
  idle time, texture misses and decode wait, PSO fallback binds, and the previous flip's backend
  stages. It works with or without `SR_PERF_FILE`.
- `SR_SAMPLE_START=<present>` / `SR_SAMPLE_WINDOW=<presents>` /
  `SR_SAMPLE_FILE=<path>`: On Windows and macOS, sample the main runtime
  thread's instruction pointer at 4 kHz over the requested guest-presentation
  window. The primary report buckets executable-relative addresses for symbol
  resolution; `<path>.categories.txt` retains the exclusive runtime category
  for every bucket, while `<path>.modules.txt` attributes categorized samples
  in runtime libraries and drivers. Sampling is diagnostic-only and its
  reports must not be committed. Resolve a category with
  `python3 tools/analyze_ip_samples.py <samples> --executable <binary>
  --category native`.
- Clean runtime shutdown prints MPEG video/audio completion and maximum A/V
  drift separately from the display-performance CSV, followed by the audio
  backend's completed, queued, dropped, resynchronized, and silent-frame
  summary. These media counters are correctness evidence, not graphics time.
- `SR_PERF_ALTERNATE=<path>[,<path>...]`: Switch output-identical optimization paths (`texdecode`,
  `merge`, `memo`, `indexed`, `gpuupload`, `gputnl`, `gputexdecode`, `gpuprim`)
  off every other `SR_PERF_ALTERNATE_PRESENTS` (default 60) guest
  presentations, so one run measures them on and off in the same scene.
  `gpuupload` forces scaled framebuffer expansion through the CPU fallback but
  cannot override missing GPU capabilities. `gputnl` forces eligible
  model-space draws through the CPU transform fallback. `gputexdecode` forces
  opt-in Vulkan compute texture decode through the asynchronous CPU snapshot
  decoder. `gpuprim` forces the opt-in Vulkan through-sprite path through
  exact CPU expansion. The `alt_off` column holds the paths currently off; bit
  31 marks a flip whose frame straddled a change, which comparisons should
  discard.
- `PSP_VFPU_TABLES=<directory>`: Override the `assets/vfpu` lookup-table directory. The golden
  harness sets this explicitly so runtime execution does not depend on its working directory.
- `SR_INTERP_MISS_LOG=<path>`: Write each distinct guest target that missed the generated
  dispatch table once. Version 2 entries retain the generated module prefix so
  overlapping overlay addresses remain distinct. The runtime executes
  supported misses through the reference interpreter, hands nested targets
  back to generated code when they enter the active native table, and reuses
  the tested VFPU instruction engine for supported interpreted VFPU operations.
  A clean exit writes a header-only report when no target missed, so missing
  evidence is distinguishable from a measured zero. Gate the miss and profile
  pair with `python3 tools/check_interp_reports.py <misses> <profile>`;
  without a policy file, any fallback fails.
- `SR_INTERP_PROFILE=<path>`: Write aggregate calls, guest instructions,
  syscalls, native handoffs, chunk limits, returns, and failures for every
  module-qualified fallback target. A clean exit likewise writes an explicit
  zero-target profile. An optional `--policy` file can review individual rows
  as `module target max_calls max_instructions`; step limits, failures, and
  incomplete returns always fail. This does not change the v2 miss log.
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
