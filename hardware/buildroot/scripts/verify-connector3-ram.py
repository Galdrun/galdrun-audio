"""Check the actual source-built RAM payload; never contacts hardware."""
from pathlib import Path
import gzip
import hashlib
import json
import struct
import sys


def verify(output, project):
    config = (output / "build/linux-6.12.112/.config").read_text()
    for option in ("MTD", "MMC", "ATA", "SCSI", "USB_SUPPORT", "MODULES"):
        if f"# CONFIG_{option} is not set" not in config:
            raise ValueError(f"Persistent storage access must be absent: {option}")
    for option in ("FEC", "SMSC_PHY", "PCI_IMX6_HOST", "MWIFIEX_PCIE",
                   "SND_SOC_FSL_SAI", "SND_SIMPLE_CARD", "INET", "DEVTMPFS"):
        if f"CONFIG_{option}=y" not in config:
            raise ValueError(f"Required built-in driver missing: {option}")

    archive = gzip.decompress((output / "images/rootfs.cpio.gz").read_bytes())
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
        raise ValueError("Incomplete initramfs")

    for name in ("init", "usr/sbin/sshd", "usr/sbin/wpa_supplicant", "usr/sbin/wpa_passphrase",
                 "usr/share/udhcpc/default.script", "root/.ssh/authorized_keys",
                 "opt/galdrun-client/galdrun-client",
                 "lib/firmware/mrvl/pcie8897_uapsta.bin",
                 "lib/firmware/imx/sdma/sdma-imx7d.bin",
                 "usr/sbin/raumfeld-network", "usr/sbin/raumfeld-wifi-scan",
                 "usr/sbin/galdrun-connector3-buttons", "usr/sbin/galdrun-connector3-mcu",
                 "etc/init.d/S45raumfeld-network"):
        if name not in files or not files[name][1]:
            raise ValueError(f"Missing RAM payload: {name}")
    if "etc/wpa_supplicant.conf" in files:
        raise ValueError("Generic image must not contain home Wi-Fi credentials")
    network = files["etc/galdrun-network.conf"][1].decode().splitlines()
    if "WIFI_SSID=" not in network or "WIFI_PASSWORD=" not in network:
        raise ValueError("Generic network settings must not contain home credentials")
    board = json.loads(files["etc/galdrun-board.json"][1])
    if board.get("networkSettings") is not True:
        raise ValueError("Network settings capability missing")
    defaults = json.loads(files["opt/galdrun-client/client-default.json"][1])
    if defaults.get("active") is not True:
        raise ValueError("New rooms must be active by default")
    if len(defaults.get("inputs", [])) != 9:
        raise ValueError("Connector 3 must define its nine touch controls")
    key = files["root/.ssh/authorized_keys"]
    if key[0] & 0o777 != 0o600 or b"PRIVATE KEY" in key[1]:
        raise ValueError("SSH public-key permissions/content are wrong")
    if any(name.startswith("etc/ssh/ssh_host_") and name.endswith("_key") for name in files):
        raise ValueError("Host private keys must be generated only in RAM")
    policy = files["etc/ssh/sshd_config"][1].decode()
    for line in ("PasswordAuthentication no", "AuthenticationMethods publickey"):
        if line not in policy.splitlines():
            raise ValueError(f"SSH policy missing {line}")
    binary = files["opt/galdrun-client/galdrun-client"][1]
    release = project / "../../releases/client/linux-armv7/galdrun-client"
    if hashlib.sha256(binary).digest() != hashlib.sha256(release.read_bytes()).digest():
        raise ValueError("RAM client differs from the exported ARMv7 build")
    if binary[:6] != b"\x7fELF\x01\x01" or struct.unpack_from("<H", binary, 18)[0] != 40:
        raise ValueError("Client must be little-endian ARM32 ELF")
    kexec = (output / "target/usr/sbin/kexec").read_bytes()
    if kexec[:6] != b"\x7fELF\x01\x01":
        raise ValueError("kexec must be ARM32 ELF")
    phoff = struct.unpack_from("<I", kexec, 28)[0]
    phsize, phnum = struct.unpack_from("<HH", kexec, 42)
    if any(struct.unpack_from("<I", kexec, phoff + i * phsize)[0] == 3 for i in range(phnum)):
        raise ValueError("kexec must be static to run on the old vendor libc")
    result = {"storage_drivers_absent": True, "network_drivers_built_in": True,
              "wifi_and_sdma_firmware_present": True, "home_wifi_credentials_present": False,
              "key_only_ssh": True, "arm_client_matches_release": True,
              "static_kexec": True, "uncompressed_initramfs_bytes": len(archive),
              "hardware_boot_verified": False, "audio_verified": False}
    (output / "images/ram-verification.json").write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    verify(Path(sys.argv[1]), Path(sys.argv[2]))
