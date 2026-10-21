#!/usr/bin/env python3

import os
import struct
import sys
import tempfile
import unittest


HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
import codegen  # noqa: E402


class FakeElf:
    def __init__(self, words):
        self.words = words

    def read_at_vaddr(self, address, size):
        word = self.words.get(address)
        return b"" if word is None else struct.pack("<I", word)


def i_type(op, source, target, immediate):
    return (
        (op << 26)
        | (source << 21)
        | (target << 16)
        | (immediate & 0xFFFF)
    )


def r_type(source, target, dest, function):
    return (source << 21) | (target << 16) | (dest << 11) | function


class DispatchGenerationTests(unittest.TestCase):
    def test_emits_sorted_immutable_table(self):
        lines = codegen.emit_dispatch_table([0x08805000, 0x08804000])
        output = "\n".join(lines)
        self.assertIn("static const SrDispatchEntry s_recomp_dispatch[]", output)
        self.assertLess(output.index("0x08804000u"), output.index("0x08805000u"))
        self.assertIn("{0x08804000u, f_08804000}", output)
        self.assertIn("sr_set_dispatch_table(", output)
        self.assertNotIn("sr_register(", output)

    def test_loads_hot_addresses_from_audit_report(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = os.path.join(temporary, "hot.txt")
            with open(path, "w", encoding="ascii") as stream:
                stream.write(
                    "# report\n"
                    "trace = calls.txt\n"
                    "0x08805000 jal sources=4\n"
                    "0x08804000 jalr sources=1\n"
                )
            self.assertEqual(
                codegen.load_hot_functions(path),
                {0x08804000, 0x08805000},
            )

    def test_rejects_malformed_hot_address(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = os.path.join(temporary, "hot.txt")
            with open(path, "w", encoding="ascii") as stream:
                stream.write("0xnot-an-address jal sources=1\n")
            with self.assertRaisesRegex(ValueError, "invalid hot function address"):
                codegen.load_hot_functions(path)

    def test_loads_host_function_override(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = os.path.join(temporary, "overrides.toml")
            with open(path, "w", encoding="ascii") as stream:
                stream.write(
                    "[[host_function]]\n"
                    "at = 0x089baac8\n"
                    'handler = "sr_kzl_downloads_unavailable"\n'
                )
            gotos, jal_next, host_functions = codegen.cx.load_overrides(path)
            self.assertEqual(gotos, {})
            self.assertEqual(jal_next, set())
            self.assertEqual(
                host_functions,
                {0x089BAAC8: "sr_kzl_downloads_unavailable"},
            )

    def test_rejects_invalid_host_function_handler(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = os.path.join(temporary, "overrides.toml")
            with open(path, "w", encoding="ascii") as stream:
                stream.write(
                    "[[host_function]]\n"
                    "at = 0x089baac8\n"
                    'handler = "not-a-c-identifier"\n'
                )
            with self.assertRaisesRegex(ValueError, "invalid C handler"):
                codegen.cx.load_overrides(path)

    def test_chunks_functions_without_splitting_them(self):
        functions = [
            ["first line", "first second"],
            ["second"],
            ["third"],
        ]
        self.assertEqual(
            codegen.chunk_functions(functions, 20),
            [[functions[0]], [functions[1], functions[2]]],
        )

    def test_caches_gprs_and_directs_constant_jalr(self):
        words = {
            0x1000: i_type(0x0F, 0, 25, 0),
            0x1004: i_type(0x0D, 25, 25, 0x2000),
            0x1008: r_type(25, 0, 31, 0x09),
            0x100C: i_type(0x09, 4, 4, 1),
            0x1010: r_type(31, 0, 0, 0x08),
            0x1014: 0,
        }
        output = "\n".join(
            codegen.emit_function(
                FakeElf(words),
                0x1000,
                [(0x1000, 0x1018)],
                {0x1000, 0x2000},
            )
        )
        self.assertIn(
            "uint32_t r4=s->r[4], r25=s->r[25], r31=s->r[31];",
            output,
        )
        self.assertIn("SR_GPR(s, 25) = (0x0u << 16);", output)
        self.assertIn("s->r[25]=r25;", output)
        self.assertIn(
            "if (_t == 0x00002000u) f_00002000(s); "
            "else dispatch(s, _t);",
            output,
        )
        self.assertIn("r25=s->r[25];", output)

    def test_does_not_fold_jalr_across_control_flow_merge(self):
        words = {
            0x1000: i_type(0x0F, 0, 25, 0),
            0x1004: i_type(0x0D, 25, 25, 0x2000),
            0x1008: r_type(25, 0, 31, 0x09),
        }
        targets = codegen.constant_jalr_targets(
            FakeElf(words),
            set(words),
            {0x1004},
            {0x2000},
        )
        self.assertEqual(targets, {})

    def test_release_trace_macros_have_no_hook_calls(self):
        self.assertIn(
            "# define SR_TRACE_BEGIN(s, pc, op) ((void)0)",
            codegen.GPR_CACHE_PREAMBLE,
        )
        self.assertIn(
            "# define SR_DIRECT_TRACE_END(s, addr, size) ((void)0)",
            codegen.GPR_CACHE_PREAMBLE,
        )
        line = codegen.normal_line(0x1000, i_type(0x09, 4, 2, 3))
        self.assertIn(
            "SR_GPR(s, 2) = (SR_GPR(s, 4) + 0x00000003u);",
            line,
        )
        self.assertNotIn("sr_begin(", line)


if __name__ == "__main__":
    unittest.main()
