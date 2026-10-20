#!/usr/bin/env python3

import os
import sys
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


if __name__ == "__main__":
    unittest.main()
