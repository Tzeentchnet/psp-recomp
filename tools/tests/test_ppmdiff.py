import importlib.util
import json
import os
import tempfile
import unittest


TOOLS_DIR = os.path.normpath(os.path.join(os.path.dirname(__file__), ".."))
MODULE_PATH = os.path.join(TOOLS_DIR, "ppmdiff.py")
SPEC = importlib.util.spec_from_file_location("ppmdiff", MODULE_PATH)
ppmdiff = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(ppmdiff)


def write_ppm(path, width, height, pixels):
    with open(path, "wb") as stream:
        stream.write(f"P6\n{width} {height}\n255\n".encode("ascii"))
        stream.write(bytes(pixels))


class PpmDiffTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.reference = os.path.join(self.temp.name, "reference")
        self.actual = os.path.join(self.temp.name, "actual")
        os.mkdir(self.reference)
        os.mkdir(self.actual)

    def tearDown(self):
        self.temp.cleanup()

    def test_threshold_gate_and_json_shape(self):
        write_ppm(os.path.join(self.reference, "frame.ppm"), 2, 1, [0, 0, 0, 50, 60, 70])
        write_ppm(os.path.join(self.actual, "frame.ppm"), 2, 1, [8, 0, 0, 59, 60, 70])

        result = ppmdiff.compare_directories(
            self.reference, self.actual, threshold=8, max_threshold_pixels=0
        )

        self.assertFalse(result["passed"])
        self.assertEqual(result["compared_frames"], 1)
        self.assertEqual(result["frames"][0]["different_pixels"], 2)
        self.assertEqual(result["frames"][0]["above_threshold_pixels"], 1)
        self.assertEqual(result["frames"][0]["max_channel_delta"], 9)
        json.dumps(result)

    def test_allows_deltas_at_threshold(self):
        write_ppm(os.path.join(self.reference, "frame.ppm"), 1, 1, [1, 2, 3])
        write_ppm(os.path.join(self.actual, "frame.ppm"), 1, 1, [9, 2, 3])

        result = ppmdiff.compare_directories(
            self.reference, self.actual, threshold=8, max_threshold_pixels=0
        )

        self.assertTrue(result["passed"])
        self.assertEqual(result["frames"][0]["above_threshold_pixels"], 0)

    def test_reports_missing_and_unexpected_manifest_frames(self):
        write_ppm(os.path.join(self.reference, "wanted.ppm"), 1, 1, [0, 0, 0])
        write_ppm(os.path.join(self.actual, "extra.ppm"), 1, 1, [0, 0, 0])

        result = ppmdiff.compare_directories(
            self.reference, self.actual, names=["wanted.ppm"]
        )

        self.assertFalse(result["passed"])
        self.assertIn("actual missing: wanted.ppm", result["errors"])
        self.assertIn("actual unexpected: extra.ppm", result["errors"])

    def test_rejects_truncated_raster(self):
        path = os.path.join(self.reference, "bad.ppm")
        write_ppm(path, 2, 1, [0, 0, 0])

        with self.assertRaises(ppmdiff.PpmError):
            ppmdiff.read_ppm(path)

    def test_manifest_requires_header_order_and_exact_count(self):
        path = os.path.join(self.temp.name, "frames.txt")
        with open(path, "w", encoding="ascii", newline="\n") as stream:
            stream.write(f"{ppmdiff.MANIFEST_HEADER}\n")
            for index in range(ppmdiff.FRAME_COUNT):
                stream.write(
                    f"{index + 1} {index + 101} frame_{index:02d}.ppm boot\n"
                )

        names = ppmdiff.read_manifest(path)

        self.assertEqual(len(names), ppmdiff.FRAME_COUNT)
        self.assertEqual(names[0], "frame_00.ppm")
        with open(path, "a", encoding="ascii", newline="\n") as stream:
            stream.write("51 151 extra.ppm boot\n")
        with self.assertRaisesRegex(ValueError, "expected 50 frames"):
            ppmdiff.read_manifest(path)


if __name__ == "__main__":
    unittest.main()
