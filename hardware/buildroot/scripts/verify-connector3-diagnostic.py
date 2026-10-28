"""Host checks for a Connector 3 diagnostic build; never contacts hardware."""
from pathlib import Path
import hashlib
import json
import subprocess
import struct
import sys
import tempfile


def verify(output):
    output = Path(output)
    images = output / "images"
    kernel = (output / "build/linux-6.12.112/.config").read_text()
    for option in ("MTD", "MMC", "ATA", "SCSI", "USB_SUPPORT", "MODULES"):
        if f"# CONFIG_{option} is not set" not in kernel:
            raise ValueError(f"CONFIG_{option} must be disabled in this RAM-only image")
    for option in ("FEC", "INET", "DEVTMPFS", "SERIAL_IMX_CONSOLE"):
        if f"CONFIG_{option}=y" not in kernel:
            raise ValueError(f"CONFIG_{option} must be built in")
    image = images / "diagnostic.itb"
    tee = (images / "tee.elf").read_bytes()
    if tee[:6] != b"\x7fELF\x01\x01" or struct.unpack_from("<I", tee, 24)[0] != 0x9FD00000:
        raise ValueError("OP-TEE must enter at the factory 0x9fd00000 address")
    if not 0 < (images / "tee-raw.bin").stat().st_size <= 0x300000:
        raise ValueError("OP-TEE exceeds its reserved 3 MiB")
    monitor_config = (output / "build/optee-os-3.15.0/out/include/generated/conf.h").read_text()
    if any(line.startswith("#define CFG_NS_ENTRY_ADDR") for line in monitor_config.splitlines()):
        raise ValueError("OP-TEE must return to U-Boot via LR, not jump directly into Linux")
    if "#define CFG_DT_ADDR 0" not in monitor_config.splitlines():
        raise ValueError("OP-TEE must not consume the original, potentially unpatched FIT DTB")
    if not 0 < image.stat().st_size <= 0xC00000:
        raise ValueError("FIT exceeds the factory bootloader's 12 MiB USB load")
    subprocess.run([str(output / "host/bin/fit_check_sign"), "-f", str(image),
                    "-k", str(images / "diagnostic-control.dtb")], check=True)
    # fit_check_sign loads kernel/FDT/ramdisk; also check the secure loadable.
    with tempfile.TemporaryDirectory(prefix="connector3-fit-") as directory:
        extracted = Path(directory) / "tee.bin"
        subprocess.run([str(output / "host/bin/dumpimage"), "-T", "flat_dt", "-p", "2",
                        "-o", str(extracted), str(image)], check=True, capture_output=True)
        raw = (images / "tee-raw.bin").read_bytes()
        if extracted.read_bytes() != raw:
            raise ValueError("The FIT secure monitor differs from the source-built binary")
        value = subprocess.run([str(output / "host/bin/fdtget"), "-t", "bx", str(image),
                                "/images/tee-1/hash-1", "value"], check=True,
                               capture_output=True, text=True).stdout
        if bytes(int(part, 16) for part in value.split()) != hashlib.sha256(raw).digest():
            raise ValueError("The signed secure-monitor hash is wrong")
    # Check the actual archive that Linux embeds, including modes and key material.
    archive = (images / "rootfs.cpio").read_bytes()
    files = {}
    offset = 0
    while archive[offset:offset + 6] == b"070701":
        fields = [int(archive[offset + 6 + i * 8:offset + 14 + i * 8], 16)
                  for i in range(13)]
        size, namesize = fields[6], fields[11]
        name = archive[offset + 110:offset + 110 + namesize - 1].decode().removeprefix("./")
        offset = (offset + 110 + namesize + 3) & ~3
        files[name] = (fields[1], archive[offset:offset + size])
        offset = (offset + size + 3) & ~3
        if name == "TRAILER!!!":
            break
    else:
        raise ValueError("Invalid or incomplete initramfs")
    for name in ("init", "usr/sbin/sshd", "usr/bin/ssh-keygen", "root/.ssh/authorized_keys",
                 "usr/share/udhcpc/default.script"):
        if name not in files or not files[name][1]:
            raise ValueError(f"Missing initramfs file: {name}")
    if files["root/.ssh/authorized_keys"][0] & 0o777 != 0o600:
        raise ValueError("SSH authorized_keys permissions must be 0600")
    key = files["root/.ssh/authorized_keys"][1]
    if b"PRIVATE KEY" in key or not key.startswith((b"ssh-", b"ecdsa-", b"sk-")):
        raise ValueError("The image must contain a public SSH key, never a private one")
    if not files["init"][0] & 0o111:
        raise ValueError("PID 1 must be executable")
    if not any(line.startswith(b"sshd:") for line in files["etc/passwd"][1].splitlines()):
        raise ValueError("The packaged initramfs must contain the SSH privilege-separation user")
    if not any(line.startswith(b"root:*:") for line in files["etc/shadow"][1].splitlines()):
        raise ValueError("Root needs an unmatchable, unlocked password for key-only SSH")
    if any(name.startswith("etc/ssh/ssh_host_") and name.endswith("_key") for name in files):
        raise ValueError("SSH host private keys must be generated on the device")
    for name in files:
        if Path(name).name in ("nandwrite", "flash_erase", "ubiformat", "fw_setenv", "galdrun-install"):
            raise ValueError(f"Flash writer must not be included: {name}")
    config = files["etc/ssh/sshd_config"][1].decode()
    for setting in ("PasswordAuthentication no", "KbdInteractiveAuthentication no",
                    "AuthenticationMethods publickey", "PermitRootLogin prohibit-password"):
        if setting not in config.splitlines():
            raise ValueError(f"Missing SSH restriction: {setting}")
    result = {"fit_bytes": image.stat().st_size, "signature_verified": True,
              "storage_drivers_disabled": True, "ssh_key_only": True,
              "hardware_boot_tested": False}
    (images / "diagnostic-verification.json").write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(result))


if __name__ == "__main__":
    verify(sys.argv[1])
