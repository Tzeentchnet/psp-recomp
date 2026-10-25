#!/usr/bin/env python3

import json
import os
from pathlib import Path
import struct
import sys
import tempfile
import unittest
from unittest import mock
import contextlib
import io


HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))

import codegen  # noqa: E402
import function_catalog  # noqa: E402
from function_reuse import (  # noqa: E402
    CATALOG_SCHEMA_VERSION,
    FINGERPRINT_ALGORITHM,
    CacheError,
    CatalogError,
    FunctionCodeCache,
    add_catalog_entry,
    digest_files,
    fingerprint_instructions,
    index_catalog,
    load_catalog,
    match_catalog,
    write_catalog,
    write_text_if_changed,
)


class FakeElf:
    def __init__(self, words):
        self.words = words
        self.sections = []

    def read_at_vaddr(self, address, size):
        word = self.words.get(address)
        return b"" if word is None else struct.pack("<I", word)


def empty_catalog():
    return {
        "schema_version": CATALOG_SCHEMA_VERSION,
        "fingerprint_algorithm": FINGERPRINT_ALGORITHM,
        "function": [],
    }


def catalog_entry(fingerprint, name="memcpy"):
    return {
        "name": name,
        "library": "libc",
        "normalized_sha256": fingerprint["normalized_sha256"],
        "instruction_count": fingerprint["instruction_count"],
        "span": fingerprint["span"],
        "exact_sha256": [fingerprint["exact_sha256"]],
        "source": "pspsdk libc",
        "source_revision": "test-revision",
        "license": "BSD-2-Clause",
    }


class FingerprintTests(unittest.TestCase):
    def test_normalized_match_masks_addresses_but_exact_hash_does_not(self):
        first = fingerprint_instructions(
            0x1000,
            [
                (0x1000, (3 << 26) | (0x2000 >> 2)),
                (0x1004, 0x24840001),
            ],
        )
        second = fingerprint_instructions(
            0x3000,
            [
                (0x3000, (3 << 26) | (0x5000 >> 2)),
                (0x3004, 0x2484FFFF),
            ],
        )

        self.assertEqual(
            first["normalized_sha256"], second["normalized_sha256"]
        )
        self.assertNotEqual(first["exact_sha256"], second["exact_sha256"])
        self.assertEqual(first["instruction_count"], 2)
        self.assertEqual(first["span"], 8)

    def test_relative_layout_is_part_of_the_fingerprint(self):
        packed = fingerprint_instructions(
            0x1000, [(0x1000, 0), (0x1004, 0)]
        )
        sparse = fingerprint_instructions(
            0x1000, [(0x1000, 0), (0x1010, 0)]
        )

        self.assertNotEqual(
            packed["normalized_sha256"], sparse["normalized_sha256"]
        )


class CatalogTests(unittest.TestCase):
    def test_catalog_environment_selects_local_default(self):
        configured = str(HERE / "local-catalog.toml")
        with mock.patch.dict(
            os.environ,
            {"PSP_RECOMP_FUNCTION_CATALOG": configured},
        ):
            args = function_catalog.parse_args(["check"])

        self.assertEqual(args.catalog, Path(configured))

    def test_round_trips_provenance_without_code(self):
        fingerprint = fingerprint_instructions(0x1000, [(0x1000, 0)])
        catalog, changed = add_catalog_entry(
            empty_catalog(), catalog_entry(fingerprint)
        )
        self.assertTrue(changed)

        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "catalog.toml"
            self.assertTrue(write_catalog(path, catalog))
            self.assertFalse(write_catalog(path, catalog))
            loaded = load_catalog(path)
            text = path.read_text(encoding="ascii")

        self.assertEqual(loaded, catalog)
        self.assertNotIn("instruction_bytes", text)
        self.assertNotIn("lines =", text)

    def test_adds_exact_variant_to_same_identity(self):
        first = fingerprint_instructions(
            0x1000, [(0x1000, 0x24840001)]
        )
        second = fingerprint_instructions(
            0x2000, [(0x2000, 0x2484FFFF)]
        )
        catalog, _ = add_catalog_entry(
            empty_catalog(), catalog_entry(first)
        )
        catalog, changed = add_catalog_entry(
            catalog, catalog_entry(second)
        )

        self.assertTrue(changed)
        self.assertEqual(len(catalog["function"]), 1)
        self.assertEqual(len(catalog["function"][0]["exact_sha256"]), 2)

    def test_rejects_normalized_collision_with_different_identity(self):
        fingerprint = fingerprint_instructions(
            0x1000, [(0x1000, 0x24840001)]
        )
        catalog, _ = add_catalog_entry(
            empty_catalog(), catalog_entry(fingerprint)
        )

        with self.assertRaisesRegex(
            CatalogError, "different identity"
        ):
            add_catalog_entry(
                catalog, catalog_entry(fingerprint, name="memmove")
            )

    def test_reports_exact_and_normalized_match_strength(self):
        first = fingerprint_instructions(
            0x1000, [(0x1000, 0x24840001)]
        )
        variant = fingerprint_instructions(
            0x2000, [(0x2000, 0x2484FFFF)]
        )
        catalog, _ = add_catalog_entry(
            empty_catalog(), catalog_entry(first)
        )
        index = index_catalog(catalog)

        self.assertEqual(match_catalog(index, first)[1], "exact")
        self.assertEqual(match_catalog(index, variant)[1], "normalized")

    def test_requires_provenance(self):
        fingerprint = fingerprint_instructions(0x1000, [(0x1000, 0)])
        entry = catalog_entry(fingerprint)
        del entry["license"]

        with self.assertRaisesRegex(CatalogError, "license"):
            add_catalog_entry(empty_catalog(), entry)

    def test_rejects_unknown_fields(self):
        fingerprint = fingerprint_instructions(
            0x1000,
            [(0x1000, 0x03E00008), (0x1004, 0x00000000)],
        )
        entry = catalog_entry(fingerprint)
        entry["raw_instructions"] = ["03e00008", "00000000"]

        with self.assertRaisesRegex(CatalogError, "unknown function fields"):
            add_catalog_entry(empty_catalog(), entry)


class FunctionCodeCacheTests(unittest.TestCase):
    def setUp(self):
        codegen.JUMPTABLES.clear()
        codegen.JAL_NEXT.clear()
        codegen.HOST_FUNCTIONS.clear()
        codegen.INSTRUCTION_HOOKS.clear()

    def test_caches_exact_generated_function(self):
        words = {
            0x1000: 0x24840001,
            0x1004: 0x03E00008,
            0x1008: 0,
        }
        elf = FakeElf(words)
        ranges = [(0x1000, 0x100C)]
        known = {0x1000}

        with tempfile.TemporaryDirectory() as temporary:
            cache = FunctionCodeCache(temporary)
            first_stats = {}
            first, first_hit, first_write = codegen.emit_function_cached(
                elf,
                0x1000,
                ranges,
                known,
                cache,
                "generator-test",
                stats=first_stats,
            )
            second_stats = {}
            second, second_hit, second_write = codegen.emit_function_cached(
                elf,
                0x1000,
                ranges,
                known,
                cache,
                "generator-test",
                stats=second_stats,
            )

        self.assertFalse(first_hit)
        self.assertTrue(first_write)
        self.assertTrue(second_hit)
        self.assertFalse(second_write)
        self.assertEqual(first, second)
        self.assertEqual(first_stats, second_stats)

    def test_cache_key_includes_generation_options_and_exact_words(self):
        first_elf = FakeElf(
            {0x1000: 0x24840001, 0x1004: 0x03E00008, 0x1008: 0}
        )
        second_elf = FakeElf(
            {0x1000: 0x24840002, 0x1004: 0x03E00008, 0x1008: 0}
        )
        ranges = [(0x1000, 0x100C)]
        known = {0x1000}

        with tempfile.TemporaryDirectory() as temporary:
            cache = FunctionCodeCache(temporary)
            codegen.emit_function_cached(
                first_elf,
                0x1000,
                ranges,
                known,
                cache,
                "generator-test",
            )
            _, changed_word_hit, _ = codegen.emit_function_cached(
                second_elf,
                0x1000,
                ranges,
                known,
                cache,
                "generator-test",
            )
            _, changed_prefix_hit, _ = codegen.emit_function_cached(
                first_elf,
                0x1000,
                ranges,
                known,
                cache,
                "generator-test",
                symbol_prefix="other_module",
            )

        self.assertFalse(changed_word_hit)
        self.assertFalse(changed_prefix_hit)

    def test_codegen_cli_uses_cache_and_preserves_unchanged_output(self):
        elf = FakeElf(
            {0x1000: 0x24840001, 0x1004: 0x03E00008, 0x1008: 0}
        )
        elf.reloc = None
        ranges = [(0x1000, 0x100C)]
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary) / "generated.c"
            cache = Path(temporary) / "cache"
            arguments = [
                "codegen.py",
                "synthetic.elf",
                str(output),
                f"--cache-dir={cache}",
            ]
            patches = (
                mock.patch.object(codegen, "Elf", return_value=elf),
                mock.patch.object(
                    codegen, "analyze", return_value=({0x1000}, ranges)
                ),
                mock.patch.object(
                    codegen, "exec_ranges", return_value=ranges
                ),
                mock.patch.object(
                    codegen, "stub_ranges", return_value=[]
                ),
            )
            with patches[0], patches[1], patches[2], patches[3]:
                stdout = io.StringIO()
                with contextlib.redirect_stdout(stdout):
                    self.assertEqual(codegen.main(arguments), 0)
                before = output.stat().st_mtime_ns
                with contextlib.redirect_stdout(stdout):
                    self.assertEqual(codegen.main(arguments), 0)
                after = output.stat().st_mtime_ns

        self.assertEqual(before, after)
        self.assertIn("0 hits, 1 misses, 1 writes", stdout.getvalue())
        self.assertIn("1 hits, 0 misses, 0 writes", stdout.getvalue())

    def test_corrupt_cache_entry_fails_explicitly(self):
        context = {"input": "exact"}
        with tempfile.TemporaryDirectory() as temporary:
            cache = FunctionCodeCache(temporary)
            cache.store(context, {"lines": ["valid"]})
            path = cache._path(cache.key(context))
            record = json.loads(path.read_text(encoding="ascii"))
            record["payload"]["lines"] = ["tampered"]
            path.write_text(json.dumps(record), encoding="ascii")

            with self.assertRaisesRegex(CacheError, "checksum"):
                cache.load(context)

    def test_dependency_digest_changes_with_file_content(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "abi.h"
            path.write_text("one", encoding="ascii")
            first = digest_files([("abi.h", path)])
            path.write_text("two", encoding="ascii")
            second = digest_files([("abi.h", path)])

        self.assertNotEqual(first, second)

    def test_write_if_changed_preserves_existing_file(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "generated.c"
            self.assertTrue(write_text_if_changed(path, "same\n"))
            before = os.stat(path).st_mtime_ns
            self.assertFalse(write_text_if_changed(path, "same\n"))
            after = os.stat(path).st_mtime_ns

        self.assertEqual(before, after)


if __name__ == "__main__":
    unittest.main()
