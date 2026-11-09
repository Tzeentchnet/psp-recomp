import pathlib
import sys
import tempfile
import unittest


sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
import analyze_ip_samples


class AnalyzeInstructionPointerSamplesTest(unittest.TestCase):
    def test_resolves_symbols_and_external_samples(self):
        base, symbols = analyze_ip_samples.parse_symbols(
            "0000000100000000 T __mh_execute_header\n"
            "0000000100000010 T _f_08810000\n"
            "0000000100000040 T _sr_vread\n",
            None,
        )
        self.assertEqual(base, 0x100000000)
        resolved, external, unresolved = (
            analyze_ip_samples.resolve_samples(
                [(0x10, 3), (0x44, 2), (0xFFFFFFFF, 5)],
                symbols,
            )
        )
        self.assertEqual(resolved["_f_08810000"], 3)
        self.assertEqual(resolved["_sr_vread"], 2)
        self.assertEqual(external, 5)
        self.assertEqual(unresolved, 0)

    def test_filters_category_and_modules(self):
        with tempfile.TemporaryDirectory() as root:
            samples = pathlib.Path(root) / "samples.txt"
            samples.write_text(
                "# psp-recomp instruction-pointer samples v1\n"
                "00000010 3\nffffffff 2\n",
                encoding="utf-8",
            )
            samples.with_name(
                samples.name + ".categories.txt"
            ).write_text(
                "# psp-recomp categorized instruction-pointer samples v1\n"
                "00000010\t0\t3\nffffffff\t6\t2\n",
                encoding="utf-8",
            )
            samples.with_name(
                samples.name + ".modules.txt"
            ).write_text(
                "# psp-recomp external instruction-pointer samples v2\n"
                "6\tlibgpu.dylib\t20\n"
                "0\tlibsystem.dylib\t10\n",
                encoding="utf-8",
            )
            self.assertEqual(
                analyze_ip_samples.read_samples(samples, 0),
                [(0x10, 3)],
            )
            self.assertEqual(
                analyze_ip_samples.read_external_modules(samples, 0),
                {"libsystem.dylib": 1},
            )
            self.assertEqual(
                analyze_ip_samples.category_id("native"),
                0,
            )


if __name__ == "__main__":
    unittest.main()
