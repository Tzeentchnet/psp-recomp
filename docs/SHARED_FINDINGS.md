# Shared PSP Recompilation Findings

These findings are stable lessons supported by both existing title projects.
They describe engineering invariants and reusable mechanisms, not game
constants. Use the `PSP-F-xxx` identifiers when discussing shared work.

## Shared runtime versus title project

| Shared `psp-recomp` responsibility | Title project responsibility |
|---|---|
| ELF/PRX parsing and relocations | Supported disc/module hashes |
| Import/export library and NID identity | Main module base, entry, GP, and thread contract |
| Scheduler, callbacks, UID primitives | Exact initial UID/GE state consumed by the title |
| Generated dispatch and interpreter handoff | Computed-entry allowlists and module identity |
| Module allocation, start, unload, and overlay reuse | Per-module native/interpreter/HLE/reject policy |
| PSP GE semantics and renderer backends | Title checkpoint manifests and acceptance thresholds |
| Generic audio, media, input, filesystem, and savedata APIs | Asset archives, DLC layout, patches, hidden controls |
| Diagnostics and comparison tools | Scripted routes, protected evidence, and release status |

If a behavior contains a game address, content path, module hash, menu state,
or control side channel, it is title policy until proven otherwise.

## Provenance and evidence

### PSP-F-001 — Exact source identity is part of the program

Disc version, region, byte size, ISO hash, module hashes, and runtime revision
must be pinned before addresses or traces are meaningful. Reject mismatches
rather than trying to continue with a nearby release.

### PSP-F-002 — PSP titles do not share one boot-file relationship

`EBOOT.BIN`, `BOOT.BIN`, and the main PRX may be encrypted, identical, thin
loaders, or independent modules. Survey each title before deciding what to
translate or skip.

### PSP-F-003 — Protected evidence stays outside source control

ISOs, decrypted executables, generated game C, saves, traces, captures, and RAM
dumps are reproducible evidence but are not redistributable source. Commit
hashes, reduced metrics, synthetic fixtures, and reproduction commands.

### PSP-F-004 — Findings need stable identifiers and measured support

Separate observation from interpretation. A finding should name the exact
input revision and include a command, trace, frame, metric, or threshold.

## Static analysis and code generation

### PSP-F-005 — Translation coverage is more than a function count

Track executable ranges, relocations, imports, exports, GP values, jump tables,
indirect calls, computed callbacks, and trapping stubs. A high discovered
function percentage can still miss the one callback needed for progress.

### PSP-F-006 — VFPU and floating-point fidelity are first-class

VFPU/COP2 forms, prefixes, packed conversions, transcendental behavior, NaNs,
rounding, FTZ/DAZ, and contraction settings can affect game logic as well as
graphics. Validate raw bit patterns where possible.

### PSP-F-007 — Manual entries and overrides require evidence

Static analysis cannot prove every indirect target. Keep title-supplied
entries and jump-table overrides in title configuration, with a trace or
runtime observation explaining each one.

### PSP-F-008 — Generated dispatch must preserve module identity

An address alone is insufficient when overlays reuse guest ranges. Dispatch
tables, miss logs, profiles, and code-identity checks must include the active
module.

## Boot, kernel, and HLE behavior

### PSP-F-009 — The standalone entry state must be measured

Argument bytes, registers, stack, GP, return address, thread UID, next UID,
priority, stack size, GE list ID, and bootloader memory writes are part of the
title contract.

### PSP-F-010 — HLE scope should follow observed execution

Start with the boot route's actual imports and missing calls. Expand coverage
through diagnostics and deterministic routes instead of assuming every PSP
title needs the same HLE surface immediately.

### PSP-F-011 — Thread and callback semantics are observable

Stack initialization, priority scheduling, return stubs, callback-enabled
waits, per-thread GP, UID allocation, and object lifetimes can change control
flow. Host-thread approximations are not automatically equivalent.

### PSP-F-012 — Unsupported behavior must fail explicitly

Unknown instructions, bad relocations, missing imports, invalid modules,
memory faults, interpreter limits, and impossible renderer states should
produce actionable failures. Fake success and permissive returns invalidate
acceptance results.

## Dynamic modules and hybrid execution

### PSP-F-013 — Dynamic PRXs require a complete lifecycle

A useful loader must support path and file-descriptor loading, PSP allocation,
segment copy, BSS clearing, relocations, import binding, export registration,
GP setup, start threads, unload, and address reuse.

### PSP-F-014 — Module replacement is explicit title policy

Choose guest, native, interpreted, HLE, or reject using validated identity.
Never HLE-replace every module matching a broad name prefix.

### PSP-F-015 — Hybrid execution is a correctness tool

Generated native code, the reference interpreter, and HLE must call one
another in both directions while preserving guest state. Interpreter fallback
enables progress without hiding unsupported semantics.

### PSP-F-016 — Native overlay entry requires live-code verification

Titles can patch code or JAL targets at runtime. Verify the active guest bytes
at each native wrapper entry when required; do not cache a successful check
across later mutations.

### PSP-F-017 — Interpreter fallback must be bounded and scheduler-aware

Long interpreted loops cannot grow the host stack indefinitely or starve
virtual time. Bound each invocation, preserve explicit failure, and hand
control back to generated code when a verified native target becomes active.

## Graphics

### PSP-F-018 — Software rendering is the primary pixel oracle

Establish deterministic software frames before optimizing D3D12 or Vulkan.
Without a known reference, hardware output can look plausible while violating
PSP sampling, blending, depth, or framebuffer rules.

### PSP-F-019 — PSP texture and fragment math is not host-default math

Texture formats, CLUT addressing, wrapping, clamping, 8-bit bilinear weights,
integer texture functions, fog, color tests, masks, dithering, and unusual
blend factors require explicit PSP behavior.

### PSP-F-020 — Framebuffer identity includes guest layout

Cache and synchronization keys must account for address, stride, format,
dimensions, and ownership. Render-to-texture, aliases, copies, CPU access, and
presentation can require ordered snapshots or readback.

### PSP-F-021 — Residuals should be classified by feature and edge

Use deterministic manifests and analyze deltas by draw type, texture mode,
blend path, and geometry edge. A single global pixel score obscures the source
of hardware differences.

## Audio and video

### PSP-F-022 — Guest timing and host device timing are separate

Track guest buffer completion, host queue lead, underruns, drops, drift, and
load-gap recovery independently. Pre-rendered silence can hide starvation
while advancing guest-visible time incorrectly.

### PSP-F-023 — Media EOF is decoder state, not only input exhaustion

MPEG/PSMF playback may emit reordered tail frames after the input ring drains.
Match PPSSPP's AU delays, timestamps, ring progress, decoder negotiation, and
completion semantics.

### PSP-F-024 — Media validation needs audiovisual routes

A decoder that accepts input is not sufficient. Validate frame cadence, audio
continuity, A/V drift, skip/finish behavior, and transition back to gameplay.

## Input, filesystems, and saves

### PSP-F-025 — Validate input as a title state transition

Scripted button edges must lead to expected menus, scenes, or gameplay state.
Host input logs alone do not prove that PSP sampling and scheduling are right.

### PSP-F-026 — Generic bindings and title extensions must remain separate

Standard PSP keyboard, mouse, and gamepad mapping belongs in the runtime.
Hidden movement profiles, native aim channels, or game-memory hooks require an
explicit title build flag and title-side documentation.

### PSP-F-027 — Writable PSP devices need an explicit safe root

Keep `disc0:` immutable. Map `ms0:` and related aliases to a configured
per-user tree, preserve PSP path behavior, and reject traversal. Do not fall
back silently to an arbitrary working directory.

### PSP-F-028 — Save-state cases are independent

Test no memory stick, empty stick, no save, existing save, new save, secure
save, legacy migration, and reload separately. Passing one load path does not
validate save creation or compatibility.

### PSP-F-029 — Title archive semantics must be preserved

Duplicate physical archive records can encode streaming locality or cursor
selection. Do not deduplicate or reorder title content without tracing the
reader's actual selection rules.

## Validation and release

### PSP-F-030 — Acceptance must climb a validation ladder

Use:

1. Unit tests.
2. Compile-only shared runtime.
3. Full title build.
4. One-function trace.
5. Bounded call or scheduler trace.
6. Deterministic frame checkpoint.
7. Scripted front end.
8. Playable vertical slice.
9. Full stage or mission.
10. Save/load and media routes.
11. Long soak and campaign matrix.

Higher levels do not replace lower-level diagnostics.

### PSP-F-031 — PPSSPP and native execution are complementary oracles

Compare instruction state, HLE calls, presentation indices, frames, input
edges, filesystem activity, and timing. Use deterministic inputs and pin the
PPSSPP revision and settings.

### PSP-F-032 — Shared changes require cross-title regression

A generic runtime or codegen change needs:

- A synthetic regression test in `psp-recomp`.
- The complete shared test suite.
- Compile/build validation in affected consumer projects.
- Deterministic title smoke routes where protected evidence is available.
- Updated title pins and documentation.

### PSP-F-033 — Release configurations must disable diagnostic escape hatches

Permissive dispatch, forced interpreter/native-module overrides, disabled
audio, unsafe PSO persistence, and other investigation switches may isolate a
problem but cannot define a successful release result.

### PSP-F-034 — Reproducibility is part of completion

A clean clone must be able to initialize the pinned runtime, verify user-owned
inputs, generate code, build, run public tests, and produce corresponding
source without relying on untracked developer state.

### PSP-F-035 — Function identity and generated-code reuse have different trust levels

A normalized instruction fingerprint can transfer a reviewed function name and
provenance, but it cannot prove that generated code is reusable. Shared
catalogs must contain metadata rather than game bytes or generated C.

### PSP-F-036 — Generated functions may be cached only from exact generation context

Local reuse must key exact relocated instructions together with generator and
runtime ABI revisions, control-flow results, call-target identity, title
hints, hooks, symbol namespace, and generation options. Corrupt or conflicting
entries fail the build. See [safe function reuse](FUNCTION_REUSE.md).

### PSP-F-037 — Real cross-title matches preserve the catalog/cache boundary

A temporary catalog of 37 reviewed Killzone library identities found five
normalized soft-float candidates in Metal Gear Acid, while the exact codegen
cache reused only one two-instruction function whose complete generation
context was identical. Normalized identity candidates therefore remain useful
for naming and provenance without weakening exact generated-code reuse.

### PSP-F-038 — Profile across guest, scheduler, frontend, backend, and GPU boundaries

An aggregate host-frame time cannot distinguish generated guest work from
intentional scheduler waiting, GE decode, vertex processing, synchronization,
backend recording, or delayed GPU execution. Use exclusive fiber-aware CPU
categories, workload counters, and GPU samples tagged with their source host
flip. Warm up after fast-forward before collecting an acceptance window.

### PSP-F-039 — Ordered framebuffer snapshots should copy proven dirty regions

Framebuffer-feedback correctness requires the sampled snapshot to reflect all
writes ordered before that draw, but it does not require copying a target's
complete scaled envelope for every refresh. Take a complete snapshot when the
active target changes, track subsequent dirty draw rectangles, and refresh the
union of accumulated dirt and the requested sample area. Clear dirty tracking
only after that union copy makes the snapshot globally current. Record both
snapshot count and copied physical pixels, and retain a full-copy diagnostic
mode for differential validation.

## Common failure patterns

- Copying another title's boot state.
- Treating every import stub as ordinary generated code.
- Faking module load success.
- Identifying overlays only by address.
- Replacing modules through broad filename patterns.
- Accepting permissive-dispatch output.
- Optimizing hardware graphics before software parity.
- Treating media input exhaustion as decoded EOF.
- Using relative writable-device fallbacks.
- Deduplicating title archives without tracing selection.
- Recording conclusions without a stable finding ID and reproduction path.

See the [title findings](titles/README.md) for the concrete cases that produced
these rules.
