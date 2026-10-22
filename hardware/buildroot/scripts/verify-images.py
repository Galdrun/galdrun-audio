"""Verify produced Connector2 images before exporting them; no device writes."""
from pathlib import Path
import hashlib
import json
import os
import struct
import sys
import tarfile
import zlib


def require(condition, message):
    if not condition:
        raise ValueError(message)


def verify(directory):
    root = Path(directory)
    config = (root / "linux.config").read_text()
    for option in ("NET", "INET", "SERIAL_8250_OMAP", "TI_CPSW_SWITCHDEV", "PHY_TI_GMII_SEL", "AT803X_PHY",
                   "MWIFIEX_SDIO", "MMC_SDHCI_OMAP", "MTD_NAND_OMAP2", "MTD_NAND_OMAP_BCH",
                   "UBIFS_FS", "SND_SOC_RAUMFELD_CONNECTOR2", "SND_SOC_CS4271_I2C",
                   "SND_SOC_AK4104", "SND_SOC_DAVINCI_MCASP", "COMMON_CLK_SI5351",
                   "MFD_TPS65217", "REGULATOR_TPS65217", "EEPROM_AT24", "KEYBOARD_GPIO",
                   "LEDS_GPIO", "USB_MUSB_DSPS", "AM335X_PHY_USB", "USB_STORAGE", "AMX3_PM", "WKUP_M3_IPC"):
        require(f"CONFIG_{option}=y\n" in config or f"CONFIG_{option}=m\n" in config,
                f"Required hardware driver disabled: CONFIG_{option}")
    manifest = {"board": "connector2", "buildroot": "2025.02.18", "linux": "6.12.112",
                "hardware_boot_tested": False, "nand_install_tested": False, "files": {}}
    with tarfile.open(root / "rootfs.tar.gz") as archive:
        members = {member.name.removeprefix("./"): member for member in archive.getmembers()}
        require(("root/.ssh/authorized_keys" in members) == bool(os.environ.get("GALDRUN_SSH_PUBKEY")),
                "An SSH key may only ship when GALDRUN_SSH_PUBKEY asks for it")
        require(not any(Path(name).name == "id_ed25519" for name in members), "Private SSH key included in firmware")
        for module in ("raumfeld-connector2", "snd-soc-cs4271-i2c", "snd-soc-ak4104", "snd-soc-davinci-mcasp", "mwifiex_sdio", "omap-rng"):
            require(any(Path(name).name.startswith(module + ".ko") for name in members), f"Module absent: {module}")
        for name in ("usr/sbin/sshd", "usr/sbin/hostapd", "usr/sbin/dnsmasq", "usr/sbin/raumfeld-network",
                     "usr/sbin/wpa_supplicant", "usr/sbin/wpa_passphrase",
                     "lib/firmware/mrvl/sd8787_uapsta.bin", "lib/firmware/am335x-pm-firmware.elf",
                     "opt/galdrun-client/galdrun-client", "opt/galdrun-client/lib/libatomic.so.1",
                     "opt/galdrun-client/client-default.json", "etc/init.d/S95galdrun-client",
                     "usr/libexec/sftp-server"):
            require(name in members, f"Required rootfs file absent: {name}")
        supplicant = archive.extractfile(members["usr/sbin/wpa_supplicant"]).read()
        require(b"-s = log output to syslog" in supplicant,
                "wpa_supplicant lacks the -s syslog option used by the network supervisor")
        for name in ("opt/galdrun-client/galdrun-client", "etc/init.d/S95galdrun-client",
                     "usr/sbin/raumfeld-status-leds", "etc/init.d/S02raumfeld-led-boot",
                     "usr/sbin/raumfeld-captive-portal",
                     "etc/init.d/S96raumfeld-status-leds"):
            require(name in members, f"Required file absent: {name}")
            require(members[name].mode & 0o111, f"Required file is not executable: {name}")
        board = json.load(archive.extractfile(members["etc/galdrun-board.json"]))
        require(board.get("statusLeds") is True, "Raumfeld status LEDs are not enabled")
        require(board.get("cpuFrequency") is True, "Raumfeld CPU selection is not enabled")
        require(board.get("dacControls") is True, "DAC settings are not enabled")
        require(archive.extractfile(members["etc/galdrun-cpu.conf"]).read() == b"CPU_MHZ=300\n",
                "Fresh firmware must default to 300 MHz")
        for name in ("usr/bin/curl", "usr/bin/rsync", "usr/bin/strace", "usr/bin/tcpdump",
                     "etc/ssl/certs/ca-certificates.crt"):
            require(name in members, f"On-demand toolkit file absent: {name}")
        for applet in ("timeout", "nc", "vmstat"):
            require(any(name.endswith("/" + applet) for name in members), f"BusyBox applet absent: {applet}")
        require(not any(name.startswith("etc/init.d/S") and "rsync" in name.lower() for name in members),
                "On-demand rsync must not have a startup service")
        ssh = archive.extractfile(members["etc/ssh/sshd_config"]).read().decode().splitlines()
        require("PasswordAuthentication yes" in ssh and "PermitRootLogin yes" in ssh and
                "PermitEmptyPasswords no" in ssh, "Password SSH configuration is absent or unsafe")
        sshd = archive.extractfile(members["etc/init.d/S50sshd"]).read().decode()
        require("grep -qx 'SSH_ENABLED=1'" in sshd and "etc/galdrun-ssh.conf" not in members,
                "SSH must stay off until the settings page turns it on")
        require(members["usr/sbin/raumfeld-ssh"].mode & 0o111 and members["etc/init.d/S50sshd"].mode & 0o111,
                "SSH switch scripts are not executable")
        require(board.get("sshSettings") is True, "The settings page has no SSH switch")
        shadow = archive.extractfile(members["etc/shadow"]).read().decode().splitlines()
        root_password = next(line.split(":")[1] for line in shadow if line.startswith("root:"))
        require(root_password == "", "The firmware must not ship a root password")
        require("usr/share/licenses/manifest.csv" in members and
                "usr/share/licenses/am335x-pm-firmware/TI-LICENSE.txt" in members and
                "usr/share/licenses/toolchain-external-bootlin/glibc-LGPL-2.1.txt" in members, "License texts absent")
        require(members["var/log"].issym() and members["var/log"].linkname == "../run/log",
                "System logs must point to the dedicated RAM filesystem")
        require(members["etc/init.d/S00logram"].mode & 0o111, "Log RAM initializer is not executable")
        syslog = archive.extractfile(members["etc/default/syslogd"]).read().decode()
        require("-s 256 -b 1" in syslog and "-O /dev/null -s 0" in syslog,
                "Bounded syslog or disabled-file fallback is absent")
        defaults = json.load(archive.extractfile(members["opt/galdrun-client/client-default.json"]))
        require(defaults["clientId"] == "", "Generic firmware must generate a unique client identity on first boot")
        require(defaults["server"] == "", "Generic firmware must not include this test device's server")
        require([(i["id"], i["kind"], i["device"], i["key"], i["bindings"]["click"]["action"])
                 for i in defaults["inputs"]] == [
                    ("POWER", "button", "gpio-keys", "KEY_POWER", "toggleStandby"),
                    ("SETUP", "button", "gpio-keys", "141", "none"),
                    ("RESET", "button", "gpio-keys", "KEY_F3", "none")],
                "Connector 2 button names, wiring or default power action differ")
        require(defaults["inputs"][0]["bindings"]["longPress"]["action"] == "deepSleep",
                "Connector 2 POWER long press must default to deep sleep")
        require("usr/sbin/raumfeld-suspend" in members and
                members["usr/sbin/raumfeld-suspend"].mode & 0o111,
                "Executable deep-sleep helper is absent")
        require(not any(name.startswith("etc/ssh/ssh_host_") for name in members),
                "Generic firmware must generate fresh SSH host keys")
        require("etc/galdrun/client.json" not in members,
                "Generic firmware must not include this test device's saved room settings")
        chrony = archive.extractfile(members["etc/chrony.conf"]).read().decode().splitlines()
        require("makestep 0.001 3" in chrony and "maxslewrate 50" in chrony,
                "NTP startup stepping and audio-safe slew limit absent")
        require("driftfile /run/chrony/drift" in chrony, "Chrony runtime drift must not write NAND")
        client = archive.extractfile(members["opt/galdrun-client/galdrun-client"]).read()
        require(client[:4] == b"\x7fELF" and struct.unpack_from("<H", client, 18)[0] == 40,
                "Bundled client is not an ARM ELF binary")
        project = Path(os.environ.get("RAUMFELD_PROJECT_DIR", Path(__file__).resolve().parents[1]))
        release = project.parent.parent / "releases/client/linux-armv6/galdrun-client"
        require(client == release.read_bytes(), "Bundled client differs from the exported ARMv6 release")
        manifest["client"] = {"path": "/opt/galdrun-client/galdrun-client",
                              "sha256": hashlib.sha256(client).hexdigest(),
                              "autostart": "/etc/init.d/S95galdrun-client",
                              "config": "/etc/galdrun/client.json"}
    for name, maximum in (("uImage", 0x600000), ("dts.cramfs", 0x40000),
                          ("rootfs.ubi", 0xf820000), ("uInitrd", 64 * 1024 * 1024),
                          ("am335x-raumfeld-connector2.dtb", 0x40000)):
        path = root / name
        data = path.read_bytes()
        require(0 < len(data) <= maximum, f"{name} exceeds its size budget")
        manifest["files"][name] = {"size": len(data), "sha256": hashlib.sha256(data).hexdigest()}
        if name in ("uImage", "uInitrd"):
            magic, header_crc, _, size, load, entry, crc, os_, arch, type_, compression, _ = struct.unpack_from(">7I4B32s", data)
            header = bytearray(data[:64]); header[4:8] = bytes(4)
            require(magic == 0x27051956 and len(data) == 64 + size, f"Invalid {name} uImage")
            require(zlib.crc32(header) == header_crc and zlib.crc32(data[64:]) == crc,
                    f"{name} CRC failure")
            require(os_ == 5 and arch == 2, f"{name} is not ARM Linux")
            require(type_ == (2 if name == "uImage" else 3), f"{name} has incorrect image type")
            if name == "uImage":
                require(load == entry == 0x80008000 and compression == 0, "Incorrect NAND kernel load/entry/format")
        elif name.endswith(".dtb"):
            magic, size = struct.unpack_from(">2I", data)
            require(magic == 0xd00dfeed and size == len(data), "Invalid board DTB")
        elif name == "dts.cramfs":
            require(data[:4] == b"E=\xcd(", "Invalid cramfs magic")
            require(struct.unpack_from("<I", data, 4)[0] == len(data), "Invalid cramfs length")
            crc = struct.unpack_from("<I", data, 32)[0]
            zeroed = bytearray(data); zeroed[32:36] = bytes(4)
            require(zlib.crc32(zeroed) == crc, "Invalid cramfs CRC")
        elif name == "rootfs.ubi":
            require(len(data) % 131072 == 0, "UBI image not PEB aligned")
            for offset in range(0, len(data), 131072):
                header = data[offset:offset + 64]
                require(header[:4] == b"UBI#" and header[4] == 1, "Invalid UBI EC header")
                vid, payload = struct.unpack_from(">2I", header, 16)
                require((vid, payload) == (2048, 4096), "Wrong vendor-compatible UBI geometry")
    manifest["limitations"] = ["Boot and electrical/audio tests are required before NAND installation.",
                              "Original SPL/U-Boot/env are intentionally absent from this image set."]
    (root / "firmware-manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    print("Firmware image CRCs, geometry and partition size limits verified.")


if __name__ == "__main__":
    if len(sys.argv) != 2:
        raise SystemExit("Usage: verify-images.py IMAGES_DIR")
    verify(sys.argv[1])
