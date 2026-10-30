"""Content guards for clean Connector 3 images; source manifests remain required."""
from pathlib import Path
import csv
import hashlib
import re
import stat


DEVICE_RADIO_SHA256 = "b2c5e7f4f88b061b92be8113ddd5ec688da94729c3a5d07bec9e334335499538"


def audit_files(files, package_manifest):
    """Check unpacked CPIO entries: path -> (mode, contents). No hardware access."""
    manifest = Path(package_manifest)
    if not manifest.is_file():
        raise ValueError("Buildroot legal-info manifest is required for a clean image")
    with manifest.open(newline="") as stream:
        packages = list(csv.DictReader(stream))
    if not packages:
        raise ValueError("Empty Buildroot source/license manifest")
    for package in packages:
        name = package.get("PACKAGE", "").lower()
        if name.startswith(("raumfeld", "teufel")) or any(
                re.search(r"https?://[^/]*(?:raumfeld\.com|teufel\.de)(?:/|$)", value)
                for value in package.values() if value):
            raise ValueError(f"Vendor runtime package/source is forbidden: {name}")

    executable_files = 0
    firmware = {}
    for name, (mode, data) in files.items():
        basename = Path(name).name.lower()
        # Legacy raumfeld-network names are Galdrun's own shared scripts, also
        # used on Connector 2. The OEM /etc/raumfeld tree and binaries are not.
        if (name.startswith(("etc/raumfeld/", "opt/raumfeld/", "usr/share/raumfeld/"))
                or re.search(r"(?:^|lib)raumfeld(?:cpp|renderer|hardware|\.|$)", basename)
                or basename in {"hardwared", "rendererd", "raumfeld-renderer",
                                "raumfeld", "galdrun-connector3-buttons"}):
            raise ValueError(f"Vendor runtime or vendor-dependent bridge is forbidden: {name}")
        if not stat.S_ISREG(mode):
            continue
        digest = hashlib.sha256(data).hexdigest()
        if digest == DEVICE_RADIO_SHA256:
            # Hold this exact binary pending provenance/license review. Being
            # extracted from the device does not make it Raumfeld-authored code.
            raise ValueError(f"Radio firmware redistribution provenance is still unverified: {name}")
        if data.startswith(b"\x7fELF"):
            executable_files += 1
        if name.startswith("lib/firmware/"):
            firmware[name] = digest
    return {"known_vendor_payload_checks_passed": True,
            "device_extracted_radio_present": False,
            "source_manifest_sha256": hashlib.sha256(manifest.read_bytes()).hexdigest(),
            "packages": len(packages), "elf_files": executable_files,
            "hardware_firmware_sha256": firmware}
