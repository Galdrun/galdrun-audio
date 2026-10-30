"""Host checks for the read-only NAND qualification kernel. No device access."""
from pathlib import Path
import hashlib
import json
import subprocess
import sys


output, diagnostic, linux = map(Path, sys.argv[1:])
config = (linux / ".config").read_text()
for option in ("MTD", "MTD_RAW_NAND", "MTD_NAND_GPMI_NAND", "MTD_UBI", "UBIFS_FS", "FEC",
               "USB_CHIPIDEA_HOST", "NOP_USB_XCEIV", "USB_STORAGE", "SCSI", "BLK_DEV_SD", "VFAT_FS"):
    if f"CONFIG_{option}=y" not in config.splitlines():
        raise ValueError(f"Storage qualification driver missing: {option}")
for option in ("MODULES", "MTD_BLOCK", "MMC", "ATA"):
    if f"# CONFIG_{option} is not set" not in config.splitlines():
        raise ValueError(f"Unneeded storage route must stay disabled: {option}")
if 'CONFIG_LOCALVERSION="-galdrun-storage-ro"' not in config.splitlines():
    raise ValueError("Storage test needs its distinct kernel release")
cmdline = next(line for line in config.splitlines() if line.startswith("CONFIG_CMDLINE="))
if "ubi.mtd=" in cmdline or "CONFIG_CMDLINE_FORCE=y" not in config.splitlines():
    raise ValueError("Storage qualification must not inherit automatic NAND/UBI boot arguments")
source = (linux / "drivers/mtd/nand/raw/gpmi-nand/gpmi-nand.c").read_text()
guard_start = source.index('if (of_property_read_bool(this->dev->of_node, "galdrun,read-only"))')
guard_end = source.index("this->ntransfers = 0;", guard_start)
guard = source[guard_start:guard_end]
for command in ("SEQIN", "RNDIN", "PAGEPROG", "CACHEDPROG", "ERASE1", "ERASE2"):
    if f"case NAND_CMD_{command}:" not in guard:
        raise ValueError(f"NAND write guard missing command: {command}")
if "return -EROFS;" not in guard:
    raise ValueError("NAND write commands must fail before hardware execution")
dtb = linux / "arch/arm/boot/dts/nxp/imx/imx7d-galdrun-connector3-storage-check.dtb"
get = str(diagnostic / "host/bin/fdtget")


def prop(node, name, kind="s"):
    return subprocess.run([get, "-t", kind, str(dtb), node, name],
                          check=True, capture_output=True, text=True).stdout.strip()


controller = "/soc/nand-controller@33002000"
usb = "/soc/bus@30800000/usb@30b10000"
if prop(usb, "status") != "okay" or prop(usb, "dr_mode") != "host":
    raise ValueError("USB stick must use the original board's USB OTG1 host port")
prop(controller, "galdrun,read-only")
prop(controller, "fsl,use-minimum-ecc")
if prop(controller, "status") != "okay":
    raise ValueError("NAND controller is not enabled")
partitions = controller + "/partitions"
expected = [("0", "u-boot", 0, 0x800000), ("800000", "u-boot-env", 0x800000, 0x80000),
            ("880000", "constants", 0x880000, 0x80000), ("900000", "settings", 0x900000, 0x8000000),
            ("8900000", "fit", 0x8900000, 0xc00000), ("9500000", "rootfs", 0x9500000, 0x16b00000)]
for address, label, offset, size in expected:
    node = partitions + "/partition@" + address
    prop(node, "read-only")
    if prop(node, "label") != label or list(map(int, prop(node, "reg", "u").split())) != [offset, size]:
        raise ValueError(f"Original NAND partition layout mismatch: {label}")
digest = lambda path: hashlib.sha256(path.read_bytes()).hexdigest()
if digest(output / "rootfs.cpio") != digest(diagnostic / "images/rootfs.cpio"):
    raise ValueError("Storage test must reuse the exact audited diagnostic userspace")
report = {"purpose": "read-only NAND RAM qualification; not an installer",
          "controller_program_erase_guard": True, "all_partitions_read_only": True,
          "ubi_auto_attach": False, "audited_diagnostic_userspace_reused": True,
          "usb_host_and_vfat_enabled": True,
          "rootfs_cpio_sha256": digest(output / "rootfs.cpio"),
          "kernel_sha256": digest(linux / "arch/arm/boot/zImage"), "dtb_sha256": digest(dtb),
          "hardware_verified": False}
(output / "storage-verification.json").write_text(json.dumps(report, indent=2) + "\n")
print(json.dumps(report))
