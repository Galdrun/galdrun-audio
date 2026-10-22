"""Build the Connector 2 USB installation image on the host; never flashes hardware.

Every byte is ours. The vendor U-Boot, with Reset held, loads the first 0x840000
bytes of connect2.img from a FAT32 stick and runs `usb_update_boot`: bootm of the
uImage at 0 with the DTB that `cramfsload` takes from the cramfs at 0x800000, no
initrd and no signature check. So the image is

    0x000000  installer uImage: the board kernel with its initramfs built in
    0x800000  dts.cramfs, the same DTB store the installed firmware boots with
    0x840000  the payload, a plain tar, which the installer reads from the stick

Usage: build-usb-installer.py <work directory> <jobs>
"""
from pathlib import Path
from importlib import util
import hashlib
import io
import json
import shutil
import struct
import subprocess
import sys
import tarfile
import zlib

project = Path(__file__).resolve().parents[1]
DTB_OFFSET = 0x800000
PAYLOAD_OFFSET = 0x840000


def uimage(data):
    header = struct.unpack_from(">7I4B32s", data)
    if header[0] != 0x27051956 or 64 + header[3] > len(data):
        raise ValueError("invalid uImage")
    zeroed = bytearray(data[:64])
    zeroed[4:8] = bytes(4)
    if zlib.crc32(zeroed) != header[1]:
        raise ValueError("uImage header CRC mismatch")
    payload = data[64:64 + header[3]]
    if zlib.crc32(payload) != header[6]:
        raise ValueError("uImage data CRC mismatch")
    return header, payload


def kernel_inputs():
    """What the installer kernel is built from; a change means a clean rebuild."""
    board = project / "board/connector2"
    files = [project / "external.mk", board / "linux.config", board / "installer/defconfig",
             board / "installer/linux.fragment"]
    files += [path for path in board.rglob("*")
              if path.suffix in (".c", ".h", ".dts", ".dtsi", ".patch", ".elf") and path.is_file()]
    digest = hashlib.sha256()
    for path in sorted(files):
        digest.update(path.relative_to(project).as_posix().encode() + b"\0" + path.read_bytes())
    return digest.hexdigest()


def build_installer_kernel(work, jobs):
    """The installer uImage, built by Buildroot next to the firmware build."""
    output = work / "connector2-installer"
    make = ["make", "-C", str(work / "buildroot-2025.02.18"), f"O={output}", f"BR2_EXTERNAL={work / 'external'}"]
    subprocess.run(make + [f"BR2_DEFCONFIG={work / 'external/board/connector2/installer/defconfig'}", "defconfig"],
                   check=True)
    inputs = kernel_inputs()
    stamp = output / ".raumfeld-kernel-inputs"
    if (output / "build/linux-6.12.112").is_dir() and (stamp.read_text().strip() if stamp.exists() else "") != inputs:
        subprocess.run(make + ["linux-dirclean"], check=True)
    with open(project / "logs/connector2-installer.log", "wb") as log:
        build = subprocess.run(make + [f"BR2_DL_DIR={project / 'downloads'}", f"-j{jobs}"],
                               stdout=log, stderr=subprocess.STDOUT)
        if not build.returncode:
            build = subprocess.run(make + [f"BR2_DL_DIR={project / 'downloads'}", "legal-info"],
                                   stdout=log, stderr=subprocess.STDOUT)
    if build.returncode:
        raise RuntimeError(f"Installer build failed; see {project / 'logs/connector2-installer.log'}")
    stamp.write_text(inputs + "\n")
    return (output / "images/uImage").read_bytes()


def export_licenses(work, licenses):
    """What connect2.img contains, by license: Buildroot's legal-info of the firmware and the installer."""
    shutil.rmtree(licenses, ignore_errors=True)
    for name, tree in (("firmware", "connector2"), ("installer", "connector2-installer")):
        info = work / tree / "legal-info"
        shutil.copytree(info / "licenses", licenses / name)
        shutil.copy2(info / "manifest.csv", licenses / name / "manifest.csv")
        # Both trees use the same Bootlin toolchain; the firmware's post-build collects its texts.
        shutil.copytree(work / "connector2/target/usr/share/licenses/toolchain-external-bootlin",
                        licenses / name / "toolchain-external-bootlin")
    shutil.copy2(project / "board/connector2/firmware/TI-LICENSE.txt",
                 licenses / "firmware/am335x-pm-firmware-TI-LICENSE.txt")
    (licenses / "SOURCE.txt").write_text(
        "connect2.img contains GPL and other open-source software. Its complete source is the\n"
        "Galdrun repository at the tag of this release (hardware/buildroot/README.md builds it),\n"
        "plus the package sources Buildroot downloads and checks by hash. manifest.csv in each\n"
        "folder lists every package with its version, license and source archive.\n", newline="\n")


def payload_tar(payloads):
    data = io.BytesIO()
    with tarfile.open(fileobj=data, mode="w", format=tarfile.USTAR_FORMAT) as archive:
        for name, content in payloads.items():
            member = tarfile.TarInfo(name)
            member.size = len(content)
            member.mode = 0o644
            archive.addfile(member, io.BytesIO(content))
    return data.getvalue()


def build(work, jobs):
    images = project / "output/connector2"
    spec = util.spec_from_file_location("verify_images", project / "scripts/verify-images.py")
    checker = util.module_from_spec(spec)
    spec.loader.exec_module(checker)
    checker.verify(images)

    installer = build_installer_kernel(work, jobs)
    uimage(installer)
    if len(installer) > DTB_OFFSET:
        raise ValueError(f"Installer uImage is {len(installer)} bytes; U-Boot boots at most {DTB_OFFSET}")
    dtb_store = (images / "dts.cramfs").read_bytes()
    if len(dtb_store) > PAYLOAD_OFFSET - DTB_OFFSET:
        raise ValueError("DTB store exceeds its 256 KiB slot")

    payloads = {
        "rootfs.tgz": (images / "rootfs.tar.gz").read_bytes(),
        "kernel.uImage": (images / "uImage").read_bytes(),
        "dts.cramfs": dtb_store,
    }
    with tarfile.open(images / "rootfs.tar.gz") as archive:
        files = []
        unpacked = 0
        for member in archive:
            name = member.name.removeprefix("./")
            if name.startswith("/") or ".." in Path(name).parts or "\n" in name or "\\" in name:
                raise ValueError(f"Unsafe archive member: {name}")
            if member.isfile():
                content = archive.extractfile(member).read()
                unpacked += (len(content) + 4095) // 4096 * 4096
                files.append(f"{hashlib.md5(content).hexdigest()}  {name}\n")
                if name == "boot/uImage" and content != payloads["kernel.uImage"]:
                    raise ValueError("Archive kernel differs from NAND kernel payload")
        if not any(line.endswith("  boot/uImage\n") for line in files):
            raise ValueError("Archive has no boot/uImage")
        if unpacked > 160 * 1024 * 1024:
            raise ValueError("Root filesystem exceeds conservative 160 MiB budget for 191 MiB volume")
        payloads["rootfs-files.md5"] = "".join(files).encode()
    payloads["payload.md5"] = "".join(
        f"{hashlib.md5(payloads[name]).hexdigest()}  {name}\n"
        for name in ("rootfs.tgz", "kernel.uImage", "dts.cramfs", "rootfs-files.md5")
    ).encode()

    image = (installer.ljust(DTB_OFFSET, b"\0") + dtb_store.ljust(PAYLOAD_OFFSET - DTB_OFFSET, b"\0")
             + payload_tar(payloads))
    destination = images / "usb-install"
    destination.mkdir(parents=True, exist_ok=True)
    (destination / "connect2.img").write_bytes(image)
    manifest = {
        "purpose": "USB firmware + preinstalled client installation, NOT a non-flashing probe",
        "board": "connector2", "image_size": len(image),
        "image_sha256": hashlib.sha256(image).hexdigest(),
        "layout": {"installer_uimage": 0, "dtb_store": DTB_OFFSET, "payload_tar": PAYLOAD_OFFSET},
        "installer_uimage": {"size": len(installer), "sha256": hashlib.sha256(installer).hexdigest()},
        "vendor_code_included": False,
        "payloads": {name: {"size": len(content), "sha256": hashlib.sha256(content).hexdigest()}
                     for name, content in payloads.items()},
        "rootfs_regular_files_allocated_bytes": unpacked,
        "installer_writes_mtd": [6, 7, 8], "installer_preserves_mtd": list(range(6)),
        "reset_held_entry_saves_environment": True,
        "client_autostart": "/etc/init.d/S95galdrun-client", "client_config": "/etc/galdrun/client.json",
        "physical_install_tested": False, "flash_commands_executed": False,
    }
    (destination / "installer-manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    export_licenses(work, destination / "licenses")
    print(f"PASS: firmware/client USB installer built: {destination / 'connect2.img'}")
    print("SHA256: " + manifest["image_sha256"])


if __name__ == "__main__":
    build(Path(sys.argv[1]), int(sys.argv[2]))
