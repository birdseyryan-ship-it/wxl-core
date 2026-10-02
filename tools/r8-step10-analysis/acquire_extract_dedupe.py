#!/usr/bin/env python3

import csv
import json
import shutil
import subprocess
import sys
from collections import defaultdict
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
R6 = ROOT / "tools" / "r6-water-acquire"
sys.path.insert(0, str(R6))

import fetch_classic_water
import fetch_classic_water_archives as archive_fetch

TARGET_FILE = (
    ROOT
    / "tools"
    / "r8-step10-analysis"
    / "step10_unique_targets.tsv"
)

OUT = ROOT / "r8-step10-stage"
ACQUIRED = OUT / "acquired"
DXBC = OUT / "dxbc"
UNIQUE = OUT / "unique_dxbc"


def parse_targets():
    rows = []
    seen_ekey = set()

    with TARGET_FILE.open(
        "r",
        encoding="utf-8",
        newline=""
    ) as f:

        for lineno, raw in enumerate(f, 1):
            raw = raw.rstrip("\r\n")

            if not raw:
                continue

            fields = raw.split()

            if len(fields) < 6:
                raise RuntimeError(
                    f"line {lineno}: "
                    f"expected >=6 fields, got {len(fields)}"
                )

            fdid, path, flags, locale, ckey, ekey = fields[:6]

            if len(ckey) != 32:
                raise RuntimeError(
                    f"line {lineno}: invalid CKey"
                )

            if len(ekey) != 32:
                raise RuntimeError(
                    f"line {lineno}: invalid EKey"
                )

            int(ckey, 16)
            int(ekey, 16)

            if ekey in seen_ekey:
                raise RuntimeError(
                    f"duplicate EKey after dedupe: {ekey}"
                )

            seen_ekey.add(ekey)

            rows.append((
                path,
                ckey.lower(),
                ekey.lower(),
            ))

    if len(rows) != 89:
        raise RuntimeError(
            f"expected 89 targets, got {len(rows)}"
        )

    return rows


def verify_acquisition():
    p = ACQUIRED / "ARCHIVE_FALLBACK.json"

    data = json.loads(p.read_text())

    expected = {
        "archive_count": 606,
        "target_count": 89,
        "locations": 89,
        "verified": 89,
        "unresolved": 0,
        "range_failures": 0,
        "decode_failures": 0,
    }

    for key, value in expected.items():
        actual = data.get(key)

        if actual != value:
            raise RuntimeError(
                f"{key}: expected {value}, got {actual}"
            )

    for r in data["results"]:
        if r.get("ckey_match") is not True:
            raise RuntimeError(
                f"CKey not positively verified: {r.get('path')}"
            )


def main():
    OUT.mkdir(exist_ok=True)

    rows = parse_targets()

    # Exact CP08 code remains authoritative.
    # Only target rows and output root are substituted.
    fetch_classic_water.ROWS = rows
    archive_fetch.ROWS = rows
    archive_fetch.ROOT = ACQUIRED

    print("=== ACQUISITION ===")
    rc = archive_fetch.main()

    if rc != 0:
        raise RuntimeError(
            f"archive acquisition returned {rc}"
        )

    verify_acquisition()

    print()
    print("=== EXACT CP08 GXSH EXTRACTION ===")

    subprocess.run(
        [
            sys.executable,
            str(R6 / "gxsh_extract_dxbc.py"),
            str(ACQUIRED / "decoded"),
            str(DXBC),
        ],
        check=True,
    )

    manifest_path = OUT / "DXBC_MANIFEST.json"
    rows = json.loads(manifest_path.read_text())

    if len(rows) != 24257:
        raise RuntimeError(
            f"expected 24257 extracted slots, got {len(rows)}"
        )

    by_hash = defaultdict(list)

    for r in rows:
        by_hash[r["dxbc_sha256"]].append(r)

    if len(by_hash) != 9228:
        raise RuntimeError(
            f"expected 9228 unique DXBC, got {len(by_hash)}"
        )

    UNIQUE.mkdir(exist_ok=True)

    map_path = OUT / "UNIQUE_DXBC_MAP.tsv"

    with map_path.open(
        "w",
        encoding="utf-8",
        newline=""
    ) as f:

        w = csv.writer(f, delimiter="\t")

        w.writerow([
            "DXBC_SHA256",
            "OCCURRENCES",
            "FAMILY_COUNT",
            "DXBC_BYTES",
            "REPRESENTATIVE_FAMILY",
            "REPRESENTATIVE_SLOT",
            "FAMILIES",
        ])

        for h in sorted(by_hash):
            members = by_hash[h]
            rep = members[0]

            src = Path(rep["dxbc_file"])

            if not src.is_absolute():
                src = ROOT / src

            dst = UNIQUE / f"{h}.dxbc"

            shutil.copy2(src, dst)

            families = sorted({
                x["family"]
                for x in members
            })

            w.writerow([
                h,
                len(members),
                len(families),
                rep["dxbc_bytes"],
                rep["family"],
                rep["slot"],
                ";".join(families),
            ])

    unique_files = list(UNIQUE.glob("*.dxbc"))

    if len(unique_files) != 9228:
        raise RuntimeError(
            f"expected 9228 representative files, "
            f"got {len(unique_files)}"
        )

    summary = "\n".join([
        "R8 Step 10B-05B Linux acquisition/extraction preparation",
        "build=1.13.2.31650",
        "targets=89",
        "archive_indices=606",
        "verified_objects=89",
        "families=89",
        "extracted_slots=24257",
        "unique_dxbc=9228",
        "gate=PASS",
        "",
    ])

    (OUT / "STEP10B05B_PREP_SUMMARY.txt").write_text(
        summary
    )

    print()
    print(summary, end="")


if __name__ == "__main__":
    main()
