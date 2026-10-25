# Safe Function Recognition and Reuse

The toolkit has two deliberately separate reuse systems:

1. A **public signature catalog** that transfers reviewed function identity and
   provenance.
2. A **local exact codegen cache** that reuses generated C only when every
   output-affecting input matches.

Normalized catalog matches never authorize code reuse.

## Trust boundary

| Data | May be committed? | Can drive generated-code reuse? |
|---|---:|---:|
| Name, library, hashes, size, provenance, license | Yes, after review | No |
| Normalized function match | Yes, as identity metadata | No |
| Exact function match | Yes, as identity metadata | No |
| Generated function C | No; local game-derived build data | Yes, through exact cache key only |
| Compiled generated object | No; use a trusted local compiler cache | Only through compiler/cache validation |

The catalog contains no machine-code bytes or generated C. The local cache may
contain generated game code and must remain ignored and private.

## Public function catalog

The repository catalog is
[`assets/functions/catalog.toml`](../assets/functions/catalog.toml). It starts
empty intentionally: entries must have reviewed identity, source revision, and
license metadata.

Each entry records:

```toml
[[function]]
name = "memcpy"
library = "libc"
normalized_sha256 = "<64 hex digits>"
instruction_count = 42
span = 168
exact_sha256 = ["<one or more exact variants>"]
source = "PSPSDK libc"
source_revision = "<commit or release>"
license = "BSD-2-Clause"
note = "Optional review note"
```

No raw instructions are stored.

### Fingerprint model

`allegrex-flow-mask-v1-sha256` hashes:

- Instruction offsets relative to the function entry.
- The analyzed control-flow instruction set.
- Opcode and register fields.
- Masked branch, jump, address, and immediate fields.
- Instruction count and covered span.

The exact variant hash uses the same relative layout with unmasked instruction
words. A normalized match can recognize a relocated or immediate-different
variant, but collisions remain possible. The loader therefore rejects two
catalog identities with the same normalized key.

### Validate the catalog

```powershell
python tools\function_catalog.py check
```

Validation rejects unsupported schema versions or algorithms, unknown fields,
missing provenance, malformed hashes or sizes, and ambiguous identities.

For a private catalog shared by local projects, set:

```powershell
[Environment]::SetEnvironmentVariable(
  "PSP_RECOMP_FUNCTION_CATALOG",
  "$env:LOCALAPPDATA\psp-recomp\function-catalog\catalog.toml",
  "User"
)
```

`PSP_RECOMP_FUNCTION_CATALOG` changes the default used by `check`, `add`, and
`match`. An explicit `--catalog` still takes precedence. Keep local research
identities outside repositories; copy an entry into the public catalog only
after its source revision and license have been reviewed.

### Add a reviewed identity

```powershell
python tools\function_catalog.py add path\to\module.elf `
  --base 0x08804000 `
  --address 0x08812340 `
  --name memcpy `
  --library libc `
  --source "PSPSDK libc" `
  --source-revision "<commit-or-release>" `
  --license "BSD-2-Clause"
```

Use `--jumptables`, `--overrides`, or repeated `--function-entry` arguments
when the title needs the same control-flow hints as code generation.

Adding another exact variant to the same reviewed identity extends its
`exact_sha256` array. Attempting to assign the same normalized fingerprint to
a different identity fails explicitly.

Do not add a proprietary title function merely because it resembles a known
library routine. Establish its source and licensing provenance first. Use a
title-local ignored catalog for unreviewed research.

### Match a new module

```powershell
python tools\function_catalog.py match path\to\module.elf `
  --base 0x08804000 `
  --output build\mygame\function_matches.toml `
  --symbols build\mygame\function_symbols.csv
```

The report classifies each result as:

- `exact` — unmasked instruction flow matches a reviewed exact variant.
- `normalized` — structural flow matches after address/immediate masking.

Both are naming/research evidence only. Review normalized matches before
importing their symbols into Ghidra or title configuration.

## Local exact codegen cache

Enable caching with a private local directory:

```powershell
[Environment]::SetEnvironmentVariable(
  "PSP_RECOMP_CODEGEN_CACHE",
  "$env:LOCALAPPDATA\psp-recomp\codegen-cache",
  "User"
)
python tools\codegen.py path\to\module.elf build\mygame\recomp.c `
  --base=0x08804000 --split
```

Or pass the directory explicitly:

```text
--cache-dir=build\mygame\.psp-recomp-cache
```

`.psp-recomp-cache/` is ignored by this repository. A user-data directory is
preferable when several title projects should share exact entries.

### Exact cache key

The key includes:

- Generator/cache source (`codegen.py`, `codegen_ext.py`, `analyze.py`, and
  `function_reuse.py`).
- The shared generated-code ABI in `src/rt/recomp.h`.
- Function entry and symbol prefix.
- Exact relocated instruction addresses and words.
- Analyzed labels and constant `jalr` targets.
- Direct-call known/unknown decisions.
- Relevant jump-table targets and their known/unknown decisions.
- `jal .+8` classification and instruction hooks.
- Runtime-JAL and live-code-verification options.

Use repeated `--cache-abi=PATH` arguments for additional title ABI files whose
changes must invalidate cached output.

The cache does **not** use normalized catalog fingerprints. A function that
only resembles a catalog entry is generated normally.

### Integrity and invalidation

Each cache record has:

- A SHA-256 key over canonical context.
- A SHA-256 checksum over its generated payload.
- A schema version.

Missing entries are generated and written atomically. Corrupt entries,
metadata mismatches, or nondeterministic output for an existing exact key fail
explicitly instead of silently regenerating or accepting questionable code.

Changing generator code, runtime ABI, instructions, relocations, call-target
identity, hints, hooks, or generation options produces a different key.

### Build acceleration

The codegen cache avoids retranslating exact functions. Generated files are
also written only when their content changes, preserving timestamps for
incremental builds. Split code generation removes stale numbered translation
units after writing the current set.

The cache stores C, not native object files. Use a trusted local `ccache` or
`sccache` configuration to reuse compiler output; compiler identity, flags,
headers, and target ABI remain the compiler cache's responsibility.

## Safety rules

- Never commit or publish a local codegen cache.
- Never use an untrusted downloaded cache.
- Never generate code from a normalized catalog match.
- Never omit title ABI inputs that affect generated declarations or calls.
- Treat a cache integrity error as a failed build.
- Run ordinary traces and title checkpoints after cache-enabled generation;
  cache correctness does not replace runtime validation.

## Existing-title validation

The implementation was exercised against the verified Killzone: Liberation
v1.20 and Metal Gear Acid USA v1.00 modules on 2026-10-22:

| Title scope | Emitted functions | Cacheable functions | Unchanged rerun |
|---|---:|---:|---:|
| Killzone `KZL.PRX` with production jump-table, override, and hot/cold hints | 42,600 | 42,298 | 42,298 hits, 0 misses, 0 writes |
| Metal Gear Acid main module plus eight configured native modules | 16,541 | 16,288 | 16,288 hits, 0 misses, 0 writes |

Both reruns preserved every generated C/H content hash and modification time,
emitted no trapping stubs, and compiled and linked with clang-cl against the
current shared runtime.

A temporary local catalog built from Killzone's 37 previously reviewed PPSSPP
known-function identities matched all 37 exactly in Killzone. It produced five
normalized Metal Gear Acid candidates: `__extendsfdf2`, `__adddf3`,
`__subdf3`, `__muldf3`, and `__negdf2`. They were reported for review and did
not authorize generated-code reuse.

The shared exact cache found one cross-title entry: a two-instruction function
at `0x088a6ba8` with the same address, words, flow decisions, generator/ABI
fingerprint, namespace, and options. All other first-pass Metal Gear Acid
functions were generated normally.

The original temporary catalog lacked the PPSSPP source revision required for
publication, so none of those entries were added to the public catalog.
Temporary extracted modules and generated C were removed after validation.
An exact cache may be retained only in an explicitly configured private
user-data directory.

## Tests

Focused coverage:

```powershell
python -m unittest tools.tests.test_function_reuse
python -m unittest tools.tests.test_codegen_dispatch
```

The complete shared validation remains:

```bash
python -m unittest discover -s tools/tests -p "test_*.py"
make test-runtime
```
