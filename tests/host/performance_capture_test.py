"""Validate actual host CPU producer records through the production parser."""
import importlib.util
import json
from pathlib import Path
import sys

spec = importlib.util.spec_from_file_location("profile_format", Path(__file__).resolve().parents[2] / "tools" / "perf" / "profile_format.py")
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


def verify(path):
    parser = module.ProfileParser()
    native_count = 0
    samples = {"cpu.frame.wall": [], "cpu.frame.period": []}
    overflow = []
    with open(path, encoding="utf-8", errors="strict") as capture:
        line_number = 0
        while True:
            line = capture.readline(module.MAX_LINE + 2)
            if not line:
                break
            line_number += 1
            assert len(line) <= module.MAX_LINE, "producer emitted oversized line"
            if not line.startswith("KPERF "):
                continue
            native_count += 1
            raw = json.loads(line[6:])
            assert raw.get("name") != "overflow", "JSON builder overflowed"
            if raw.get("type") == "samples":
                assert len(raw["samples"]) <= 8, "producer batch exceeded eight samples"
            rows = parser.parse_line(line, line_number)
            assert rows, "producer record was not normalized"
            for row in rows:
                if row["type"] == "metrics":
                    window = row.get("window", {})
                    assert window.get("samples", 0) > 0, "producer invented zero-frame window"
                    assert window["end_us"] >= window["start_us"], "window moved backwards"
                    assert window["first_frame"] <= window["last_frame"], "frame range moved backwards"
                if row["type"] == "sample" and row["stream"] in samples:
                    assert row["tags"].get("frame_domain") == "cpu", "sample lacks CPU frame domain"
                    samples[row["stream"]].append(row)
                if row.get("name") == "sample_overflow":
                    overflow.append(row)
    assert not parser.issues, f"actual producer parse issues: {parser.issues}"
    assert native_count > 0, "capture contained no producer records"
    assert all(samples.values()), "capture lacked wall or period samples"
    assert len(overflow) == 1 and overflow[0]["count"] == 2 and overflow[0]["unit"] == "frames", "expected fixture loss event missing or incorrect"
    for stream, rows in samples.items():
        frames = [row["frame"] for row in rows]
        times = [row["time"] for row in rows]
        assert frames == sorted(set(frames)), f"duplicate or reversed frames: {stream}"
        assert times == sorted(times), f"reversed timestamps: {stream}"
    print(f"PASS:PERFORMANCE_CAPTURE records={native_count} wall={len(samples['cpu.frame.wall'])} period={len(samples['cpu.frame.period'])} bounded_loss_frames=2")


def verify_formatter(path):
    parser = module.ProfileParser()
    rows = []
    with open(path, encoding="utf-8") as capture:
        for i, line in enumerate(capture, 1):
            assert len(line) <= 1000, "formatter line exceeded transport bound"
            rows.extend(parser.parse_line(line, i))
    assert not parser.issues, f"formatter parse issues: {parser.issues}"
    assert rows[0]["name"] == "overflow", "formatter overflow is not an explicit event"
    assert len([r for r in rows if r["type"] == "sample" and r["stream"] == "gpu.busy"]) == 8
    print("PASS:PERFORMANCE_FORMATTER_CAPTURE overflow=1 gpu_samples=8")


if __name__ == "__main__":
    if len(sys.argv) == 3 and sys.argv[1] == "--formatter":
        verify_formatter(sys.argv[2])
        raise SystemExit(0)
    if len(sys.argv) != 2:
        raise SystemExit("usage: performance_capture_test.py <CPU producer capture>")
    verify(sys.argv[1])
