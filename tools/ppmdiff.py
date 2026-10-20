"""Compare two directories of binary PPM frames and fail on regressions."""

import argparse
import json
import os
import sys


FRAME_COUNT = 50
MANIFEST_HEADER = "# psp-recomp golden frames v2"


class PpmError(ValueError):
    pass


def _token(data, offset):
    size = len(data)
    while offset < size:
        if data[offset] in b" \t\r\n":
            offset += 1
            continue
        if data[offset] == ord("#"):
            newline = data.find(b"\n", offset)
            if newline < 0:
                raise PpmError("unterminated header comment")
            offset = newline + 1
            continue
        break
    start = offset
    while offset < size and data[offset] not in b" \t\r\n#":
        offset += 1
    if start == offset:
        raise PpmError("missing header token")
    return data[start:offset], offset


def read_ppm(path):
    with open(path, "rb") as stream:
        data = stream.read()
    try:
        magic, offset = _token(data, 0)
        width_text, offset = _token(data, offset)
        height_text, offset = _token(data, offset)
        maximum_text, offset = _token(data, offset)
        width = int(width_text)
        height = int(height_text)
        maximum = int(maximum_text)
    except (PpmError, ValueError) as exc:
        raise PpmError(f"invalid PPM header: {exc}") from exc
    if magic != b"P6":
        raise PpmError("expected binary P6 data")
    if width <= 0 or height <= 0:
        raise PpmError("dimensions must be positive")
    if maximum != 255:
        raise PpmError("maximum channel value must be 255")
    if offset >= len(data) or data[offset] not in b" \t\r\n":
        raise PpmError("missing raster separator")
    if data[offset:offset + 2] == b"\r\n":
        offset += 2
    else:
        offset += 1
    expected_size = width * height * 3
    raster = data[offset:]
    if len(raster) != expected_size:
        raise PpmError(
            f"raster has {len(raster)} bytes, expected {expected_size}"
        )
    return raster, width, height


def read_manifest(path):
    names = []
    seen_names = set()
    previous_ppsspp_present = 0
    previous_runtime_present = 0
    seen_header = False
    with open(path, "r", encoding="ascii") as stream:
        for line_number, raw_line in enumerate(stream, 1):
            line = raw_line.strip()
            if not line:
                continue
            if line.startswith("#"):
                if line == MANIFEST_HEADER:
                    seen_header = True
                continue
            fields = line.split()
            if len(fields) != 4:
                raise ValueError(
                    f"{path}:{line_number}: expected PPSSPP/runtime presents, name, and scene"
                )
            ppsspp_present_text, runtime_present_text, name, _scene = fields
            try:
                ppsspp_present = int(ppsspp_present_text, 10)
                runtime_present = int(runtime_present_text, 10)
            except ValueError as exc:
                raise ValueError(f"{path}:{line_number}: invalid presented-frame index") from exc
            if (
                ppsspp_present <= previous_ppsspp_present
                or runtime_present <= previous_runtime_present
            ):
                raise ValueError(
                    f"{path}:{line_number}: both presented-frame indices must increase"
                )
            if (
                not name.endswith(".ppm")
                or "/" in name
                or "\\" in name
                or name in seen_names
            ):
                raise ValueError(f"{path}:{line_number}: invalid or duplicate PPM name")
            previous_ppsspp_present = ppsspp_present
            previous_runtime_present = runtime_present
            seen_names.add(name)
            names.append(name)
    if not seen_header:
        raise ValueError(f"{path}: missing '{MANIFEST_HEADER}' header")
    if len(names) != FRAME_COUNT:
        raise ValueError(f"{path}: expected {FRAME_COUNT} frames, found {len(names)}")
    return names


def _ppm_names(directory):
    return {
        name
        for name in os.listdir(directory)
        if name.lower().endswith(".ppm") and os.path.isfile(os.path.join(directory, name))
    }


def compare_directories(
    reference_dir,
    actual_dir,
    names=None,
    threshold=8,
    max_different_pixels=None,
    max_threshold_pixels=0,
    max_channel_delta=255,
):
    reference_names = _ppm_names(reference_dir)
    actual_names = _ppm_names(actual_dir)
    expected_names = set(names) if names is not None else reference_names | actual_names
    ordered_names = list(names) if names is not None else sorted(expected_names)
    errors = []
    frames = []

    if not expected_names:
        errors.append("no PPM frames found")
    for label, found in (("reference", reference_names), ("actual", actual_names)):
        missing = sorted(expected_names - found)
        unexpected = sorted(found - expected_names)
        if missing:
            errors.append(f"{label} missing: {', '.join(missing)}")
        if unexpected:
            errors.append(f"{label} unexpected: {', '.join(unexpected)}")

    for name in ordered_names:
        if name not in reference_names or name not in actual_names:
            continue
        reference_path = os.path.join(reference_dir, name)
        actual_path = os.path.join(actual_dir, name)
        try:
            reference, width, height = read_ppm(reference_path)
            actual, actual_width, actual_height = read_ppm(actual_path)
        except (OSError, PpmError) as exc:
            errors.append(f"{name}: {exc}")
            continue
        if (width, height) != (actual_width, actual_height):
            errors.append(
                f"{name}: dimension mismatch "
                f"{width}x{height} vs {actual_width}x{actual_height}"
            )
            continue

        different = 0
        above_threshold = 0
        maximum_delta = 0
        for offset in range(0, len(reference), 3):
            delta = max(
                abs(reference[offset] - actual[offset]),
                abs(reference[offset + 1] - actual[offset + 1]),
                abs(reference[offset + 2] - actual[offset + 2]),
            )
            if delta:
                different += 1
                if delta > threshold:
                    above_threshold += 1
                maximum_delta = max(maximum_delta, delta)

        pixels = width * height
        violations = []
        if max_different_pixels is not None and different > max_different_pixels:
            violations.append(
                f"different pixels {different} > {max_different_pixels}"
            )
        if above_threshold > max_threshold_pixels:
            violations.append(
                f"pixels above delta {threshold}: "
                f"{above_threshold} > {max_threshold_pixels}"
            )
        if maximum_delta > max_channel_delta:
            violations.append(
                f"maximum channel delta {maximum_delta} > {max_channel_delta}"
            )
        if violations:
            errors.append(f"{name}: {'; '.join(violations)}")
        frames.append(
            {
                "name": name,
                "width": width,
                "height": height,
                "pixels": pixels,
                "different_pixels": different,
                "different_percent": 100.0 * different / pixels,
                "threshold": threshold,
                "above_threshold_pixels": above_threshold,
                "above_threshold_percent": 100.0 * above_threshold / pixels,
                "max_channel_delta": maximum_delta,
                "passed": not violations,
            }
        )

    return {
        "passed": not errors,
        "reference_dir": os.path.abspath(reference_dir),
        "actual_dir": os.path.abspath(actual_dir),
        "expected_frames": len(expected_names),
        "compared_frames": len(frames),
        "errors": errors,
        "frames": frames,
    }


def _nonnegative(value):
    parsed = int(value)
    if parsed < 0:
        raise argparse.ArgumentTypeError("must be nonnegative")
    return parsed


def parse_args(argv):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("reference_dir")
    parser.add_argument("actual_dir")
    parser.add_argument("--manifest", help="ordered 50-frame capture manifest")
    parser.add_argument(
        "--threshold",
        type=_nonnegative,
        default=8,
        help="channel delta above which a pixel is counted (default: 8)",
    )
    parser.add_argument(
        "--max-different-pixels",
        type=_nonnegative,
        help="maximum changed pixels allowed per frame (default: unlimited)",
    )
    parser.add_argument(
        "--max-threshold-pixels",
        type=_nonnegative,
        default=0,
        help="maximum pixels above --threshold allowed per frame (default: 0)",
    )
    parser.add_argument(
        "--max-channel-delta",
        type=_nonnegative,
        default=255,
        help="maximum channel delta allowed per frame (default: 255)",
    )
    parser.add_argument("--json", dest="json_path", help="write a machine-readable report")
    args = parser.parse_args(argv)
    if args.threshold > 255 or args.max_channel_delta > 255:
        parser.error("channel deltas must be at most 255")
    return args


def main(argv=None):
    args = parse_args(argv)
    try:
        names = read_manifest(args.manifest) if args.manifest else None
        result = compare_directories(
            args.reference_dir,
            args.actual_dir,
            names=names,
            threshold=args.threshold,
            max_different_pixels=args.max_different_pixels,
            max_threshold_pixels=args.max_threshold_pixels,
            max_channel_delta=args.max_channel_delta,
        )
        if args.json_path:
            with open(args.json_path, "w", encoding="utf-8", newline="\n") as stream:
                json.dump(result, stream, indent=2)
                stream.write("\n")
    except (OSError, ValueError) as exc:
        print(f"ppmdiff: {exc}", file=sys.stderr)
        return 2

    for frame in result["frames"]:
        print(
            f"{frame['name']}: {frame['pixels']}px "
            f"diff={frame['different_pixels']} "
            f"({frame['different_percent']:.2f}%) "
            f"big(>{frame['threshold']})={frame['above_threshold_pixels']} "
            f"({frame['above_threshold_percent']:.2f}%) "
            f"maxdelta={frame['max_channel_delta']} "
            f"{'PASS' if frame['passed'] else 'FAIL'}"
        )
    for error in result["errors"]:
        print(f"ERROR: {error}", file=sys.stderr)
    print(
        f"{'PASS' if result['passed'] else 'FAIL'}: "
        f"{result['compared_frames']}/{result['expected_frames']} frames compared"
    )
    return 0 if result["passed"] else 1


if __name__ == "__main__":
    sys.exit(main())
