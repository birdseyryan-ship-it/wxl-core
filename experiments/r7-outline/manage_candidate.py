"""Explicit user-run install/rollback; replaces WarcraftXL.dll only."""
import argparse
import datetime
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess

WOW = "57dd8955fd7238b00969f6011cdaa13dca14daa5849d1f9be64152bd4c7fe5da"
D3D = "73727ffdb3274eda5c42aaf610fae5f1ff3e0fdc0f5ac7e23fffee9c44bcd2f1"
ACCEPTED = "e18b2d9c5daa5bfe17439ea89a12585235d24f373f239e8d8e49a7ef53f69dfd"


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def preflight(client):
    running = subprocess.run(["pgrep", "-af", r"[Ww]ow\.exe$"], capture_output=True, text=True)
    if running.returncode != 1:
        raise RuntimeError("WoW is running or the exact process check failed: " + running.stdout.strip())
    if str(client).startswith("/home/rbirdsey/azerothcore-ironman") or str(client).startswith("/var/home/rbirdsey/azerothcore-ironman"):
        raise RuntimeError("Forbidden server path")
    for name, expected in [("Wow.exe", WOW), ("d3d9.dll", D3D)]:
        if sha(client / name) != expected:
            raise RuntimeError(name + " authority mismatch")


def manifest(package):
    values = {}
    for line in (package / "SHA256SUMS.txt").read_text().splitlines():
        digest, name = line.split(None, 1)
        name = name.lstrip("*")
        if Path(name).name != name or len(digest) != 64:
            raise RuntimeError("Invalid package manifest")
        if sha(package / name) != digest:
            raise RuntimeError("Package file mismatch: " + name)
        values[name] = digest
    if "WarcraftXL.dll" not in values or (package / "d3d9.dll").exists():
        raise RuntimeError("Expected WarcraftXL-only candidate")
    return values


def replace_one(source, destination, expected):
    temp = destination.with_name(destination.name + ".r7-outline-new")
    if temp.exists():
        raise RuntimeError("Existing staging file; preserve and inspect it: " + str(temp))
    with source.open("rb") as src, temp.open("xb") as dst:
        shutil.copyfileobj(src, dst)
        dst.flush()
        os.fsync(dst.fileno())
    if sha(temp) != expected:
        raise RuntimeError("Staged bytes mismatch; live DLL untouched")
    os.replace(temp, destination)
    if sha(destination) != expected:
        raise RuntimeError("Installed hash mismatch")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("action", choices=["install", "rollback"])
    parser.add_argument("--client", type=Path, required=True)
    parser.add_argument("--package", type=Path, default=Path(__file__).resolve().parent)
    args = parser.parse_args()
    client = args.client.resolve(strict=True)
    package = args.package.resolve(strict=True)
    preflight(client)
    values = manifest(package)
    target = client / "WarcraftXL.dll"
    receipt = client / "R7_OUTLINE_BACKUPS" / "ACTIVE_INSTALL.json"
    current = sha(target)
    if args.action == "install":
        if current != ACCEPTED:
            raise RuntimeError("Live WarcraftXL differs from the supplied accepted baseline")
        if receipt.exists():
            raise RuntimeError("An existing diagnostic receipt must be rolled back first")
        stamp = datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%dT%H%M%SZ")
        backup = receipt.parent / (stamp + "-" + values["WarcraftXL.dll"][:8])
        backup.mkdir(parents=True, exist_ok=False)
        shutil.copy2(target, backup / "WarcraftXL.dll")
        if sha(backup / "WarcraftXL.dll") != ACCEPTED:
            raise RuntimeError("Rollback backup did not verify; live DLL untouched")
        data = {"backup": str(backup), "before": ACCEPTED, "candidate": values["WarcraftXL.dll"]}
        with receipt.open("x") as f:
            json.dump(data, f, indent=2)
            f.flush()
            os.fsync(f.fileno())
        # Repeat both authority and process gates immediately before replacement.
        preflight(client)
        if sha(target) != ACCEPTED:
            raise RuntimeError("Live WarcraftXL changed during preflight")
        replace_one(package / "WarcraftXL.dll", target, values["WarcraftXL.dll"])
        print("Installed diagnostic WarcraftXL:", sha(target))
        print("Rollback backup:", backup)
    else:
        data = json.loads(receipt.read_text())
        backup = Path(data["backup"]).resolve(strict=True)
        if not backup.is_relative_to(receipt.parent.resolve()) or data["before"] != ACCEPTED:
            raise RuntimeError("Invalid rollback receipt")
        if current not in {data["candidate"], ACCEPTED}:
            raise RuntimeError("Live DLL is neither this candidate nor its accepted predecessor")
        if sha(backup / "WarcraftXL.dll") != ACCEPTED:
            raise RuntimeError("Rollback source mismatch")
        if current != ACCEPTED:
            preflight(client)
            replace_one(backup / "WarcraftXL.dll", target, ACCEPTED)
        receipt.rename(backup / "ROLLED_BACK.json")
        print("Restored accepted WarcraftXL:", sha(target))
    preflight(client)
    print("Wow.exe and d3d9.dll verified unchanged. Companion files were not written.")


if __name__ == "__main__":
    main()
