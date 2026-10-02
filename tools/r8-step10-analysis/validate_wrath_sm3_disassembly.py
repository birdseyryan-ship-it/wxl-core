#!/usr/bin/env python3

import csv
import hashlib
import json
import re
import sys
from collections import defaultdict
from pathlib import Path

if len(sys.argv) != 2:
    raise SystemExit(
        "usage: validate_wrath_sm3_disassembly.py STAGE_ROOT"
    )

root = Path(sys.argv[1])

unique_tsv = root / "WRATH_SM3_UNIQUE.tsv"
manifest_json = root / "DISASSEMBLY_MANIFEST.json"

meta = {}

with unique_tsv.open(
    "r",
    encoding="utf-8",
    newline="",
) as f:
    for r in csv.DictReader(f, delimiter="\t"):
        meta[r["SHA256"]] = r

if len(meta) != 1067:
    raise RuntimeError(
        f"expected 1067 provenance rows, got {len(meta)}"
    )

manifest = json.loads(
    manifest_json.read_text(encoding="utf-8")
)

if len(manifest) != 1067:
    raise RuntimeError(
        f"expected 1067 disassembly rows, got {len(manifest)}"
    )

missing = []
stage_mismatch = []
asm_by_hash = defaultdict(list)

ps = 0
vs = 0

for r in manifest:
    h = r["dxbc_sha256"]

    if h not in meta:
        missing.append(h)
        continue

    source_stage = meta[h]["STAGE"]

    if source_stage == "ps":
        ps += 1
        expected_profile = "ps_3_0"
    elif source_stage == "vs":
        vs += 1
        expected_profile = "vs_3_0"
    else:
        raise RuntimeError(
            f"unexpected source stage: {source_stage}"
        )

    asm = Path(r["asm_file"])

    if not asm.is_absolute():
        asm = Path.cwd() / asm

    if not asm.is_file():
        raise RuntimeError(
            f"missing assembly output: {asm}"
        )

    text = asm.read_text(
        encoding="utf-8",
        errors="replace",
    )

    if not re.search(
        rf"\b{re.escape(expected_profile)}\b",
        text,
        re.IGNORECASE,
    ):
        stage_mismatch.append(
            (
                h,
                expected_profile,
                str(asm),
            )
        )

    ah = hashlib.sha256(
        text.encode("utf-8")
    ).hexdigest()

    asm_by_hash[ah].append(h)

if missing:
    raise RuntimeError(
        f"{len(missing)} disassemblies lack provenance"
    )

if stage_mismatch:
    raise RuntimeError(
        f"{len(stage_mismatch)} shader-stage mismatches"
    )

if ps != 464:
    raise RuntimeError(
        f"expected 464 unique PS3 disassemblies, got {ps}"
    )

if vs != 603:
    raise RuntimeError(
        f"expected 603 unique VS3 disassemblies, got {vs}"
    )

out = root / "ASM_DEDUPE.tsv"

with out.open(
    "w",
    encoding="utf-8",
    newline="",
) as f:
    w = csv.writer(f, delimiter="\t")

    w.writerow([
        "ASM_SHA256",
        "PROGRAM_COUNT",
        "PROGRAM_HASHES",
    ])

    for ah in sorted(asm_by_hash):
        members = sorted(asm_by_hash[ah])

        w.writerow([
            ah,
            len(members),
            ";".join(members),
        ])

gate = "\n".join([
    "R8 Step 10B-06B2 exact Wrath SM3 disassembly gate",
    "input_unique_sm3=1067",
    f"disassembled={len(manifest)}",
    f"ps3={ps}",
    f"vs3={vs}",
    f"unique_asm={len(asm_by_hash)}",
    f"asm_duplicate_programs={1067-len(asm_by_hash)}",
    f"missing_provenance={len(missing)}",
    f"stage_mismatches={len(stage_mismatch)}",
    "gate=PASS",
    "",
])

(root / "WRATH_DISASSEMBLY_GATE.txt").write_text(
    gate,
    encoding="utf-8",
)

print(gate, end="")
