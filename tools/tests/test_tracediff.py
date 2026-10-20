import importlib.util
import os
import tempfile
import unittest


ROOT = os.path.normpath(
    os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..")
)
MODULE_PATH = os.path.join(ROOT, "tools", "tracediff.py")
SPEC = importlib.util.spec_from_file_location("tracediff", MODULE_PATH)
tracediff = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(tracediff)


class TraceDiffTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()

    def tearDown(self):
        self.temporary.cleanup()

    def write_trace(self, name, lines):
        path = os.path.join(self.temporary.name, name)
        with open(path, "w", encoding="ascii", newline="\n") as stream:
            stream.write("# psp-recomp trace v1\n")
            stream.write("\n".join(lines) + "\n")
        return path

    def test_identical_streams_match(self):
        lines = [
            "0 pc=0x08804000 op=0x24020001 r2=0x00000001",
            "1 pc=0x08804004 op=0x03e00008",
        ]
        first = self.write_trace("first.txt", lines)
        second = self.write_trace("second.txt", lines)
        self.assertIsNone(tracediff.diff(first, second))

    def test_kernel_trampoline_range_can_be_filtered(self):
        oracle = self.write_trace(
            "oracle.txt",
            [
                "0 pc=0x08804000 op=0x24020001 r2=0x00000001",
                "1 pc=0x08000010 op=0x00000000 r4=0xdeadbeef",
                "2 pc=0x08804004 op=0x03e00008",
            ],
        )
        runtime = self.write_trace(
            "runtime.txt",
            [
                "0 pc=0x08804000 op=0x24020001 r2=0x00000001",
                "1 pc=0x08804004 op=0x03e00008",
            ],
        )
        self.assertIsNotNone(tracediff.diff(oracle, runtime))
        self.assertIsNone(
            tracediff.diff(oracle, runtime, [(0x08000000, 0x08800000)])
        )

    def test_reports_stream_length_difference(self):
        longer = self.write_trace(
            "longer.txt",
            [
                "0 pc=0x08804000 op=0x00000000",
                "1 pc=0x08804004 op=0x00000000",
            ],
        )
        shorter = self.write_trace(
            "shorter.txt", ["0 pc=0x08804000 op=0x00000000"]
        )
        result = tracediff.diff(longer, shorter)
        self.assertEqual(result[0], 1)
        self.assertIn("shorter.txt ends first", result[2])

    def test_syscall_writes_can_be_ignored(self):
        oracle = self.write_trace(
            "oracle.txt",
            [
                "0 pc=0x08d8f124 op=0x0004090c "
                "r2=0x0000011b r29=0x09fff4f0"
            ],
        )
        runtime = self.write_trace(
            "runtime.txt",
            [
                "0 pc=0x08d8f124 op=0x00000000 "
                "r4=0xdeadbeef"
            ],
        )
        self.assertIsNotNone(tracediff.diff(oracle, runtime))
        self.assertIsNone(
            tracediff.diff(oracle, runtime, (), ignore_syscall_writes=True)
        )

    def test_rejects_malformed_pc(self):
        malformed = self.write_trace(
            "malformed.txt", ["0 pc=garbage op=0x00000000"]
        )
        with self.assertRaisesRegex(tracediff.TraceError, "pc/op must be"):
            list(tracediff.iter_steps(malformed))


if __name__ == "__main__":
    unittest.main()
