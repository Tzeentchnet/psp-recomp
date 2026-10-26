# Credits and Third-Party Attribution

This project is **not** built from scratch. Large parts of it are derived from,
or directly reuse, other people's work. This file documents what was used, where
it lives, and under which license — and what your obligations are if you
redistribute this project or anything built with it.

## License of this project

Because this project contains ports and derivative runtime behavior from PPSSPP
(GPL-2.0-or-later), the combined work is licensed **GPL-2.0-or-later**. See
[LICENSE](LICENSE).

If you distribute binaries built with this toolkit (for example a recompiled
game `.exe` plus `SDL3.dll`), the GPL requires that you also make the **complete
corresponding source** available to your recipients, including this toolkit's
source and any modifications you made. You may not relicense the result or ship
it as a closed-source product.

## AI assistance disclosure

Much of this project — including the D3D12 renderer (`src/rt/gpu_d3d12/`) and
the SDL3 + Vulkan renderer (`src/rt/gpu_sdl3vk/`) — was written with
substantial assistance from a large language model (Anthropic's Claude). AI
was used across the runtime, the HLE layer and the renderers:
to draft code, to translate/port C++ from PPSSPP into C (see the next section),
and to write comments and documentation.

What this means for you:

- **Correctness is not guaranteed.** AI-generated code can contain subtle bugs,
  and the "faithfulness vs PPSSPP" claims in the comments are the model's
  assertions, not an independent audit. Validate before relying on anything.
- **Provenance.** Where code was translated from PPSSPP, the GPL-2.0-or-later
  obligations apply regardless of whether a human or a model did the translating;
  using an AI tool does not change the license of derived code. The specific
  translated code is itemised below.
- The GPU backends are **original implementations** (they do not reuse
  PPSSPP's GPU), but they reproduce PSP/GE pixel semantics that were derived
  from PPSSPP's software renderer (see `src/rt/ge.c`), so the combined work
  remains derivative.

## PPSSPP — the single biggest dependency

- **Project:** PPSSPP — https://github.com/hrydgard/ppsspp
- **License:** GPL-2.0-or-later (see `third_party/ppsspp/LICENSE.TXT`)
- **Authors:** Henrik Rydgård and the PPSSPP contributors
- **How it is used here (this is substantial, not incidental):**
  - **HLE / media port.** The sceMpeg/PSMF runtime (`src/rt/mpeg.c`) is a direct
    port of PPSSPP's `Core/HLE/sceMpeg.cpp` (control flow, ring-buffer
    accounting, AU getters); it is explicitly GPLv2+. Other HLE behaviour
    (sceGe handling, kernel semantics) is modelled on or derived from PPSSPP.
  - **GE semantics reference.** The software GE rasterizer was validated
    against PPSSPP's software renderer; the D3D12 and Vulkan backends
    (`src/rt/gpu_d3d12` and `src/rt/gpu_sdl3vk`) reproduce
    PPSSPP-documented GE rules.
  - **Verification oracle.** The build runs `PPSSPPHeadless.exe` (PPSSPP's
    headless interpreter) as the reference the recompiled code is diffed
    against, bit-for-bit, up to the first HLE call.
  - **System fonts.** `font/` ships PPSSPP's free replacement `jpn0.pgf` and
    `ltn0.pgf`, needed for native text dialogs. They are not PSP firmware dumps;
    the superproject content audit pins their PPSSPP v1.20.4 SHA-256 identities.
  - **Secure savedata.** `third_party/ppsspp_savedata/` contains a standalone
    subset of PPSSPP's Chnnlsv implementation plus libkirk. Chnnlsv is
    GPL-2.0-or-later; libkirk is GPL-3.0-or-later, so linked binaries must comply
    with GPL-3.0-or-later. See that directory's README and `COPYING.GPLv3`.

  Note: an earlier `src/rt/gpu_vk/` bridge reused PPSSPP's `GPU_Vulkan`
  wholesale behind a C ABI (modelled on `WindowsVulkanContext.cpp`, linking
  `GPU.lib`/`Common.lib`/`Core.lib`). It was never activated and has been
  removed; the active renderers are the project's own D3D12 and SDL3 + Vulkan
  backends.

Where this project's code is "PPSSPP rewritten in C" (HLE kernel behaviour,
sceGe handling, media engine glue, etc.), it is still a derivative of GPL'd
PPSSPP code and is covered by the same license.

## FFmpeg standalone ATRAC3/ATRAC3+ decoder

- **Location:** `third_party/at3_standalone/`
- **Origin:** PPSSPP v1.20.4 (`fa50bb1`), `ext/at3_standalone`, extracted from FFmpeg by PPSSPP contributors.
- **License:** LGPL-2.1-or-later; see
  [`third_party/at3_standalone/COPYING.LGPLv2.1`](third_party/at3_standalone/COPYING.LGPLv2.1)
  and the copyright/license header in each source file.
- **Local changes:** removed PPSSPP logging and allocator dependencies, retained portable aligned allocation,
  selected SIMD from compiler architecture macros, and fixed warning-clean portability issues. The C ABI adapter
  is `src/rt/atrac_decode.cpp`.

The decoder remains separately available under the LGPL even though the complete linked runtime is distributed
under GPL-2.0-or-later.

## Translated / ported code from PPSSPP (specifics)

The following files are direct translations/ports of PPSSPP C++ into C, or are
generated from PPSSPP data. All are GPL-2.0-or-later. PPSSPP source paths are
relative to `third_party/ppsspp/`.

| File in this project | Translated/derived from (PPSSPP) | Nature |
| --- | --- | --- |
| `src/rt/mpeg.c`, `src/rt/h264_mf.c`, `mpeg` wrappers in `src/rt/hle.c` | `Core/HLE/sceMpeg.cpp` (rev 4e109dd6), `Core/HW/MpegDemux.cpp` | PSMF header/ring/context/stream/AU behavior modelled on PPSSPP; native decode uses original Windows Media Foundation H.264 glue plus the separately attributed standalone ATRAC decoder rather than PPSSPP's FFmpeg MediaEngine. |
| `src/rt/atrac_decode.cpp`, Atrac wrappers in `src/rt/hle.c` | `Core/HW/Atrac3Standalone.cpp`, `Core/Util/AtracTrack.cpp`, `Core/HLE/AtracCtx.cpp` | C ABI and PSP RIFF/buffer/loop integration around the separately attributed LGPL standalone decoder above. |
| `src/rt/pgf.c`, `src/rt/pgf.h` | `Core/Font/PGF.cpp` | C port of the PGF font reader: PGF parse, `PGFFontInfo`/`PGFCharInfo`/`GlyphImage` layouts, glyph rasterisation. |
| `src/rt/vfpu_interp.c`; VFPU prefix + transcendental helpers behind `src/rt/recomp.h`/`recomp.c` | `Core/MIPS/MIPS.cpp` and PPSSPP's VFPU kernels | Exact ports of the table-based VFPU transcendentals and the `vcst` constant table; source/destination prefix application ported from PPSSPP. |
| `src/rt/ge.c`, `src/rt/ge_patch_math.h`, `tools/tests/ge_patch_math_test.c` (software GE rasterizer, shared GPU frontend, and exact patch math) | `GPU/Software/*` (`Clipper`, `TransformUnit`, `Sampler.cpp`, `Lighting.cpp`, `DrawPixel.cpp`, `SoftGpu.cpp`), `GPU/Common/VertexDecoder*`, `GPU/ge_constants.h` / `GECommands.h`, `GPU/Common/TextureCacheCommon`; `GPU/Common/SplineCommon.cpp` and `unittest/TestSplineTessellation.cpp` (rev `ce97cb120182`) | Algorithm-for-algorithm reimplementation of PPSSPP's software renderer: vertex decode, T&L, near-clipping, primitive acceptance, culling-by-submission-order, sampler/CLUT offsets, depth/fog semantics, and command decode. The four-lane GE-exact spline interpolation and scalar/SIMD bit-equivalence tests are C adaptations of PPSSPP's pinned implementation. |
| `src/rt/hle.c` (kernel/HLE behaviour), `src/rt/sas_reverb.c` | `Core/HLE/*` (`sceUtility.cpp`, `sceCtrl.cpp`, `sceAtrac.cpp`, `sceSas.cpp`, kernel object/UID pool, `SetDeadbeefRegs`), `Core/HW/Display.cpp`, `Core/HW/SasAudio.cpp`, `Core/HW/SasReverb.cpp` | Modelled on PPSSPP: UID allocation, partition layout, `sceUtility` defaults, sceCtrl ring-buffer sampling, sceAtrac decode bookkeeping, SAS ADSR/mixing and PSX-style preset reverb, deadbeef register poisoning, vblank/line timing. |
| `src/rt/nid_names.h` | `Core/HLE/*.cpp` function tables | Generated from PPSSPP's HLE NID tables (do not edit by hand). |
| `src/rt/recomp.h` (`CpuState` layout, semantics) | `Core/MIPS/MIPS*` | VFPU `v[128]` physical ordering, `fpcond`/`fcr31` split, unaligned LWL/LWR/SWL/SWR, kernel object pool numbering all match PPSSPP. |

The D3D12 and SDL3 + Vulkan renderers (`src/rt/gpu_d3d12/` and
`src/rt/gpu_sdl3vk/`) are **not** ports of PPSSPP code — they are original
implementations (AI-assisted; see the disclosure above). They do, however,
consume primitives from `src/rt/ge.c` and reproduce PSP/GE pixel rules that
were themselves derived from PPSSPP, so the combined work is derivative for
licensing purposes.

## Other third-party components

- **SDL3** (`SDL3.dll`, `src/rt/gpu_d3d12/`, `src/rt/gpu_sdl3vk/`) — zlib license — https://libsdl.org
- **Vulkan loader / headers** — Apache-2.0 — https://github.com/KhronosGroup/Vulkan-Loader
- **PSPSDK headers/defines/constants** — BSD-compatible (see top of
  `third_party/ppsspp/LICENSE.TXT`).
- **Media Foundation H.264 path** (`src/rt/h264_mf.c`) — uses Microsoft Media
  Foundation, part of Windows.
- **Yakumo** (TeamGDB, MIT) — https://github.com/TeamGDB/Yakumo — design reference only: the
  background texture decode, persistent pipeline-key prewarm, on/off path alternation and
  slow-frame stall attribution follow ideas from its Vulkan renderer and performance tooling.
  No Yakumo source is included.

## If you only want the recompiler, not PPSSPP

The MIPS-to-C recompiler itself (`tools/`, parts of `src/rt`) and the D3D12 and
SDL3 + Vulkan backends are independent of PPSSPP. But the runtime still
contains PPSSPP-derived HLE/GE/VFPU code and uses PPSSPP as its verification
oracle, so a GPL-free build is not available today; relicensing would require
replacing those derivative parts.
