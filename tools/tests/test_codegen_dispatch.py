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

    def test_emits_runtime_import_patches(self):
        lines = codegen.emit_dispatch_table(
            [0x08804000],
            {0x08926760: ("kjfs", 0x93E9297F)},
        )
        output = "\n".join(lines)
        self.assertIn(
            'sr_module_patch_import(0x08926760u, 0x93e9297fu, "kjfs");',
            output,
        )

    def test_prefixes_module_symbols_and_registration(self):
        output = "\n".join(
            codegen.emit_dispatch_table(
                [0x09AF5800, 0x09AF6000],
                symbol_prefix="mga_kjfs",
            )
        )

        self.assertIn(
            "{0x09af5800u, mga_kjfs_f_09af5800}", output
        )
        self.assertIn(
            "const SrDispatchTable mga_kjfs_dispatch_table", output
        )
        self.assertIn("int mga_kjfs_register(void)", output)
        self.assertIn(
            "sr_activate_dispatch_table(&mga_kjfs_dispatch_table)",
            output,
        )
        self.assertNotIn("sr_register_all", output)

    def test_emits_vfpu7_integer_pack_operations(self):
        unsigned_char, _, _ = codegen.vfpu_effect(0x08885E54, 0xD03C8484)
        signed_short, _, _ = codegen.vfpu_effect(0x08886D6C, 0xD03F88CC)
        self.assertIn("_v>>23", unsigned_char)
        self.assertIn(">>16", signed_short)
        self.assertIn("sr_vwrite", unsigned_char)
        self.assertIn("sr_vwrite", signed_short)

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
            gotos, jal_next, host_functions, instruction_hooks = (
                codegen.cx.load_overrides(path)
            )
            self.assertEqual(gotos, {})
            self.assertEqual(jal_next, set())
            self.assertEqual(instruction_hooks, {})
            self.assertEqual(
                host_functions,
                {0x089BAAC8: "sr_kzl_downloads_unavailable"},
            )

    def test_loads_instruction_hook_override(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = os.path.join(temporary, "overrides.toml")
            with open(path, "w", encoding="ascii") as stream:
                stream.write(
                    "[[instruction_hook]]\n"
                    "at = 0x08900688\n"
                    'handler = "sr_kzl_twinstick_apply_aim"\n'
                )
            gotos, jal_next, host_functions, instruction_hooks = (
                codegen.cx.load_overrides(path)
            )
            self.assertEqual(gotos, {})
            self.assertEqual(jal_next, set())
            self.assertEqual(host_functions, {})
            self.assertEqual(
                instruction_hooks,
                {0x08900688: "sr_kzl_twinstick_apply_aim"},
            )

    def test_declares_each_instruction_hook_handler_once(self):
        self.assertEqual(
            codegen.instruction_hook_prototypes(
                {
                    0x08900688: "sr_target_hook",
                    0x089006A0: "sr_target_hook",
                    0x089006B8: "sr_other_hook",
                }
            ),
            [
                "void sr_other_hook(CpuState *s);",
                "void sr_target_hook(CpuState *s);",
            ],
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

    def test_prefixes_direct_function_calls(self):
        words = {
            0x1000: (0x03 << 26) | (0x2000 >> 2),
            0x1004: 0,
            0x1008: r_type(31, 0, 0, 0x08),
            0x100C: 0,
            0x2000: r_type(31, 0, 0, 0x08),
            0x2004: 0,
        }
        output = "\n".join(
            codegen.emit_function(
                FakeElf(words),
                0x1000,
                [(0x1000, 0x100C), (0x2000, 0x2008)],
                {0x1000, 0x2000},
                symbol_prefix="mga_test",
            )
        )

        self.assertIn("void mga_test_f_00001000", output)
        self.assertIn("mga_test_f_00002000(s);", output)
        self.assertNotIn(" f_00002000(s);", output)

    def test_runtime_jal_reads_patched_guest_instruction(self):
        words = {
            0x1000: (0x03 << 26) | (0x2000 >> 2),
            0x1004: 0,
            0x1008: r_type(31, 0, 0, 0x08),
            0x100C: 0,
            0x2000: r_type(31, 0, 0, 0x08),
            0x2004: 0,
        }
        output = "\n".join(
            codegen.emit_function(
                FakeElf(words),
                0x1000,
                [(0x1000, 0x100C), (0x2000, 0x2008)],
                {0x1000, 0x2000},
                symbol_prefix="mga_test",
                runtime_jal=True,
                verify_runtime_code=True,
            )
        )

        self.assertIn(
            "uint32_t _jw = MEM_R32(0x00001000u)", output
        )
        self.assertIn(
            "(MEM_R32(0x00001000u) >> 26) != 0x03u", output
        )
        self.assertNotIn(
            "MEM_R32(0x00001000u) != 0x0c000800u", output
        )
        self.assertIn("dispatch(s, _t)", output)
        self.assertNotIn("mga_test_f_00002000(s);", output)

    def test_runtime_code_verification_falls_back_explicitly(self):
        words = {
            0x1000: i_type(0x09, 4, 4, 1),
            0x1004: r_type(31, 0, 0, 0x08),
            0x1008: 0,
        }
        output = "\n".join(
            codegen.emit_function(
                FakeElf(words),
                0x1000,
                [(0x1000, 0x100C)],
                {0x1000},
                symbol_prefix="mga_test",
                verify_runtime_code=True,
            )
        )

        self.assertIn("int _code_matches = 1", output)
        self.assertNotIn("static int _code_matches", output)
        self.assertNotIn("if (_code_matches < 0)", output)
        self.assertIn(
            "MEM_R32(0x00001000u) != 0x24840001u", output
        )
        self.assertIn(
            'SR_INTERPRET_TAIL(s, 0x00001000u, "mga_test")',
            output,
        )
        self.assertIn(
            "sr_disable_native_function(mga_test_f_00001000)",
            output,
        )

    def test_instruction_hook_synchronizes_cached_gprs(self):
        words = {
            0x1000: i_type(0x09, 4, 4, 1),
            0x1004: r_type(31, 0, 0, 0x08),
            0x1008: 0,
        }
        used = set()
        output = "\n".join(
            codegen.emit_function(
                FakeElf(words),
                0x1000,
                [(0x1000, 0x100C)],
                {0x1000},
                instruction_hooks={0x1000: "host_hook"},
                used_instruction_hooks=used,
            )
        )
        self.assertEqual(used, {0x1000})
        flush = output.index("s->r[4]=r4;")
        hook = output.index("host_hook(s);")
        reload = output.index("r4=s->r[4];", hook)
        instruction = output.index(
            "SR_GPR(s, 4) = (SR_GPR(s, 4) + 0x00000001u);"
        )
        self.assertLess(flush, hook)
        self.assertLess(hook, reload)
        self.assertLess(reload, instruction)

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
