#!/usr/bin/env python3

import os
import sys
import tempfile
import unittest


HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
import codegen  # noqa: E402


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


if __name__ == "__main__":
    unittest.main()
