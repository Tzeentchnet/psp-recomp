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
```

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

The final route must state which switches are part of the supported product
configuration and leave investigation-only switches unset.

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
- [ ] Deterministic input and frame/call manifests pass.
- [ ] Save, media, and filesystem routes pass when affected.
- [ ] Performance thresholds and soak duration pass when affected.
- [ ] Tracked-content audit finds no game-derived artifacts.
- [ ] Findings, title status, and runtime pin documentation are current.
- [ ] A clean recursive clone reproduces the public build.
