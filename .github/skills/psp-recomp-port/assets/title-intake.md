# PSP Recompilation Title Intake

Copy this checklist into the new title repository and complete it with measured
evidence.

## Identity

- Title:
- Region:
- Disc ID:
- Version:
- ISO byte size:
- ISO SHA-1:
- Runtime repository/commit:
- PPSSPP revision/settings:

## Executables

| Path | Plaintext/encrypted | Size | SHA-1 | Role |
|---|---|---:|---|---|
| `PSP_GAME/SYSDIR/EBOOT.BIN` | | | | |
| `PSP_GAME/SYSDIR/BOOT.BIN` | | | | |
| Main PRX | | | | |

## Main module

- Module name:
- Base:
- Entry:
- GP:
- Text/image size:
- Argument bytes/length:
- Start thread UID:
- Next UID:
- Priority:
- Stack size:
- Attributes:
- Initial GE list ID:
- Explicit computed entries:
- Reviewed catalog matches:
- Normalized matches awaiting review:
- Private codegen cache location:

## Dynamic modules

- Module roots:
- Plaintext PRX count:
- Encrypted PRX count:
- Overlay address reuse:
- Raw-LBN paths:
- Import/export private libraries:

| Exact module identity | Load path/hash | Initial policy | Evidence |
|---|---|---|---|
| | | Native / interpreter / HLE / reject | |

## Content and storage

- Main asset archives:
- Duplicate-record semantics:
- DLC/install data:
- Writable devices and aliases:
- Save identifiers/formats:
- User-supplied external content:

## First deterministic routes

| Route | Input script | Expected checkpoint | Evidence |
|---|---|---|---|
| Boot | Neutral | | |
| Front end | | | |
| New game | | | |
| Save/load | | | |
| Movie/audio | | | |
| Vertical slice | | | |

## Initial unknowns

- Unsupported instructions:
- Missing imports/HLE:
- Unresolved indirect targets:
- Unsupported module formats/relocations:
- Graphics differences:
- Audio/video differences:
- Filesystem/save differences:

## Evidence boundary

- [ ] Game-derived files are ignored.
- [ ] Exact hashes are tracked.
- [ ] Reduced reports are safe to publish.
- [ ] Findings use stable identifiers.
- [ ] Shared candidates have a planned synthetic test.
