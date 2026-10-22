"""The source of the GPL and LGPL software in connect2.img, as one release asset.

GPLv2 section 3(a): the source is offered from the same place as the image, so the
GitHub release that publishes connect2.img points to a release holding this tar.
The tag is a hash of the bundled sources, so one bundle serves every Galdrun release
until Buildroot, a package or a kernel patch changes.

Usage: source-bundle.py <work directory> --tag
       source-bundle.py <work directory> --write <file.tar>
"""
from pathlib import Path
import csv
import hashlib
import io
import sys
import tarfile
import urllib.request

project = Path(__file__).resolve().parents[1]
REPOSITORY = "https://github.com/Galdrun/galdrun-audio"
TREES = ("connector2", "connector2-installer")
# The Bootlin toolchain's glibc ships in the image (LGPL); Buildroot's legal-info only
# saves the binary toolchain, so its source comes from Bootlin's own source mirror.
GLIBC = "glibc-2.39-74-g198632a05f6c7b9ab67d3331d8caace9ceabb685"
GLIBC_URL = f"https://toolchains.bootlin.com/downloads/releases/sources/{GLIBC}/{GLIBC}.tar.gz"
GLIBC_SHA256 = "2858e8e47c4c0df32b526c56d5590ec939e8178201ee9526bb070999ce4aa1f2"
BUILDROOT = "buildroot-2025.02.18.tar.xz"


def glibc_archive():
    path = project / "downloads/toolchain-sources" / f"{GLIBC}.tar.gz"
    if not path.exists():
        path.parent.mkdir(parents=True, exist_ok=True)
        part = path.with_suffix(".part")
        with urllib.request.urlopen(GLIBC_URL) as response, open(part, "wb") as out:
            while chunk := response.read(1 << 20):
                out.write(chunk)
        part.rename(path)
    if hashlib.sha256(path.read_bytes()).hexdigest() != GLIBC_SHA256:
        raise ValueError(f"{path} does not match its pinned SHA-256")
    return path


def members(work):
    """(archive name, file) for every bundled file, firmware tree first."""
    found = {}
    for tree in TREES:
        info = work / tree / "legal-info"
        hashes = {}
        for line in (info / "legal-info.sha256").read_text().splitlines():
            digest, name = line.split("  ", 1)
            hashes[name] = digest
        with open(info / "manifest.csv", newline="") as manifest:
            for row in csv.DictReader(manifest):
                if "GPL" not in row["LICENSE"]:
                    continue
                folder = f"{row['PACKAGE']}-{row['VERSION']}"
                files = {name: digest for name, digest in hashes.items() if name.startswith(f"sources/{folder}/")}
                if not files:
                    raise ValueError(f"{tree}: no saved source for {folder}")
                if folder in found and found[folder][1] != files:
                    folder = f"{tree}/{folder}"  # same package, other patches: keep both
                found.setdefault(folder, (info, files))
    result = []
    for folder, (info, files) in sorted(found.items()):
        prefix = folder.rsplit("/", 1)[0] + "/" if "/" in folder else ""
        result += [(f"{prefix}{name}", info / name) for name in sorted(files)]
    result.append((f"toolchain/{GLIBC}.tar.gz", glibc_archive()))
    result.append((f"buildroot/{BUILDROOT}", project / "downloads" / BUILDROOT))
    for tree in TREES:
        result.append((f"manifest/{tree}.csv", work / tree / "legal-info/manifest.csv"))
    return result


def tag(work):
    digest = hashlib.sha256()
    for name, path in members(work):
        if not name.startswith("manifest/"):
            digest.update(name.encode() + b"\0" + hashlib.sha256(path.read_bytes()).digest())
    return f"connector2-sources-{digest.hexdigest()[:12]}"


def readme(name):
    return (
        f"{name}: the source of the GPL and LGPL software in Galdrun's connect2.img\n\n"
        "sources/     each package as Buildroot saved it: upstream archive plus the patches applied\n"
        f"toolchain/   glibc of the Bootlin armv7-eabihf 2024.05-1 toolchain\n"
        f"buildroot/   Buildroot itself, which builds the image\n"
        "manifest/    every package in the image with its version and license\n\n"
        f"Galdrun's own parts (board files, device tree, kernel driver, configs) are in\n{REPOSITORY}\n"
        "under hardware/buildroot/ at the tag of the firmware release; its README builds the image.\n")


def write(work, path):
    name = tag(work)
    with tarfile.open(path, "w", format=tarfile.PAX_FORMAT) as archive:
        text = readme(name).encode()
        info = tarfile.TarInfo(f"{name}/README.txt")
        info.size, info.mode = len(text), 0o644
        archive.addfile(info, io.BytesIO(text))
        for member, source in members(work):
            info = archive.gettarinfo(source, f"{name}/{member}")
            info.mode, info.uid, info.gid, info.uname, info.gname = 0o644, 0, 0, "root", "root"
            with open(source, "rb") as data:
                archive.addfile(info, data)
    return name


if __name__ == "__main__":
    if len(sys.argv) == 3 and sys.argv[2] == "--tag":
        print(tag(Path(sys.argv[1])))
    elif len(sys.argv) == 4 and sys.argv[2] == "--write":
        print(write(Path(sys.argv[1]), Path(sys.argv[3])))
    else:
        sys.exit(__doc__)
