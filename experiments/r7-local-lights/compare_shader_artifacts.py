from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re


ARRAY = re.compile(r"\bg_\w+\s*\[[^]]*\]\s*=\s*\{([^}]+)\}")
ALLOWED = ("ps_lit_march_", "ps_lit_noisy_march_", "ps_lit_composite_", "ps_lit_split_composite_")


def bytecode(path: Path) -> bytes:
    matches = ARRAY.findall(path.read_text(encoding="utf-8-sig"))
    if len(matches) != 1:
        raise ValueError(f"expected one compiled shader byte array: {path}")
    tokens = [token.strip() for token in matches[0].split(",") if token.strip()]
    return bytes(int(token, 16 if token.lower().startswith("0x") else 10) for token in tokens)


def compare(baseline: Path, cached: Path) -> dict:
    left = {path.name: path for path in (baseline / "shaders").glob("*.h")}
    right = {path.name: path for path in (cached / "shaders").glob("*.h")}
    if not left or left.keys() != right.keys():
        raise ValueError("compiled shader sets must be nonempty and identical")
    changed = []
    identical = []
    unexpected = []
    for name in sorted(left):
        old, new = bytecode(left[name]), bytecode(right[name])
        if old == new:
            identical.append(name)
        else:
            changed.append({"shader": name, "baseline_sha256": hashlib.sha256(old).hexdigest(),
                            "cached_sha256": hashlib.sha256(new).hexdigest()})
            if not name.startswith(ALLOWED):
                unexpected.append(name)
    for name in ("waterdata.bin", "fogdata.bin"):
        if (baseline / name).read_bytes() != (cached / name).read_bytes():
            unexpected.append(name)
    return {"status": "failed" if unexpected else "passed", "identical": identical, "changed": changed,
            "unexpected_changes": unexpected, "gpu_speedup_validated": False}


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("baseline", type=Path)
    parser.add_argument("cached", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    result = compare(args.baseline, args.cached)
    args.output.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"status": result["status"], "changed_count": len(result["changed"]),
                      "unexpected_changes": result["unexpected_changes"]}, indent=2))
    return int(result["status"] != "passed")


if __name__ == "__main__":
    raise SystemExit(main())
