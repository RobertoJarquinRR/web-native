#!/usr/bin/env python3
"""Compare a D3D11 window capture with a wgpu-native offscreen reference.

Both renderers consume the same wn_display_list, so the screenshots must be
close modulo rasterizer/antialiasing differences. The check also rejects a
blank capture (a clear-only frame would compare equal to a blank reference).

Usage:
    python verify_d3d11.py --capture out/d3d11.png --reference out/wgpu.png \
        --report out/d3d11_compare.json [--max-diff 8] [--max-bad-fraction 0.005]
"""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

from PIL import Image, ImageChops, ImageStat


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--capture", required=True)
    parser.add_argument("--reference", required=True)
    parser.add_argument("--report", required=True)
    parser.add_argument("--label", default="d3d11-vs-wgpu")
    parser.add_argument("--max-diff", type=int, default=8,
                        help="largest per-channel difference allowed (0..255)")
    parser.add_argument("--max-bad-fraction", type=float, default=0.005,
                        help="fraction of pixels allowed to exceed --max-diff")
    parser.add_argument("--min-content-fraction", type=float, default=0.02,
                        help="fraction of pixels that must differ from the clear color")
    args = parser.parse_args()

    capture_path = Path(args.capture)
    reference_path = Path(args.reference)
    report_path = Path(args.report)

    failures: list[str] = []
    checks: dict[str, object] = {}

    if not capture_path.is_file():
        failures.append(f"capture missing: {capture_path}")
    if not reference_path.is_file():
        failures.append(f"reference missing: {reference_path}")
    if failures:
        result = _write(report_path, args, checks, failures)
        print(f"{args.label}: FAIL")
        for item in failures:
            print(f"  - {item}")
        return 1

    capture = Image.open(capture_path).convert("RGBA")
    reference = Image.open(reference_path).convert("RGBA")
    checks["capture_size"] = list(capture.size)
    checks["reference_size"] = list(reference.size)
    if capture.size != reference.size:
        failures.append(f"size mismatch: {capture.size} vs {reference.size}")
        result = _write(report_path, args, checks, failures)
        print(f"{args.label}: FAIL")
        for item in failures:
            print(f"  - {item}")
        return result

    diff = ImageChops.difference(capture, reference)
    lum = diff.convert("L")
    histogram = lum.histogram()
    total = capture.size[0] * capture.size[1]
    stats = ImageStat.Stat(diff)
    channel_max = max(histogram_index for histogram_index, count in enumerate(histogram)
                      if count > 0)
    exact = histogram[0]
    within_1 = sum(histogram[:2])
    within_tolerance = sum(histogram[: args.max_diff + 1])
    bad = total - within_tolerance
    bad_fraction = bad / total

    # Content check: count pixels that differ from the capture's clear color.
    clear = capture.getpixel((0, 0))
    blank = Image.new("RGBA", capture.size, clear)
    content_diff = ImageChops.difference(capture, blank).convert("L")
    content_hist = content_diff.histogram()
    content = total - content_hist[0] - content_hist[1] - content_hist[2]
    content_fraction = content / total

    checks.update({
        "mean_abs_rgba": [round(value, 4) for value in stats.mean],
        "rms_rgba": [round(value, 4) for value in stats.rms],
        "max_channel_diff": int(channel_max),
        "exact_fraction": round(exact / total, 6),
        "within_1_fraction": round(within_1 / total, 6),
        "within_tolerance_fraction": round(within_tolerance / total, 6),
        "bad_fraction": round(bad_fraction, 6),
        "content_fraction": round(content_fraction, 6),
        "clear_rgba": list(clear),
    })

    if channel_max > args.max_diff:
        failures.append(
            f"max channel diff {channel_max} exceeds {args.max_diff}")
    if bad_fraction > args.max_bad_fraction:
        failures.append(
            f"{bad_fraction:.4%} of pixels exceed max diff {args.max_diff} "
            f"(limit {args.max_bad_fraction:.4%})")
    if content_fraction < args.min_content_fraction:
        failures.append(
            f"capture looks blank: only {content_fraction:.4%} of pixels differ "
            f"from the clear color (min {args.min_content_fraction:.4%})")

    _write(report_path, args, checks, failures)
    print(f"{args.label}: {'PASS' if not failures else 'FAIL'} "
          f"(max={channel_max}, bad={bad_fraction:.4%}, exact={exact / total:.4%}, "
          f"content={content_fraction:.2%})")
    for item in failures:
        print(f"  - {item}")
    return 0 if not failures else 1


def _write(path: Path, args, checks: dict, failures: list[str]) -> int:
    path.parent.mkdir(parents=True, exist_ok=True)
    payload = {
        "label": args.label,
        "capture": str(args.capture),
        "reference": str(args.reference),
        "result": "PASS" if not failures else "FAIL",
        "checks": checks,
        "failures": failures,
        "limits": {
            "max_diff": args.max_diff,
            "max_bad_fraction": args.max_bad_fraction,
            "min_content_fraction": args.min_content_fraction,
        },
    }
    path.write_text(json.dumps(payload, indent=2), encoding="utf-8")
    return 0


if __name__ == "__main__":
    sys.exit(main())
