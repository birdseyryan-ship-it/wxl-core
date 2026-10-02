#!/usr/bin/env python3

import csv
import json
import os
import shutil
import sys
from collections import defaultdict
from pathlib import Path

if len(sys.argv) != 3:
    raise SystemExit(
        "usage: summarize_disassembly.py "
        "UNIQUE_DXBC_MAP.tsv DISASSEMBLY_MANIFEST.json"
    )

map_path = Path(sys.argv[1])
disasm_path = Path(sys.argv[2])

dxbc_meta = {}

with map_path.open(
    "r",
    encoding="utf-8",
    newline=""
) as f:

    for r in csv.DictReader(f, delimiter="\t"):
        dxbc_meta[r["DXBC_SHA256"]] = r

rows = json.loads(disasm_path.read_text())

if len(rows) != 9228:
    raise RuntimeError(
        f"expected 9228 disassemblies, got {len(rows)}"
    )

by_asm = defaultdict(list)

missing = []

for r in rows:
    h = r["dxbc_sha256"]

    if h not in dxbc_meta:
        missing.append(h)
        continue

    by_asm[r["asm_sha256"]].append(r)

if missing:
    raise RuntimeError(
        f"{len(missing)} DXBC hashes lack provenance mapping"
    )

unique_asm = Path("unique_asm")
unique_asm.mkdir(exist_ok=True)

out_tsv = Path("ASM_DEDUPE.tsv")

with out_tsv.open(
    "w",
    encoding="utf-8",
    newline=""
) as f:

    w = csv.writer(f, delimiter="\t")

    w.writerow([
        "ASM_SHA256",
        "DXBC_COUNT",
        "FAMILY_COUNT",
        "ASM_BYTES",
        "REPRESENTATIVE_ASM",
        "DXBC_HASHES",
        "FAMILIES",
    ])

    for ah in sorted(by_asm):
        members = by_asm[ah]

        dxbc_hashes = sorted({
            x["dxbc_sha256"]
            for x in members
        })

        families = set()

        for h in dxbc_hashes:
            families.update(
                x
                for x in dxbc_meta[h]["FAMILIES"].split(";")
                if x
            )

        rep = members[0]

        src = Path(rep["asm_file"])
        dst = unique_asm / f"{ah}.asm"

        try:
            os.link(src, dst)
        except OSError:
            shutil.copy2(src, dst)

        w.writerow([
            ah,
            len(dxbc_hashes),
            len(families),
            rep["asm_bytes"],
            rep["asm_file"],
            ";".join(dxbc_hashes),
            ";".join(sorted(families)),
        ])

asm_files = len(list(unique_asm.glob("*.asm")))

if asm_files != len(by_asm):
    raise RuntimeError(
        "unique ASM representative count mismatch"
    )

summary = "\n".join([
    "R8 Step 10B-05B exact Microsoft D3DDisassemble gate",
    f"input_unique_dxbc={len(rows)}",
    f"unique_asm={len(by_asm)}",
    f"asm_duplicate_dxbc={len(rows)-len(by_asm)}",
    f"mapped_dxbc={len(dxbc_meta)}",
    f"missing_provenance={len(missing)}",
    f"unique_asm_files={asm_files}",
    "gate=PASS",
    "",
])

Path("DISASSEMBLY_GATE.txt").write_text(summary)

print(summary, end="")
