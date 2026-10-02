#!/usr/bin/env python3

import csv
import hashlib
import shutil
import struct
import sys
from collections import defaultdict
from pathlib import Path

if len(sys.argv) != 3:
    raise SystemExit(
        "usage: parse_wrath_bls10003.py RAW_DIR OUTPUT_DIR"
    )

raw = Path(sys.argv[1])
out = Path(sys.argv[2])

unique_dir = out / "unique_dxbc"
unique_dir.mkdir(parents=True, exist_ok=True)

def sha256(data):
    return hashlib.sha256(data).hexdigest()

def classify(filename):
    lower = filename.lower()

    if "__ps_3_0__" in lower:
        stage = "ps"
        expected_token = 0xFFFF0300
        expected_count = 16
    elif "__vs_3_0__" in lower:
        stage = "vs"
        expected_token = 0xFFFE0300
        expected_count = 90
    else:
        raise RuntimeError(
            f"cannot determine stage from filename: {filename}"
        )

    kind = "WMO" if "mapobj" in lower else "M2"

    return stage, kind, expected_token, expected_count

slot_rows = []
by_hash = defaultdict(list)
zero_files = []

container_count = 0
ps_slots = 0
vs_slots = 0

for p in sorted(raw.glob("*.bls")):
    data = p.read_bytes()

    stage, kind, expected_token, expected_count = classify(
        p.name
    )

    if not data:
        zero_files.append(p.name)
        continue

    container_count += 1

    if len(data) < 12:
        raise RuntimeError(f"short BLS container: {p.name}")

    if data[:4] != b"HSXG":
        raise RuntimeError(
            f"unexpected BLS magic in {p.name}: {data[:4]!r}"
        )

    version = struct.unpack_from("<I", data, 4)[0]

    if version != 0x00010003:
        raise RuntimeError(
            f"unexpected BLS version in {p.name}: {version:#x}"
        )

    shader_count = struct.unpack_from("<I", data, 8)[0]

    if shader_count != expected_count:
        raise RuntimeError(
            f"{p.name}: expected {expected_count} records, "
            f"got {shader_count}"
        )

    pos = 12

    for slot in range(shader_count):
        if pos + 16 > len(data):
            raise RuntimeError(
                f"{p.name} slot {slot}: truncated record header"
            )

        meta0, meta1, meta2, byte_count = struct.unpack_from(
            "<IIII",
            data,
            pos,
        )

        pos += 16

        if byte_count < 8:
            raise RuntimeError(
                f"{p.name} slot {slot}: "
                f"implausible bytecode size {byte_count}"
            )

        if byte_count % 4:
            raise RuntimeError(
                f"{p.name} slot {slot}: "
                "bytecode size is not DWORD aligned"
            )

        if pos + byte_count > len(data):
            raise RuntimeError(
                f"{p.name} slot {slot}: bytecode exceeds container"
            )

        code = data[pos:pos + byte_count]
        pos += byte_count

        first = struct.unpack_from("<I", code, 0)[0]
        last = struct.unpack_from("<I", code, len(code) - 4)[0]

        if first != expected_token:
            raise RuntimeError(
                f"{p.name} slot {slot}: "
                f"wrong shader token {first:#x}"
            )

        if last != 0x0000FFFF:
            raise RuntimeError(
                f"{p.name} slot {slot}: "
                f"missing D3D9 END token, got {last:#x}"
            )

        h = sha256(code)

        row = {
            "source_file": p.name,
            "slot": slot,
            "kind": kind,
            "stage": stage,
            "meta0": meta0,
            "meta1": meta1,
            "meta2": meta2,
            "bytecode_bytes": byte_count,
            "sha256": h,
        }

        slot_rows.append(row)
        by_hash[h].append(row)

        target = unique_dir / f"{h}.dxbc"

        if not target.exists():
            target.write_bytes(code)

        if stage == "ps":
            ps_slots += 1
        else:
            vs_slots += 1

    if pos != len(data):
        raise RuntimeError(
            f"{p.name}: parser stopped at {pos}, "
            f"file length is {len(data)}"
        )

# ------------------------------------------------------------
# Hard gates from independently inspected 10B-06B1 corpus
# ------------------------------------------------------------

if len(list(raw.glob("*.bls"))) != 55:
    raise RuntimeError("expected 55 effective paths")

if len(zero_files) != 4:
    raise RuntimeError(
        f"expected four zero-length paths, got {len(zero_files)}"
    )

if container_count != 51:
    raise RuntimeError(
        f"expected 51 populated containers, got {container_count}"
    )

if len(slot_rows) != 2000:
    raise RuntimeError(
        f"expected 2000 total shader slots, got {len(slot_rows)}"
    )

if ps_slots != 560:
    raise RuntimeError(
        f"expected 560 PS3 slots, got {ps_slots}"
    )

if vs_slots != 1440:
    raise RuntimeError(
        f"expected 1440 VS3 slots, got {vs_slots}"
    )

if len(by_hash) != 1067:
    raise RuntimeError(
        f"expected 1067 unique shader programs, got {len(by_hash)}"
    )

# ------------------------------------------------------------
# Provenance manifest
# ------------------------------------------------------------

slot_manifest = out / "WRATH_SM3_SLOT_MANIFEST.tsv"

with slot_manifest.open(
    "w",
    encoding="utf-8",
    newline="",
) as f:
    w = csv.writer(f, delimiter="\t")

    w.writerow([
        "SOURCE_FILE",
        "SLOT",
        "KIND",
        "STAGE",
        "META0",
        "META1",
        "META2",
        "BYTECODE_BYTES",
        "SHA256",
    ])

    for r in slot_rows:
        w.writerow([
            r["source_file"],
            r["slot"],
            r["kind"],
            r["stage"],
            f"0x{r['meta0']:08X}",
            f"0x{r['meta1']:08X}",
            f"0x{r['meta2']:08X}",
            r["bytecode_bytes"],
            r["sha256"],
        ])

# ------------------------------------------------------------
# Unique-program manifest
# ------------------------------------------------------------

unique_manifest = out / "WRATH_SM3_UNIQUE.tsv"

with unique_manifest.open(
    "w",
    encoding="utf-8",
    newline="",
) as f:
    w = csv.writer(f, delimiter="\t")

    w.writerow([
        "SHA256",
        "STAGE",
        "BYTECODE_BYTES",
        "OCCURRENCES",
        "FAMILY_COUNT",
        "KINDS",
        "REPRESENTATIVE_FILE",
        "REPRESENTATIVE_SLOT",
        "FAMILIES",
    ])

    for h in sorted(by_hash):
        members = by_hash[h]
        stages = {r["stage"] for r in members}

        if len(stages) != 1:
            raise RuntimeError(
                f"cross-stage bytecode identity: {h}"
            )

        families = sorted({
            r["source_file"]
            for r in members
        })

        kinds = sorted({
            r["kind"]
            for r in members
        })

        rep = members[0]

        w.writerow([
            h,
            rep["stage"],
            rep["bytecode_bytes"],
            len(members),
            len(families),
            ";".join(kinds),
            rep["source_file"],
            rep["slot"],
            ";".join(families),
        ])

# ------------------------------------------------------------
# Zero-length winning patch paths
# ------------------------------------------------------------

(out / "ZERO_LENGTH_EFFECTIVE_PATHS.txt").write_text(
    "\n".join(sorted(zero_files)) + "\n",
    encoding="utf-8",
)

# ------------------------------------------------------------
# Independent category counts
# ------------------------------------------------------------

sets = defaultdict(set)

for r in slot_rows:
    sets[(r["kind"], r["stage"])].add(r["sha256"])

wmo_ps = len(sets[("WMO", "ps")])
m2_ps = len(sets[("M2", "ps")])
wmo_vs = len(sets[("WMO", "vs")])
m2_vs = len(sets[("M2", "vs")])

if wmo_ps != 72:
    raise RuntimeError(f"expected WMO PS unique=72, got {wmo_ps}")

if m2_ps != 392:
    raise RuntimeError(f"expected M2 PS unique=392, got {m2_ps}")

if wmo_vs != 117:
    raise RuntimeError(f"expected WMO VS unique=117, got {wmo_vs}")

if m2_vs != 486:
    raise RuntimeError(f"expected M2 VS unique=486, got {m2_vs}")

if sets[("WMO", "ps")] & sets[("M2", "ps")]:
    raise RuntimeError("unexpected WMO/M2 PS bytecode overlap")

if sets[("WMO", "vs")] & sets[("M2", "vs")]:
    raise RuntimeError("unexpected WMO/M2 VS bytecode overlap")

ps_unique = sum(
    1
    for h, members in by_hash.items()
    if members[0]["stage"] == "ps"
)

vs_unique = sum(
    1
    for h, members in by_hash.items()
    if members[0]["stage"] == "vs"
)

if ps_unique != 464:
    raise RuntimeError(
        f"expected 464 unique PS3, got {ps_unique}"
    )

if vs_unique != 603:
    raise RuntimeError(
        f"expected 603 unique VS3, got {vs_unique}"
    )

# ------------------------------------------------------------
# Summary
# ------------------------------------------------------------

summary = "\n".join([
    "R8 Step 10B-06B2 Wrath BLS v0x10003 preparation",
    "effective_paths=55",
    "zero_length_effective_paths=4",
    "populated_containers=51",
    "container_magic=HSXG",
    "container_version=0x00010003",
    "ps3_slots=560",
    "vs3_slots=1440",
    "total_slots=2000",
    "unique_ps3=464",
    "unique_vs3=603",
    "global_unique_sm3=1067",
    "wmo_unique_ps3=72",
    "m2_unique_ps3=392",
    "wmo_unique_vs3=117",
    "m2_unique_vs3=486",
    "wmo_m2_ps_overlap=0",
    "wmo_m2_vs_overlap=0",
    "payload_extension=.dxbc",
    "payload_content=legacy Direct3D9 SM3 token streams",
    "gate=PASS",
    "",
])

(out / "PREP_SUMMARY.txt").write_text(
    summary,
    encoding="utf-8",
)

print(summary, end="")
