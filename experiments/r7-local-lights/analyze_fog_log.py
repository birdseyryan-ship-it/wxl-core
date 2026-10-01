from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re
import statistics


FRAME = re.compile(r"^(\d\d:\d\d:\d\d\.\d+) frame (\d+):.*fog gpu ([\d.]+) ms \(median of (\d+) frames, (\d+) skipped\)")
CAMERA = re.compile(r" camera \(([^)]+)\).* target \(([^)]+)\)")
POINTS = re.compile(r"local points (?:(\d+) captured|not captured) enabled (\d+)")
SETTING = re.compile(r"settings from CoAVolFog\.ini: (\w+) (\S+) -> (\S+)")


def seconds(value: str) -> float:
    hour, minute, second = value.split(":")
    return int(hour) * 3600 + int(minute) * 60 + float(second)


def analyze(data: bytes) -> dict:
    samples = []
    events = []
    settings = {"LocalLights": "UNKNOWN", "Temporal": "UNKNOWN"}
    pending = None
    previous = None
    for number, line in enumerate(data.decode("utf-8-sig").splitlines(), 1):
        setting = SETTING.search(line)
        if setting:
            events.append({"line": number, "time": line[:12], "setting": setting[1],
                           "from": setting[2], "to": setting[3]})
            settings[setting[1]] = setting[3]
        frame = FRAME.search(line)
        if frame:
            pending = {"line": number, "time": frame[1], "frame": int(frame[2]), "fog_ms": float(frame[3]),
                       "sample_count": int(frame[4]), "skipped": int(frame[5]), "camera": None,
                       "target": None, "captured_lights": None, "enabled": None, "settings_at_end": dict(settings)}
            if previous:
                elapsed = (seconds(frame[1]) - seconds(previous["time"])) % 86400
                pending["interval_seconds"] = elapsed
                pending["average_frame_rate_from_log"] = (int(frame[2]) - previous["frame"]) / elapsed if elapsed else None
                pending["settings_changed_in_interval"] = any(
                    previous["line"] < event["line"] <= number for event in events)
            samples.append(pending)
            previous = pending
        if pending:
            camera = CAMERA.search(line)
            if camera:
                pending["camera"], pending["target"] = camera[1], camera[2]
            points = POINTS.search(line)
            if points:
                pending["captured_lights"] = int(points[1] or 0)
                pending["enabled"] = int(points[2])
    disabled = [sample for sample in samples if sample["enabled"] == 0
                and not sample.get("settings_changed_in_interval", True)
                and sample["settings_at_end"]["LocalLights"] == "0"]
    fast = [sample for sample in disabled if sample["fog_ms"] < 8]
    slow = [sample for sample in disabled if sample["fog_ms"] > 18]
    fixed = {}
    for sample in disabled:
        identity = (sample["camera"], sample["target"], sample["settings_at_end"]["Temporal"])
        fixed.setdefault(identity, []).append(sample)
    contrasts = []
    for (camera, target, temporal), group in fixed.items():
        lower = [sample for sample in group if sample["fog_ms"] < 8]
        upper = [sample for sample in group if sample["fog_ms"] > 18]
        if lower and upper:
            low_median = statistics.median(sample["fog_ms"] for sample in lower)
            high_median = statistics.median(sample["fog_ms"] for sample in upper)
            contrasts.append({"camera": camera, "target": target, "temporal": temporal,
                              "fast_count": len(lower), "slow_count": len(upper), "fast_median_ms": low_median,
                              "slow_median_ms": high_median, "difference_ms": high_median - low_median,
                              "fast_times": [sample["time"] for sample in lower],
                              "slow_times": [sample["time"] for sample in upper]})
    return {"input_sha256": hashlib.sha256(data).hexdigest(), "fog_records": len(samples),
            "disabled_stable_records": len(disabled), "disabled_fast_records": len(fast),
            "disabled_slow_records": len(slow), "fixed_camera_contrasts": contrasts, "settings_events": events,
            "samples": samples, "root_cause": "UNKNOWN", "gpu_speedup_validated": False,
            "scope": "Logged interval medians and end-of-interval camera/settings; continuous camera stability is not proven.",
            "timer_warning": "Fog timestamps span depth transport, march, filtering, composite and optional late GodRays; idle time inside boundaries can be included."}


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("log", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    report = analyze(args.log.read_bytes())
    args.output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({key: report[key] for key in ("fog_records", "disabled_stable_records",
                                                  "disabled_fast_records", "disabled_slow_records", "root_cause")}, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
