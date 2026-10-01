#!/usr/bin/env python3
"""
Read-only compatibility preflight for the R6 CoAVolFog companion experiment.

It verifies the canonical patched 3.3.5a Wow.exe identity plus the exact
build-12340 code/data assumptions that CoAVolFog's water path checks at runtime.
No files are modified.
"""
from __future__ import annotations

import hashlib
import struct
import sys
from pathlib import Path

EXPECTED_SHA256 = "57dd8955fd7238b00969f6011cdaa13dca14daa5849d1f9be64152bd4c7fe5da"
EXPECTED_TIMESTAMP = 0x4C2452FE
EXPECTED_IMAGE_BASE = 0x00400000

CALLS = [
    ("world render", 0x004FB03D, 0x004F8EA0),
    ("opaque M2 pass", 0x004F911D, 0x00823CB0),
    ("liquid surface pass", 0x004F9170, 0x0077F020),
    ("world text draw", 0x007E5818, 0x006BCE40),
    ("screen effects/world done", 0x004F9281, 0x008C1010),
    ("water pass", 0x00790AA2, 0x008A2240),
]

POINTERS = [
    ("GetProcAddress loader slot", 0x00B2ED98, 0x0041C654),
    ("water material render slot", 0x00A5954C, 0x008A5590),
    ("water no-spec material render slot", 0x00A59580, 0x008A5900),
]

EXACT_BYTES = [
    # WaterClientLayout from jealous-sound/coa-vfog @
    # 4e31ddf53a326ba9c0ad03488b4c30f7ce23c969.
    ("settings bank count check", 0x008A28F8, "3b3d183bd400"),
    ("settings bank array load", 0x008A2900, "a11c3bd400"),
    ("settings bank entry load", 0x008A2952, "8b04b8"),
    ("settings texture copy destination", 0x008A2809, "895dfc"),
    ("LiquidType texture source", 0x008A280C, "8d473c"),
    ("settings texture slot stride", 0x008A282E, "8145fc80000000"),
    ("LiquidType minimum id load", 0x00793DF7, "a17440ad00"),
    ("LiquidType maximum id check", 0x00793E00, "3b357040ad00"),
    ("LiquidType rows load", 0x00793E08, "8b158440ad00"),
    ("LiquidType row load", 0x00793E12, "8b048a"),
    ("LiquidType material id read", 0x008A1FE8, "8b7838"),
    ("material render call/settings argument", 0x008A22C7, "8b46048b0e8b118b520850"),
    ("liquid render pass return", 0x008A2376, "c20800"),
    ("liquid renderer load at water pass", 0x00790A91, "8b0d1086cd00"),
    ("transparent liquid pass index", 0x00790A9B, "6a01"),
    ("liquid renderer kept in ebx", 0x008A224C, "8bd9"),
    ("liquid bucket stride", 0x008A229C, "c1e704"),
    ("liquid bucket count load", 0x008A229F, "8b4c1f04"),
    ("empty-bucket skip", 0x008A22B2, "8b470483c41033db85c089450c763a"),
    ("water material render return", 0x008A58FB, "c21c00"),
    ("water no-spec render return", 0x008A5C6B, "c21c00"),

    # LightRecordLayout needed by BuildWaterInputs / zone water colours.
    ("light record copy destination", 0x007F3574, "b9d48bd300"),
    ("sky top colour store", 0x007EC03C, "89560c"),
    ("sky middle colour store", 0x007EC051, "894e10"),
    ("sky band 1 colour store", 0x007EC063, "894614"),
    ("sky band 2 colour store", 0x007EC075, "895618"),
    ("sky smog colour store", 0x007EC087, "894e1c"),
    ("fog colour store", 0x007EC09C, "894620"),
    ("ocean close colour store", 0x007EC11D, "895638"),
    ("ocean far colour store", 0x007EC132, "894e3c"),
    ("river close colour store", 0x007EC144, "894640"),
    ("river far colour store", 0x007EC152, "895644"),
]


class PE:
    def __init__(self, data: bytes):
        self.data = data
        if len(data) < 0x100 or data[:2] != b"MZ":
            raise ValueError("not an MZ executable")
        pe = struct.unpack_from("<I", data, 0x3C)[0]
        if data[pe:pe + 4] != b"PE\\0\\0":
            raise ValueError("missing PE signature")
        self.pe = pe
        self.sections_count = struct.unpack_from("<H", data, pe + 6)[0]
        self.timestamp = struct.unpack_from("<I", data, pe + 8)[0]
        opt_size = struct.unpack_from("<H", data, pe + 20)[0]
        opt = pe + 24
        magic = struct.unpack_from("<H", data, opt)[0]
        if magic != 0x10B:
            raise ValueError(f"expected PE32 optional header, got 0x{magic:04X}")
        self.image_base = struct.unpack_from("<I", data, opt + 28)[0]
        sec = opt + opt_size
        self.sections = []
        for i in range(self.sections_count):
            p = sec + i * 40
            name = data[p:p + 8].split(b"\\0", 1)[0].decode("ascii", "replace")
            vsize, va, raw_size, raw_ptr = struct.unpack_from("<IIII", data, p + 8)
            self.sections.append((name, va, max(vsize, raw_size), raw_ptr, raw_size))

    def offset(self, va: int) -> int:
        rva = va - self.image_base
        for name, sec_va, span, raw_ptr, raw_size in self.sections:
            if sec_va <= rva < sec_va + span:
                delta = rva - sec_va
                if delta >= raw_size:
                    raise ValueError(f"VA 0x{va:08X} is in virtual-only tail of {name}")
                return raw_ptr + delta
        # Header VAs are theoretically possible, though not expected here.
        if 0 <= rva < min(x[3] for x in self.sections):
            return rva
        raise ValueError(f"VA 0x{va:08X} is not backed by a PE section")

    def read(self, va: int, size: int) -> bytes:
        p = self.offset(va)
        out = self.data[p:p + size]
        if len(out) != size:
            raise ValueError(f"short read at VA 0x{va:08X}")
        return out

    def u32(self, va: int) -> int:
        return struct.unpack("<I", self.read(va, 4))[0]


def main() -> int:
    exe = Path(sys.argv[1] if len(sys.argv) > 1 else "Wow.exe")
    if not exe.is_file():
        print(f"STOP: not found: {exe}")
        return 2

    data = exe.read_bytes()
    digest = hashlib.sha256(data).hexdigest()
    try:
        pe = PE(data)
    except Exception as exc:
        print(f"STOP: PE parse failed: {exc}")
        return 2

    failures = []

    def check(ok: bool, label: str, got: str, expected: str):
        state = "PASS" if ok else "FAIL"
        print(f"{state:4} {label}: {got}")
        if not ok:
            print(f"     expected: {expected}")
            failures.append(label)

    check(digest == EXPECTED_SHA256, "canonical Wow.exe SHA256",
          digest, EXPECTED_SHA256)
    check(pe.timestamp == EXPECTED_TIMESTAMP, "PE timestamp",
          f"0x{pe.timestamp:08X}", f"0x{EXPECTED_TIMESTAMP:08X}")
    check(pe.image_base == EXPECTED_IMAGE_BASE, "PE image base",
          f"0x{pe.image_base:08X}", f"0x{EXPECTED_IMAGE_BASE:08X}")

    for label, site, expected in CALLS:
        try:
            raw = pe.read(site, 5)
            if raw[0] != 0xE8:
                check(False, f"CALL {label} @ 0x{site:08X}",
                      raw.hex(), f"E8 rel32 -> 0x{expected:08X}")
                continue
            rel = struct.unpack("<i", raw[1:5])[0]
            target = (site + 5 + rel) & 0xFFFFFFFF
            check(target == expected, f"CALL {label} @ 0x{site:08X}",
                  f"0x{target:08X}", f"0x{expected:08X}")
        except Exception as exc:
            check(False, f"CALL {label} @ 0x{site:08X}", str(exc),
                  f"0x{expected:08X}")

    for label, va, expected in POINTERS:
        try:
            value = pe.u32(va)
            check(value == expected, f"{label} @ 0x{va:08X}",
                  f"0x{value:08X}", f"0x{expected:08X}")
        except Exception as exc:
            check(False, f"{label} @ 0x{va:08X}", str(exc),
                  f"0x{expected:08X}")

    for label, va, expected_hex in EXACT_BYTES:
        expected = bytes.fromhex(expected_hex)
        try:
            got = pe.read(va, len(expected))
            check(got == expected, f"{label} @ 0x{va:08X}",
                  got.hex(), expected.hex())
        except Exception as exc:
            check(False, f"{label} @ 0x{va:08X}", str(exc),
                  expected.hex())

    print()
    if failures:
        print(f"STOP: CoAVolFog water preflight FAILED ({len(failures)} mismatches).")
        print("Do not install the companion candidate on this executable.")
        return 1

    print("PASS: canonical executable and all checked CoAVolFog water assumptions match.")
    print("This proves static compatibility only; Proton/DXVK and R0-R5 runtime compatibility still require a live gate.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
