#!/usr/bin/env python3
"""Safe function fingerprints, provenance catalogs, and local codegen caching."""

from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path
import re
import struct
import tempfile
from typing import Any, Iterable


CATALOG_SCHEMA_VERSION = 1
CACHE_SCHEMA_VERSION = 1
FINGERPRINT_ALGORITHM = "allegrex-flow-mask-v1-sha256"
_SHA256_RE = re.compile(r"[0-9a-f]{64}")


class CatalogError(ValueError):
    pass


class CacheError(RuntimeError):
    pass


def normalize_instruction(word: int) -> int:
    """Mask address/immediate fields while retaining opcode and register identity."""
    opcode = word >> 26
    if opcode in (2, 3):
        return word & 0xFC000000
    if (
        opcode == 1
        or 4 <= opcode <= 15
        or 20 <= opcode <= 23
        or 32 <= opcode <= 63
    ):
        return word & 0xFFFF0000
    return word


def fingerprint_instructions(
    start: int, instructions: Iterable[tuple[int, int]]
) -> dict[str, Any]:
    ordered = sorted(instructions)
    if not ordered:
        raise ValueError("cannot fingerprint an empty function")
    addresses = [address for address, _ in ordered]
    if len(addresses) != len(set(addresses)):
        raise ValueError("function fingerprint contains duplicate addresses")
    if any(address & 3 for address in addresses):
        raise ValueError("function fingerprint contains an unaligned address")

    exact = hashlib.sha256()
    normalized = hashlib.sha256()
    exact.update(b"psp-recomp-exact-flow-v1\0")
    normalized.update(FINGERPRINT_ALGORITHM.encode("ascii") + b"\0")
    for address, word in ordered:
        relative = (address - start) & 0xFFFFFFFF
        exact.update(struct.pack("<II", relative, word & 0xFFFFFFFF))
        normalized.update(
            struct.pack(
                "<II", relative, normalize_instruction(word & 0xFFFFFFFF)
            )
        )

    return {
        "algorithm": FINGERPRINT_ALGORITHM,
        "normalized_sha256": normalized.hexdigest(),
        "exact_sha256": exact.hexdigest(),
        "instruction_count": len(ordered),
        "span": addresses[-1] - addresses[0] + 4,
    }


def catalog_key(fingerprint: dict[str, Any]) -> tuple[str, int, int]:
    return (
        fingerprint["normalized_sha256"],
        fingerprint["instruction_count"],
        fingerprint["span"],
    )


def _require_string(entry: dict[str, Any], field: str) -> str:
    value = entry.get(field)
    if not isinstance(value, str) or not value.strip():
        raise CatalogError(f"catalog function.{field} must be a non-empty string")
    return value


def _validate_sha256(value: Any, field: str) -> str:
    if not isinstance(value, str) or not _SHA256_RE.fullmatch(value.lower()):
        raise CatalogError(f"{field} must be a 64-digit SHA-256")
    return value.lower()


def validate_catalog(catalog: dict[str, Any]) -> dict[str, Any]:
    unknown_top_level = set(catalog) - {
        "schema_version",
        "fingerprint_algorithm",
        "function",
    }
    if unknown_top_level:
        raise CatalogError(
            "unknown catalog fields: " + ", ".join(sorted(unknown_top_level))
        )
    if catalog.get("schema_version") != CATALOG_SCHEMA_VERSION:
        raise CatalogError(
            f"unsupported catalog schema {catalog.get('schema_version')!r}; "
            f"expected {CATALOG_SCHEMA_VERSION}"
        )
    if catalog.get("fingerprint_algorithm") != FINGERPRINT_ALGORITHM:
        raise CatalogError(
            "catalog fingerprint_algorithm does not match "
            f"{FINGERPRINT_ALGORITHM}"
        )
    functions = catalog.get("function", [])
    if not isinstance(functions, list):
        raise CatalogError("catalog function entries must be an array")

    normalized_keys: dict[tuple[str, int, int], str] = {}
    exact_owners: dict[str, str] = {}
    validated = []
    allowed_entry_fields = {
        "name",
        "library",
        "normalized_sha256",
        "instruction_count",
        "span",
        "exact_sha256",
        "source",
        "source_revision",
        "license",
        "note",
    }
    for raw in functions:
        if not isinstance(raw, dict):
            raise CatalogError("catalog function entry must be a table")
        entry = dict(raw)
        unknown_fields = set(entry) - allowed_entry_fields
        if unknown_fields:
            raise CatalogError(
                "unknown function fields: "
                + ", ".join(sorted(unknown_fields))
            )
        name = _require_string(entry, "name")
        _require_string(entry, "source")
        _require_string(entry, "source_revision")
        _require_string(entry, "license")
        for field in ("library", "note"):
            if field in entry:
                _require_string(entry, field)
        entry["normalized_sha256"] = _validate_sha256(
            entry.get("normalized_sha256"), f"{name}.normalized_sha256"
        )
        for field in ("instruction_count", "span"):
            value = entry.get(field)
            if not isinstance(value, int) or value <= 0:
                raise CatalogError(f"{name}.{field} must be a positive integer")
        if entry["span"] & 3:
            raise CatalogError(f"{name}.span must be four-byte aligned")
        if entry["instruction_count"] * 4 > entry["span"]:
            raise CatalogError(
                f"{name}.instruction_count cannot exceed its covered span"
            )
        exact_values = entry.get("exact_sha256", [])
        if not isinstance(exact_values, list):
            raise CatalogError(f"{name}.exact_sha256 must be an array")
        entry["exact_sha256"] = sorted(
            {
                _validate_sha256(value, f"{name}.exact_sha256")
                for value in exact_values
            }
        )
        key = catalog_key(entry)
        previous = normalized_keys.get(key)
        if previous is not None:
            raise CatalogError(
                f"normalized fingerprint collision between {previous!r} "
                f"and {name!r}"
            )
        normalized_keys[key] = name
        for digest in entry["exact_sha256"]:
            previous = exact_owners.get(digest)
            if previous is not None:
                raise CatalogError(
                    f"exact fingerprint collision between {previous!r} "
                    f"and {name!r}"
                )
            exact_owners[digest] = name
        validated.append(entry)

    return {
        "schema_version": CATALOG_SCHEMA_VERSION,
        "fingerprint_algorithm": FINGERPRINT_ALGORITHM,
        "function": validated,
    }


def load_catalog(path: os.PathLike[str] | str) -> dict[str, Any]:
    import tomllib

    with open(path, "rb") as stream:
        return validate_catalog(tomllib.load(stream))


def _toml_string(value: str) -> str:
    return json.dumps(value, ensure_ascii=True)


def _catalog_text(catalog: dict[str, Any]) -> str:
    catalog = validate_catalog(catalog)
    lines = [
        "# Public function identities only. Never store game bytes or generated C here.",
        f"schema_version = {CATALOG_SCHEMA_VERSION}",
        f"fingerprint_algorithm = {_toml_string(FINGERPRINT_ALGORITHM)}",
        "",
    ]
    order = (
        "name",
        "library",
        "normalized_sha256",
        "instruction_count",
        "span",
        "exact_sha256",
        "source",
        "source_revision",
        "license",
        "note",
    )
    functions = sorted(
        catalog["function"],
        key=lambda item: (
            item.get("library", ""),
            item["name"],
            item["normalized_sha256"],
        ),
    )
    for entry in functions:
        lines.append("[[function]]")
        for field in order:
            if field not in entry:
                continue
            value = entry[field]
            if isinstance(value, str):
                rendered = _toml_string(value)
            elif isinstance(value, int):
                rendered = str(value)
            elif isinstance(value, list):
                rendered = "[" + ", ".join(_toml_string(v) for v in value) + "]"
            else:
                raise CatalogError(
                    f"cannot serialize catalog field {field}: {type(value).__name__}"
                )
            lines.append(f"{field} = {rendered}")
        lines.append("")
    return "\n".join(lines)


def write_text_if_changed(
    path: os.PathLike[str] | str, text: str, encoding: str = "ascii"
) -> bool:
    target = Path(path)
    try:
        if target.read_text(encoding=encoding) == text:
            return False
    except FileNotFoundError:
        pass
    target.parent.mkdir(parents=True, exist_ok=True)
    descriptor, temporary_name = tempfile.mkstemp(
        prefix=f".{target.name}.", suffix=".tmp", dir=target.parent
    )
    temporary = Path(temporary_name)
    try:
        with os.fdopen(descriptor, "w", encoding=encoding, newline="\n") as stream:
            stream.write(text)
        os.replace(temporary, target)
    except Exception:
        temporary.unlink(missing_ok=True)
        raise
    return True


def write_catalog(
    path: os.PathLike[str] | str, catalog: dict[str, Any]
) -> bool:
    return write_text_if_changed(path, _catalog_text(catalog), encoding="ascii")


def add_catalog_entry(
    catalog: dict[str, Any], entry: dict[str, Any]
) -> tuple[dict[str, Any], bool]:
    catalog = validate_catalog(catalog)
    candidate = validate_catalog(
        {
            "schema_version": CATALOG_SCHEMA_VERSION,
            "fingerprint_algorithm": FINGERPRINT_ALGORITHM,
            "function": [entry],
        }
    )["function"][0]
    key = catalog_key(candidate)
    identity_fields = (
        "name",
        "library",
        "source",
        "source_revision",
        "license",
    )
    for existing in catalog["function"]:
        if catalog_key(existing) != key:
            continue
        if any(existing.get(field) != candidate.get(field) for field in identity_fields):
            raise CatalogError(
                "normalized fingerprint already belongs to a different "
                f"identity: {existing['name']!r}"
            )
        merged = sorted(
            set(existing["exact_sha256"]) | set(candidate["exact_sha256"])
        )
        changed = merged != existing["exact_sha256"]
        existing["exact_sha256"] = merged
        if "note" in candidate and candidate["note"] != existing.get("note"):
            if existing.get("note"):
                raise CatalogError(
                    f"{existing['name']!r} already has a different note"
                )
            existing["note"] = candidate["note"]
            changed = True
        return validate_catalog(catalog), changed
    catalog["function"].append(candidate)
    return validate_catalog(catalog), True


def index_catalog(
    catalog: dict[str, Any],
) -> dict[tuple[str, int, int], dict[str, Any]]:
    catalog = validate_catalog(catalog)
    return {catalog_key(entry): entry for entry in catalog["function"]}


def match_catalog(
    catalog_index: dict[tuple[str, int, int], dict[str, Any]],
    fingerprint: dict[str, Any],
) -> tuple[dict[str, Any], str] | None:
    entry = catalog_index.get(catalog_key(fingerprint))
    if entry is None:
        return None
    strength = (
        "exact"
        if fingerprint["exact_sha256"] in entry["exact_sha256"]
        else "normalized"
    )
    return entry, strength


def digest_files(
    files: Iterable[tuple[str, os.PathLike[str] | str]]
) -> str:
    digest = hashlib.sha256()
    digest.update(b"psp-recomp-codegen-dependencies-v1\0")
    for label, raw_path in sorted(files):
        path = Path(raw_path)
        if not path.is_file():
            raise CacheError(f"cache dependency does not exist: {path}")
        data = path.read_bytes()
        digest.update(label.encode("utf-8") + b"\0")
        digest.update(struct.pack("<Q", len(data)))
        digest.update(data)
    return digest.hexdigest()


def canonical_json(value: Any) -> str:
    return json.dumps(
        value,
        ensure_ascii=True,
        separators=(",", ":"),
        sort_keys=True,
    )


class FunctionCodeCache:
    """Content-addressed local cache for exact generated function results."""

    def __init__(self, root: os.PathLike[str] | str):
        self.root = Path(root)
        if self.root.exists() and not self.root.is_dir():
            raise CacheError(f"codegen cache path is not a directory: {self.root}")

    @staticmethod
    def key(context: dict[str, Any]) -> str:
        digest = hashlib.sha256()
        digest.update(b"psp-recomp-function-code-v1\0")
        digest.update(canonical_json(context).encode("ascii"))
        return digest.hexdigest()

    def _path(self, key: str) -> Path:
        return self.root / f"v{CACHE_SCHEMA_VERSION}" / key[:2] / f"{key}.json"

    def load(self, context: dict[str, Any]) -> dict[str, Any] | None:
        key = self.key(context)
        path = self._path(key)
        try:
            raw = path.read_text(encoding="ascii")
        except FileNotFoundError:
            return None
        try:
            record = json.loads(raw)
        except json.JSONDecodeError as exc:
            raise CacheError(f"corrupt codegen cache entry {path}: {exc}") from exc
        if (
            not isinstance(record, dict)
            or record.get("schema_version") != CACHE_SCHEMA_VERSION
            or record.get("key") != key
            or not isinstance(record.get("payload"), dict)
        ):
            raise CacheError(f"invalid codegen cache entry metadata: {path}")
        payload = record["payload"]
        expected = hashlib.sha256(
            canonical_json(payload).encode("ascii")
        ).hexdigest()
        if record.get("payload_sha256") != expected:
            raise CacheError(f"codegen cache entry failed its checksum: {path}")
        return payload

    def store(
        self, context: dict[str, Any], payload: dict[str, Any]
    ) -> bool:
        key = self.key(context)
        existing = self.load(context)
        if existing is not None:
            if existing != payload:
                raise CacheError(
                    "codegen produced different output for an existing exact "
                    f"cache key {key}"
                )
            return False
        payload_sha256 = hashlib.sha256(
            canonical_json(payload).encode("ascii")
        ).hexdigest()
        record = {
            "schema_version": CACHE_SCHEMA_VERSION,
            "key": key,
            "payload_sha256": payload_sha256,
            "payload": payload,
        }
        path = self._path(key)
        return write_text_if_changed(
            path, canonical_json(record) + "\n", encoding="ascii"
        )
