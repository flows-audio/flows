#!/usr/bin/env python3
"""Render every test case with the native and WebAssembly `flows` CLIs and compare.

Both builds compile the same source with the same compiler and DSP engine, so
output must agree to within rounding: 2 PCM16 steps for audio and a small
relative tolerance for captured float signals (CPUs may fuse multiply-adds
differently; WebAssembly never does).

Usage: render_parity.py --native build/flows --wasm build-web/flows.js [--node node]
"""
from __future__ import annotations

import argparse
import csv
import math
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(Path(__file__).resolve().parent))
from run_case import load_json, sample_index  # noqa: E402

AUDIO_STEPS = 2
CAPTURE_RELATIVE = 1e-4
CAPTURE_ABSOLUTE = 1e-5


def read_pcm16(path: Path) -> list[int]:
    data = path.read_bytes()
    offset = 12
    while offset + 8 <= len(data):
        chunk, size = data[offset:offset + 4], struct.unpack("<I", data[offset + 4:offset + 8])[0]
        if chunk == b"data":
            body = data[offset + 8:offset + 8 + size]
            return list(struct.unpack(f"<{len(body) // 2}h", body))
        offset += 8 + size + (size & 1)
    raise ValueError(f"{path} has no data chunk")


def read_captures(path: Path) -> list[list[str]]:
    with path.open(newline="") as file:
        return list(csv.reader(file))


def render(command: list[str], case: dict, case_path: Path, output: Path) -> None:
    output.mkdir(parents=True)
    rate = case["render"]["sample_rate"]
    note_ids: dict[str, int] = {}
    lines = []
    for event in case["events"]:
        identity = note_ids.setdefault(event["note_id"], len(note_ids) + 1)
        lines.append(f"{sample_index(event['time_seconds'], rate)} {event['type']} {identity} {event['pitch']} {event.get('velocity', 0.0):.17g}\n")
    events = output / "events.tsv"
    events.write_text("".join(lines))
    source = (case_path.parent / case["preset"]).resolve()
    arguments = ["render", str(source), "--events", str(events), "--output", str(output), "--sample-rate", str(rate),
                 "--block-size", str(case["render"]["block_size"]), "--duration", str(case["render"]["duration_seconds"]),
                 "--observe", ",".join(case["observations"])]
    result = subprocess.run(command + arguments, capture_output=True, text=True, timeout=300)
    if result.returncode != 0:
        raise RuntimeError(f"{' '.join(command)} failed: {result.stderr.strip()}")


def compare(case_id: str, native: Path, wasm: Path) -> str:
    a, b = read_pcm16(native / "output.wav"), read_pcm16(wasm / "output.wav")
    if len(a) != len(b):
        raise AssertionError(f"{case_id}: {len(a)} native frames, {len(b)} WebAssembly frames")
    audio = max((abs(x - y) for x, y in zip(a, b)), default=0)
    if audio > AUDIO_STEPS:
        raise AssertionError(f"{case_id}: audio differs by {audio} PCM16 steps")
    rows_a, rows_b = read_captures(native / "captures.csv"), read_captures(wasm / "captures.csv")
    if len(rows_a) != len(rows_b) or (rows_a and rows_a[0] != rows_b[0]):
        raise AssertionError(f"{case_id}: capture tables differ in shape or columns")
    worst = 0.0
    for row_a, row_b in zip(rows_a[1:], rows_b[1:]):
        # zip() would silently drop a field one core left out.
        if len(row_a) != len(rows_a[0]) or len(row_b) != len(rows_b[0]):
            raise AssertionError(f"{case_id}: a capture row's width differs from its header")
        for x, y in zip(row_a, row_b):
            try:
                fx, fy = float(x), float(y)
            except ValueError:
                if x != y:
                    raise AssertionError(f"{case_id}: capture value {x!r} != {y!r}")
                continue
            if fx == fy or (math.isnan(fx) and math.isnan(fy)):
                continue
            # NaN or infinity on one side only is a divergence; an infinite
            # value would otherwise make the relative tolerance infinite.
            if not (math.isfinite(fx) and math.isfinite(fy)):
                raise AssertionError(f"{case_id}: capture {fx} vs {fy}")
            difference = abs(fx - fy)
            if difference > CAPTURE_ABSOLUTE + CAPTURE_RELATIVE * max(abs(fx), abs(fy)):
                raise AssertionError(f"{case_id}: capture {fx} vs {fy}")
            worst = max(worst, difference)
    return f"{case_id}: {len(a)} frames, audio max {audio} step(s), captures max {worst:.3g}"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--native", required=True, type=Path)
    parser.add_argument("--wasm", required=True, type=Path)
    parser.add_argument("--node", default="node")
    parser.add_argument("--cases", default=ROOT / "test" / "cases", type=Path)
    args = parser.parse_args()
    cases = sorted(args.cases.glob("*.json"))
    if not cases:
        raise SystemExit("no render cases found")
    with tempfile.TemporaryDirectory(prefix="flows-parity-") as scratch:
        for case_path in cases:
            case = load_json(case_path)
            native, wasm = Path(scratch) / case["id"] / "native", Path(scratch) / case["id"] / "wasm"
            render([str(args.native.resolve())], case, case_path, native)
            render([args.node, str(args.wasm.resolve())], case, case_path, wasm)
            print(compare(case["id"], native, wasm))
    print(f"Render parity passed for {len(cases)} cases.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
