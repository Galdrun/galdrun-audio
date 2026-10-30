"""Check the actual source-built RAM payload; never contacts hardware."""
from pathlib import Path
import gzip
import hashlib
import json
import os
import struct
import subprocess
import sys
from connector3_content import audit_files


def verify(output, project):
    config = (output / "build/linux-6.12.112/.config").read_text()
    for option in ("MTD", "MMC", "ATA", "SCSI", "USB_SUPPORT", "MODULES"):
        if f"# CONFIG_{option} is not set" not in config:
            raise ValueError(f"Persistent storage access must be absent: {option}")
    for option in ("FEC", "SMSC_PHY", "PCI_IMX6_HOST", "PCIEPORTBUS", "MWIFIEX_PCIE",
                   "SND_SOC_FSL_SAI", "SND_SOC_GALDRUN_CONNECTOR3", "INET", "DEVTMPFS",
                   "CPU_FREQ", "ARM_IMX_CPUFREQ_DT", "MFD_AXP20X_I2C", "REGULATOR_AXP20X",
                   "SUSPEND"):
        if f"CONFIG_{option}=y" not in config:
            raise ValueError(f"Required built-in driver missing: {option}")
    cmdline = next(line for line in config.splitlines() if line.startswith("CONFIG_CMDLINE="))
    if "earlycon=ec_imx6q,0x30860000,115200" not in cmdline:
        raise ValueError("Early UART1 console missing")
    if "ignore_loglevel" in cmdline or "mwifiex.debug_mask=0x7" not in cmdline:
        raise ValueError("Verbose Wi-Fi logging can saturate the serial console")

    fdtget = output / "host/bin/fdtget"
    dtb = output / "images/imx7d-galdrun-connector3-ram.dtb"
    def dt_property(name):
        return subprocess.check_output([str(fdtget), '-t', 's', str(dtb), '/sound', name], text=True).strip()
    if dt_property('compatible') != 'galdrun,connector3-audio':
        raise ValueError('Connector 3 clock-switching audio driver must own the card')
    if dt_property('clock-names') != 'pll mclk sai':
        raise ValueError('Connector 3 requires the PLL and both audio root clocks')
    clock_header = (output / 'build/linux-6.12.112/include/dt-bindings/clock/imx7d-clock.h').read_text()
    clock_ids = {parts[1]: int(parts[2]) for line in clock_header.splitlines()
                 if (parts := line.split()) and len(parts) == 3 and parts[0] == '#define'
                 and parts[2].isdigit()}
    audio_clocks = subprocess.check_output(
        [str(fdtget), '-t', 'u', str(dtb), '/sound', 'clocks'], text=True).split()
    expected_clocks = [clock_ids[name] for name in (
        'IMX7D_PLL_AUDIO_MAIN', 'IMX7D_AUDIO_MCLK_ROOT_DIV', 'IMX7D_SAI1_ROOT_CLK')]
    if len(audio_clocks) != 6 or list(map(int, audio_clocks[1::2])) != expected_clocks:
        raise ValueError('Connector 3 must use registered, programmable mainline audio clocks')
    opps = subprocess.check_output([str(fdtget), '-l', str(dtb), '/opp-table'], text=True).split()
    if set(opps) != {'opp-792000000', 'opp-996000000'}:
        raise ValueError('Only the stock normal CPU speeds may be exposed')
    cpu_supply = subprocess.check_output(
        [str(fdtget), '-t', 'u', str(dtb), '/cpus/cpu@0', 'cpu-supply'], text=True).strip()
    regulator = '/soc/bus@30800000/i2c@30a50000/pmic@32/regulators/dcdc4'
    regulator_phandle = subprocess.check_output(
        [str(fdtget), '-t', 'u', str(dtb), regulator, 'phandle'], text=True).strip()
    if cpu_supply != regulator_phandle:
        raise ValueError('CPU frequency transitions require the actual AXP152 DCDC4 supply')
    reserved = subprocess.check_output(
        [str(fdtget), '-t', 'x', str(dtb), '/soc/sram@900000/suspend-reserved@0', 'reg'],
        text=True).split()
    if reserved != ['0', '4000']:
        raise ValueError('Suspend code and translation table must be reserved in OCRAM')

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
                 "usr/sbin/galdrun-connector3-mcu",
                 "etc/init.d/S45raumfeld-network"):
        if name not in files or not files[name][1]:
            raise ValueError(f"Missing RAM payload: {name}")
    if "etc/wpa_supplicant.conf" in files:
        raise ValueError("Generic image must not contain home Wi-Fi credentials")
    radio_source = os.environ.get("GALDRUN_CONNECTOR3_WIFI_FIRMWARE")
    expected_radio = Path(radio_source) if radio_source else output / "build/linux-firmware-20250211/mrvl/pcie8897_uapsta.bin"
    radio_hash = hashlib.sha256(files["lib/firmware/mrvl/pcie8897_uapsta.bin"][1]).hexdigest()
    if radio_hash != hashlib.sha256(expected_radio.read_bytes()).hexdigest():
        raise ValueError("Archived Wi-Fi firmware differs from the selected source")
    network = files["etc/galdrun-network.conf"][1].decode().splitlines()
    if "WIFI_SSID=" not in network or "WIFI_PASSWORD=" not in network:
        raise ValueError("Generic network settings must not contain home credentials")
    board = json.loads(files["etc/galdrun-board.json"][1])
    if board.get("networkSettings") is not True:
        raise ValueError("Network settings capability missing")
    for capability in ("dacControls", "selectedCardDacControls", "mcuAudioControls", "cpuFrequency"):
        if board.get(capability) is not True:
            raise ValueError(f"MCU audio capability missing: {capability}")
    if board.get("hardwareClockControl") != "Drift compensator":
        raise ValueError('Stock-sign PLL trim must be explicitly opted in')
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
    content_audit = (audit_files(files, output / "legal-info/manifest.csv") if not radio_source else
                     {"eligible_for_clean_installer": False,
                      "reason": "private device-extracted radio firmware; diagnostic use only"})
    result = {"storage_drivers_absent": True, "network_drivers_built_in": True,
              "content_audit": content_audit,
              "wifi_and_sdma_firmware_present": True, "home_wifi_credentials_present": False,
              "wifi_firmware_source": "local-device" if radio_source else "linux-firmware",
              "wifi_firmware_sha256": radio_hash,
              "audio_clock_families": [44100, 48000],
              "hardware_clock_control": "Drift compensator", "clock_trim_range_ppm": [-500, 500],
              "hardware_volume": "MCU Output Level (0..100), mute",
              "suspend_port": "opt-in CPU-off/DDR self-refresh; peripheral domain retained",
              "cpu_frequencies_mhz": [792, 996], "cpu_supply": "AXP152 DCDC4",
              "deep_sleep_qualified": False, "wake_on_lan_qualified": False,
              "key_only_ssh": True, "arm_client_matches_release": True,
              "static_kexec": True, "uncompressed_initramfs_bytes": len(archive),
              "hardware_boot_verified": False, "audio_verified": False}
    (output / "images/ram-verification.json").write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    verify(Path(sys.argv[1]), Path(sys.argv[2]))
