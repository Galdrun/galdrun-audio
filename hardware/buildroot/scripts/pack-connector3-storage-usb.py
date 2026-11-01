"""Sign the protected storage-check kernel for USB cold boot; no device access."""
from pathlib import Path
import hashlib
import json
import subprocess
import sys
from connector3_fit import sign_fit


def pack(output, diagnostic, linux, signing, destination):
    scripts = Path(__file__).resolve().parent
    subprocess.run([sys.executable, str(scripts / "verify-connector3-diagnostic.py"), str(diagnostic)], check=True)
    subprocess.run([sys.executable, str(scripts / "verify-connector3-storage-check.py"),
                    str(output), str(diagnostic), str(linux)], check=True)
    report = json.loads((output / "storage-verification.json").read_text())
    payload, fit_report = sign_fit(linux / "arch/arm/boot/zImage",
        linux / "arch/arm/boot/dts/nxp/imx/imx7d-galdrun-connector3-storage-check.dtb",
        diagnostic, signing, "read-only storage check")
    usb = payload + bytes(0xC00000 - len(payload))
    (destination / "connect3.img").write_bytes(usb)
    (destination / "connect3.img.sha256").write_text(hashlib.sha256(usb).hexdigest() + "  connect3.img\n")
    report.update(fit_report)
    report.update({"purpose": "signed USB read-only storage cold-boot check; not an installer",
                   "usb_bytes": len(usb), "usb_sha256": hashlib.sha256(usb).hexdigest(),
                   "hardware_verified": False})
    (destination / "usb-storage-verification.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    pack(*map(lambda value: Path(value).resolve(), sys.argv[1:]))
