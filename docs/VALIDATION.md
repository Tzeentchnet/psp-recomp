# Validation and Evidence Guide

Validation is not one test suite. It is a ladder from synthetic behavior to a
complete title route, with deterministic evidence at every level.

## Core requirements

- Pin the title input, runtime commit, PPSSPP revision, settings, and script.
- Use strict failures for unsupported behavior.
- Compare the same guest route and presentation indices.
- Keep game-derived evidence ignored.
- Record commands, thresholds, and results with the corresponding finding.

## Validation ladder

| Level | Purpose | Required evidence |
|---|---|---|
| 1. Unit | Prove one parser, instruction, HLE primitive, or renderer rule | Synthetic fixture and executable assertion |
| 2. Shared compile | Catch type, toolchain, and linkage regressions | Runtime-only target on supported compilers |
| 3. Full title build | Prove generated code and title wiring compile | Clean configure/generate/build log |
| 4. Function trace | Prove local CPU/VFPU semantics | Exact register and memory deltas |
| 5. Bounded scheduler trace | Prove calls, HLE, callbacks, and thread order | Deterministic trace/call sequence |
| 6. Frame checkpoint | Prove GE and presentation state | Exact or thresholded frame manifest |
| 7. Scripted front end | Prove input and state progression | Pad script, expected frames, input edges |
| 8. Vertical slice | Prove interacting subsystems | Reproducible playable route |
| 9. Stage/mission | Prove sustained title execution | Route completion and bounded fallback set |
| 10. Save/media routes | Prove persistence and audiovisual transitions | Save round trip, movie/audio completion |
| 11. Soak/campaign | Prove release stability | Duration, routes, performance, zero unexpected failures |

## Shared public checks

From an MSYS2 UCRT64 shell:

```bash
python -m unittest discover -s tools/tests -p "test_*.py"
make test-runtime
```

The root Makefile uses POSIX line continuations. Run its native test targets
through an MSYS2 shell rather than `mingw32-make` under `cmd.exe`.

Useful focused targets include:

```bash
make test-dispatch-table
make test-fps-counter
make test-input-config
make test-module-loader
make test-memstick-state
make test-interpreter
make test-ge-tnl
make test-d3d12-vertex
make check-shaders-vulkan
make test-vulkan-tnl-validation
```

Run `test-d3d12-vertex` in a Windows/MSYS2 environment, or cross-compile it
and `gpu_d3d12/device.cpp` against MinGW D3D12 headers on another host. A
cross-compile proves source and ABI compatibility, not DXIL validity or
runtime behavior. Compile checked-in D3D12 shader artifacts with Microsoft
DXC and still require a native Windows run with the DXIL validator and D3D12
debug layer before closing a hardware gate.

`make test-ge-tnl` also verifies the shared 64-byte raw-texture job ABI,
immutable source/CLUT snapshot payloads, mip descriptors and offset/bounds
rejection. These tests define compute-decoder inputs but do not by themselves
claim that the opt-in runtime cache path was exercised.

Also run:

```powershell
git diff --check
```

Consumer repositories may add runtime-only CMake targets, content audits,
Python tests, and protected runners. Follow their pinned reproduction docs.

## Comparison tools

| Tool | Use |
|---|---|
| `tools/codegen_gate.py` | Generate, compile, and compare an ELF against an oracle trace |
| `tools/microtest_gate.py` | Run a synthetic module and compare its trace |
| `tools/tracediff.py` | Stream exact CPU/register/memory trace comparison |
| `tools/funcdiff_cmp.py` | Compare function-level oracle and native traces |
| `tools/ppmdiff.py` | Compare deterministic PPM frame sets and manifests |
| `tools/nidseq.py` | Compare imported-NID call sequences |
| `tools/vfpu_fuzz_gen.py` | Generate VFPU differential vectors |
| `tools/gen_microtest.py` | Generate instruction microtests |
| `tools/function_catalog.py` | Validate, extend, and match reviewed function identities |
| `tools/compile_d3d12_shaders.py --check` | Verify checked-in D3D12 shader includes |
| `tools/compile_vulkan_shaders.py --check` | Verify production and validation Vulkan shader includes |
| `make test-vulkan-tnl-validation` | Execute the real GPU T&L, point/sprite expansion, preprocessing, and direct/indexed/DXT texture decode shaders and compare readback with CPU oracles |

The CPU trace schema and deterministic kernel/GE headers are defined in
[TRACE_FORMAT.md](../tools/TRACE_FORMAT.md).

## Deterministic title harness

A title harness should pin:

- Source image and module hashes.
- Runtime and PPSSPP commits.
- Base, entry, GP, start-thread, UID, and GE state.
- Pad-script version and exact input edges.
- Presentation indices and capture manifest.
- Expected fallback targets and module-qualified identities.
- HLE/import call sequence when relevant.
- Renderer, scale, audio mode, and environment switches.

Use a clean memstick fixture for every stateful route. Do not reuse a
developer save directory as acceptance evidence.

CSV v10 evidence must retain point, line, sprite, Bezier and spline
draw/workload fields alongside their exclusive timing columns. Bezier and
spline evaluation/draw columns must also sum to the aggregate
`patch_eval_ms` and `patch_draw_ms` values used by the promotion gate.
`native_dispatches` and `scheduler_yields` must remain independent of HLE,
interpreter, media, and graphics workload fields.

## CPU/runtime profiling

Configure both `SR_INTERP_MISS_LOG` and `SR_INTERP_PROFILE` on protected
routes. A clean runtime exit must produce both files, including header-only
files for a measured zero. Validate them with:

```bash
python3 tools/check_interp_reports.py <misses> <profile>
```

The default gate rejects every fallback. If a reviewed overlay must remain
interpreted, pass `--policy <file>` with one
`module target max_calls max_instructions` row per allowed target. Step-limit
stops, interpreter failures, and calls that do not return always fail.

On Windows and macOS, use `SR_SAMPLE_START`, `SR_SAMPLE_WINDOW`, and
`SR_SAMPLE_FILE` to capture a bounded instruction-pointer window. Resolve
exclusive native work rather than aggregate renderer/wait samples:

```bash
python3 tools/analyze_ip_samples.py <samples> \
  --executable <binary> --category native
```

Record the 4 kHz sample estimate beside CSV `native_ms`; a large mismatch
invalidates attribution. For a small nested phase whose per-item clock reads
would perturb the workload, resolve its containing exclusive category and use
the phase's symbol or instruction range. Record both the isolated estimate and
a conservative upper bound that includes the whole containing symbol; do not
add hot-loop timers merely to obtain a finer CSV column. Keep MPEG completion/
A/V-drift and audio queue/drop/resynchronization summaries separate from
graphics timing.

## Strict versus diagnostic modes

Diagnostic switches are valuable for isolation, but a result obtained with an
escape hatch is not an acceptance pass.

Examples that require explicit labeling:

- `SR_DISPATCH_PERMISSIVE=1`
- `SR_DISABLE_NATIVE_MODULES`
- `SR_NOAUDIO=1`
- `SR_D3D12_WARP=1`
- `SR_D3D12_UNSAFE_PSO_CACHE=1`
- Forced software/hardware renderer overrides
- Reduced interpreter step limits
- Disabled input hot reload or title extensions

For Vulkan texture-decode work,
`SR_VULKAN_SYNC_TEXTURE_DECODE=1` is the exact synchronous fallback and
`SR_PERF_ALTERNATE=texdecode` is the supported A/B switch.
`SR_VULKAN_GPU_TEXTURE_DECODE=1` opts scale-1 ordinary RAM textures into the
compute decoder, and `SR_PERF_ALTERNATE=gputexdecode` compares it directly
with asynchronous CPU snapshot decode. `SR_VULKAN_VERIFY_TEXTURE_DECODE=1`
also decodes every cache miss with the CPU oracle and aborts on a byte
mismatch. A qualifying runtime capture must show that GPU jobs were scheduled,
report any CPU fallbacks, and leave GPU decode disabled for the final
default-path comparison.

For Vulkan through-mode sprite expansion,
`SR_VULKAN_GPU_PRIMITIVE_EXPANSION=1` selects the compact-control prototype
without enabling model-space GPU T&L. Pair exact protected-title captures with
the switch off and on, then use `SR_PERF_ALTERNATE=gpuprim` for timing. Compare
sprite CPU time, complete-frame median/p95/p99, GPU time, host upload bytes,
feedback snapshot pixels, and thermal state. Do not treat reduced upload bytes
alone as a promotion result, and do not use `gputnl` as a proxy because it
changes a separate transform-and-lighting path.

For Vulkan GPU T&L classification, keep `SR_GPU_TNL=1` and use
`SR_VULKAN_GPU_TNL_CLASSES` to run `direct`, `preprocess`, `points`, and
`sprites` independently. Every unselected class takes the CPU fallback.
Classify a changed frame with these isolated runs before changing eligibility
or widening an image threshold. If tolerant live shader validation passes but
an exact capture still differs, add `SR_GPU_TNL_VALIDATE_EXACT=1` to stop on
the first non-padding component that is not numerically equal to the CPU
reference. For a late route segment, set
`SR_GPU_TNL_VALIDATE_START=<present>` just before the target frame so earlier
submissions retain normal pacing.
After the direct shader is exact, evaluate
`SR_VULKAN_GPU_TNL_DIRECT_COMPUTE=1` separately: require the same complete
frame manifest, then compare canonical versus submitted vertex counts,
backend submission time, GPU time, host uploads, and complete-frame
distribution before retaining the transform-once path.

On macOS, select KosmicKrisp with its SDK ICD manifest through
`VK_DRIVER_FILES`. The validation-layer manifest must resolve to the same
validation dylib that the active loader actually opens; do not combine an SDK
manifest with a different Homebrew layer binary. Record the ICD, loader,
layer, API, driver and conformance versions. Compare CPU and GPU captures
within one driver because cross-driver CPU rasterization hashes are not an
exactness oracle. Screen-space and canonical RGBA8 inputs must be fetched as
integers and normalized explicitly in shaders rather than relying on
driver-specific UNORM conversion.

The final route must state which switches are part of the supported product
configuration and leave investigation-only switches unset.

## Performance promotion records

Every default-path change requires a reduced, reviewable record containing:

- shared and title revisions plus generated image/module hashes;
- named route state, renderer, driver, scale, quality settings and input
  fixture;
- presentation bounds, repetition count, thermal state and observed variance;
- complete-frame median, p95 and p99 plus affected CPU, backend and GPU
  categories;
- workload and transfer deltas;
- logical and full-resolution correctness results; and
- the resulting default, capability gate and diagnostic fallback.

Name the guest state represented by each window. An opening movie, menu,
cutscene and gameplay window are different workloads even when they belong to
one deterministic route. Presentation numbers alone do not prove gameplay.
Use expected frame manifests, workload counters or another explicit state
checkpoint, and never publish movie or menu timing as gameplay timing.

For paired forced-path runs, compare the same presentation indices and
aggregate all host flips belonging to each guest presentation. In-run A/B
halves are valid only when both halves exercise the same stable workload.
Reduced transfer traffic alone does not justify promotion when complete-frame
timing remains within variance.

A performance-changing path can alter the number of guest vblanks reached at
a fixed presentation. Turbo mode can drift by hundreds of vblanks, and even a
paced pair can differ by one vblank at an animated checkpoint. If two captures
land in different simulation states, their pixel delta is not
path-correctness evidence. Require matching route/state counters or exact
vblank checkpoints, and use live shader/oracle validation to establish
transform or decode equality independently.

If no path passes, publish the rejection and keep it opt-in. Do not advance a
title runtime pin until its public build and affected protected routes pass
against the exact shared revision; an uncommitted shared worktree is not a
runtime pin candidate.

## Graphics evidence

Keep two classes of capture:

1. **Logical PSP output** for canonical cross-backend comparison.
2. **Full internal-resolution output** for scale and hardware analysis.

Capture at exact presentation indices under deterministic input. When a frame
diff fails:

1. Identify the first divergent presentation.
2. Classify pixels by draw/feature/edge.
3. Use GE state and draw logs around that presentation.
4. Reproduce in software before changing a hardware shader.
5. Add a synthetic renderer test when the rule can be isolated.

## Trace evidence

For CPU and scheduler traces:

- Compare opcode, register writes, and memory writes in order.
- Preserve module identity for overlapping addresses.
- Document intentionally ignored native-kernel ranges.
- Never ignore a mismatch merely because later frames look correct.
- Keep trace bounds small enough to isolate the first divergence, then expand.

## Protected evidence policy

Keep these ignored:

- ISO/PBP/PRX/BIN/DAT files from the game.
- Generated game C and relocated images.
- Saves and memstick trees.
- CPU/HLE traces containing game data.
- RAM dumps, screenshots, frame captures, and audio/video extracts.

Commit instead:

- Cryptographic hashes and byte sizes.
- Synthetic modules and homebrew fixtures.
- Reduced numeric reports.
- Finding IDs and reproduction commands.
- Pass/fail summaries without protected payloads.

## Shared-change regression matrix

Before moving a shared runtime commit into title projects:

| Check | Shared runtime | Killzone | Metal Gear Acid | New title |
|---|---:|---:|---:|---:|
| Synthetic unit regression | Required | — | — | — |
| Complete shared tests | Required | — | — | — |
| Runtime-only compile | Required | Required | As available | As available |
| Parent public tests | — | Required | Required | Required |
| Deterministic protected smoke | — | Required when affected | Required when affected | Required |
| Full route or stage | — | For release-critical changes | For release-critical changes | Before release |
| Pin/docs update | — | Required | Required | Required |

## Completion checklist

- [ ] Input and dependency revisions are pinned.
- [ ] `git diff --check` passes.
- [ ] Shared Python and native tests pass.
- [ ] Function catalog and exact-cache tests pass when reuse code is affected.
- [ ] Consumer public tests and compile targets pass.
- [ ] No unexpected dispatch misses, permissive returns, or module fallbacks.
- [ ] Interpreter miss/profile reports pass the zero-or-reviewed policy gate.
- [ ] Native samples agree with exclusive CPU timing when CPU work is affected.
- [ ] Deterministic input and frame/call manifests pass.
- [ ] Save, media, and filesystem routes pass when affected.
- [ ] Performance thresholds and soak duration pass when affected.
- [ ] Tracked-content audit finds no game-derived artifacts.
- [ ] Findings, title status, and runtime pin documentation are current.
- [ ] A clean recursive clone reproduces the public build.
