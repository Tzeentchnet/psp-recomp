# New Title Project Template

Use this template when creating a source-only repository for another PSP
recompilation. Keep title facts in that repository and contribute only generic
mechanisms and synthetic tests back to `psp-recomp`.

## Recommended layout

```text
<Title>-Recomp/
├── .gitmodules
├── CMakeLists.txt
├── README.md
├── CREDITS.md
├── LICENSE
├── config/
│   ├── <slug>.toml
│   ├── <slug>_neutral.pad
│   └── <slug>_dispatch_misses.txt
├── docs/
│   ├── README.md
│   ├── STATUS.md
│   ├── PLAN.md
│   ├── FINDINGS.md
│   ├── REPRODUCTION.md
│   └── UPSTREAM.md
├── external/
│   └── psp-recomp/
├── tools/
│   └── <slug>/
└── build/ and out/       # ignored
```

Not every project needs every file on day one, but the separation between
configuration, evidence-backed findings, reproduction, and shared framework
work should remain.

## Runtime pin

Track `psp-recomp` `main` while pinning an exact gitlink:

```ini
[submodule "external/psp-recomp"]
    path = external/psp-recomp
    url = https://github.com/Tzeentchnet/psp-recomp.git
    branch = main
```

The branch supports intentional updates; the gitlink makes every build
reproducible.

Use the shared [function catalog and exact local cache](FUNCTION_REUSE.md) for
recognition and repeat generation. Keep unreviewed title fingerprints and all
generated cache entries ignored.

## Minimum title configuration

```toml
# <Title>, <region/disc ID>, <version>.

[runtime]
repository = "https://github.com/Tzeentchnet/psp-recomp.git"
branch = "main"
commit = "<40-hex-runtime-commit>"

[disc]
id = "<DISC_ID>"
version = "<VERSION>"
iso_size = 0
iso_sha1 = "<40-hex-sha1>"
plaintext_module_roots = []

[disc.files]
"PSP_GAME/SYSDIR/EBOOT.BIN" = "<sha1>"
"PSP_GAME/SYSDIR/BOOT.BIN" = "<sha1>"

[module]
file = "<path-to-main-module>"
name = "<module-name>"
base = 0x00000000
entry = 0x00000000
text_size = 0x00000000
image_size = 0x00000000
function_entries = []

[module.start_thread]
uid = 0x000
priority = 0x00
stack_size = 0x00000
attr = 0
next_uid = 0x000
initial_ge_list_id = 0
argument = "<exact-measured-argument>"
```

Add dynamic-module entries only after recording path, hash, base policy,
exports/imports, runtime identity, and native/interpreter/HLE policy.

## Initial README contract

State:

- Exact supported disc ID, region, version, size, and hash.
- Source-only status and user-owned input requirement.
- Current validated milestone and explicit limitations.
- Toolchain requirements.
- Reproducible configure/generate/build/run commands.
- Pinned runtime branch and commit.
- Links to status, findings, reproduction, and credits.

Do not imply full-game compatibility from a boot or menu milestone.

## Findings format

```markdown
### <PREFIX>-F-001 — Short factual title

**Status:** Confirmed | Partial | Hypothesis | Superseded

**Inputs**
- Disc/module hash:
- Runtime commit:
- PPSSPP revision/settings:

**Observation**
State only what the evidence shows.

**Evidence**
List commands, trace bounds, addresses, frames, metrics, or synthetic tests.

**Interpretation**
Explain the likely meaning separately from the observation.

**Impact**
Classify as:
- Title configuration
- Title code/policy
- Shared runtime/codegen candidate
- Diagnostic only

**Validation**
Name the regression or route that must continue to pass.
```

Never renumber a published finding. Mark it superseded and link to the
replacement.

## Module policy table

Maintain a table such as:

| Module identity | Path/hash | Policy | Reason | Validation |
|---|---|---|---|---|
| Main module | Exact config | Native generated | Static title code | Boot trace + checkpoints |
| Sony media module | Exact identity | HLE | Shared implementation | Movie route |
| Title overlay | Exact identity | Interpreter, then native | Runtime-mutated code | Overlay checkpoint |
| Encrypted unknown | Exact identity | Reject | Unsupported input | Explicit loader error |

Broad wildcard replacement is not an acceptable policy.

## Validation matrix

Track at least:

| Route | Input fixture | Expected milestone | Required subsystems | Status |
|---|---|---|---|---|
| Boot | Neutral | First HLE/thread checkpoint | CPU, scheduler, filesystem | |
| Front end | Scripted | Stable menu frame | Input, GE, audio | |
| New game | Scripted | First playable state | Modules, saves, media | |
| Save/load | Isolated memstick | Round trip | Savedata, filesystem | |
| Movie | Scripted | Clean return to title | MPEG, ATRAC, timing | |
| Vertical slice | Scripted/manual | Completed objective | All active systems | |
| Soak | Fixed route | Duration/frames | Stability, performance | |

Record exact presentation indices, trace bounds, or completion conditions.

## Shared-change proposal

Before modifying `psp-recomp`, answer:

1. Is this a reusable mechanism or a title value/policy?
2. Can it be tested with a synthetic fixture?
3. What strict failure replaces the current unsupported case?
4. Which existing titles can regress?
5. Which public and protected checks will be run?
6. Does the shared documentation need a new `PSP-F-xxx` finding?

Keep the title workaround isolated until those questions have evidence-backed
answers.

## Release checklist

- [ ] Exact source identity enforced.
- [ ] No game-derived files are tracked.
- [ ] Runtime gitlink and branch metadata are current.
- [ ] Public tests run from a clean recursive clone.
- [ ] Protected acceptance matrix is complete.
- [ ] Permissive diagnostic modes are disabled.
- [ ] Credits and licenses cover every dependency and ported component.
- [ ] Corresponding-source packaging includes the exact runtime tree.
