"""Verify and sign the normal Connector 3 NAND-boot image; no device access."""
from pathlib import Path
import hashlib
import json
import shutil
import subprocess
import sys
from connector3_fit import sign_fit
from connector3_content import DEVICE_RADIO_SHA256


def verify(linux, diagnostic, signing, destination, rootfs):
    config = (linux / '.config').read_text().splitlines()
    for option in ('MTD', 'MTD_RAW_NAND', 'MTD_NAND_GPMI_NAND', 'MTD_UBI', 'UBIFS_FS',
                   'UBIFS_FS_ZLIB', 'FEC', 'MWIFIEX_PCIE', 'SND_SOC_GALDRUN_CONNECTOR3',
                   'USB_CHIPIDEA_HOST', 'USB_STORAGE', 'VFAT_FS', 'CMDLINE_FORCE', 'IMX_SDMA',
                   'SUSPEND', 'CPU_PM', 'ARM_PSCI_FW'):
        if f'CONFIG_{option}=y' not in config:
            raise ValueError(f'Required built-in driver/setting missing: {option}')
    for option in ('MODULES', 'MMC', 'ATA', 'MTD_BLOCK', 'MTD_PARTITIONED_MASTER', 'MTD_CMDLINE_PARTS'):
        if f'# CONFIG_{option} is not set' not in config:
            raise ValueError(f'Unneeded storage route enabled: {option}')
    if 'CONFIG_INITRAMFS_SOURCE=""' not in config:
        raise ValueError('Normal NAND boot must not embed a RAM root filesystem')
    firmware_names = ('mrvl/pcie8897_uapsta.bin', 'regulatory.db', 'regulatory.db.p7s', 'imx/sdma/sdma-imx7d.bin')
    if 'CONFIG_EXTRA_FIRMWARE="' + ' '.join(firmware_names) + '"' not in config:
        raise ValueError('Built-in WLAN and audio DMA need their firmware before the NAND root mounts')
    firmware_dir = json.loads(next(line.split('=', 1)[1] for line in config
                                  if line.startswith('CONFIG_EXTRA_FIRMWARE_DIR=')))
    embedded = {name: (Path(firmware_dir) / name).read_bytes() for name in firmware_names}
    vmlinux = (linux / 'vmlinux').read_bytes()
    if any(data not in vmlinux for data in embedded.values()):
        raise ValueError('Normal kernel must embed the required firmware')
    if hashlib.sha256(embedded[firmware_names[0]]).hexdigest() != DEVICE_RADIO_SHA256:
        raise ValueError('Normal kernel must embed the exact approved radio firmware')
    cmdline = json.loads(next(line.split('=', 1)[1] for line in config if line.startswith('CONFIG_CMDLINE=')))
    tokens = cmdline.split()
    for token in ('ubi.mtd=5,4096', 'root=ubi0:RootFS', 'rootfstype=ubifs', 'rw', 'init=/init', 'maxcpus=1'):
        if tokens.count(token) != 1:
            raise ValueError(f'Persistent boot argument missing/duplicated: {token}')
    # Writable like Connector 2: the client updates itself in /opt/galdrun-client.
    if 'ro' in tokens or sum(token.startswith('ubi.mtd=') for token in tokens) != 1:
        raise ValueError('Only the intended rootfs may be attached, with a writable root mount')
    dtb = linux / 'arch/arm/boot/dts/nxp/imx/imx7d-galdrun-connector3.dtb'
    get = diagnostic / 'host/bin/fdtget'

    def prop(node, name, kind='s', required=True):
        result = subprocess.run([str(get), '-t', kind, str(dtb), node, name], capture_output=True, text=True)
        if required:
            result.check_returncode()
        return result.stdout.strip() if result.returncode == 0 else None

    pcie = '/soc/pcie@33800000'
    if prop(pcie, 'clock-names') != 'pcie pcie_bus pcie_phy wifi-32k':
        raise ValueError('Radio sleep-clock consumer missing')
    if prop(pcie, 'assigned-clock-rates', 'u') != '0 0 0 32768':
        raise ValueError('Radio sleep clock must run at 32768 Hz')

    if prop('/sound', 'clock-names') != 'pll mclk sai sai2':
        raise ValueError('Analog and TOSLINK outputs must share the audio PLL clocks')
    if prop('/soc/bus@30800000/spba-bus@30800000/sai@308b0000', 'status') != 'okay':
        raise ValueError('TOSLINK output needs SAI2')
    controller = '/soc/nand-controller@33002000'
    if prop(controller, 'status') != 'okay' or prop(controller, 'galdrun,read-only', required=False) is not None:
        raise ValueError('Persistent rootfs needs the NAND controller without the diagnostic blanket write guard')
    prop(controller, 'fsl,use-minimum-ecc')
    expected = [('0', 'u-boot', 0, 0x800000), ('800000', 'u-boot-env', 0x800000, 0x80000),
                ('880000', 'constants', 0x880000, 0x80000), ('900000', 'settings', 0x900000, 0x8000000),
                ('8900000', 'fit', 0x8900000, 0xc00000), ('9500000', 'rootfs', 0x9500000, 0x16b00000)]
    for number, (address, name, offset, size) in enumerate(expected):
        node = controller + '/partitions/partition@' + address
        if prop(node, 'label') != name or list(map(int, prop(node, 'reg', 'u').split())) != [offset, size]:
            raise ValueError(f'Partition layout differs from the qualified device: {name}')
        readonly = prop(node, 'read-only', required=False) is not None
        if readonly != (number < 5):
            raise ValueError(f'Partition protection differs from normal-boot policy: {name}')
    if prop('/', 'galdrun,experimental-suspend', required=False) is not None:
        raise ValueError('Experimental deep sleep must stay disabled')
    prop('/', 'galdrun,cpu-core-suspend')
    if prop('/psci', 'compatible') != 'arm,psci-0.2' or prop('/psci', 'method') != 'smc':
        raise ValueError('Deep sleep requires the qualified PSCI monitor interface')
    for number in (0, 1):
        if prop(f'/cpus/cpu@{number}', 'enable-method') != 'psci':
            raise ValueError('CPU power control must use PSCI')
    if prop('/cpus/cpu@1', 'status') != 'okay':
        raise ValueError('Offline CPU1 must be present so the PM port can park it')
    if 'Its 88W8897 survives device host-sleep' in (linux/'drivers/pci/controller/dwc/pci-imx6.c').read_text():
        raise ValueError('Retired PCIe retention bypass prevents WLAN recovery')
    if 'Drain dump/reset work before invalidating' not in (linux/'drivers/net/wireless/marvell/mwifiex/pcie.c').read_text():
        raise ValueError('WLAN removal must drain recovery work before clearing its adapter')
    subprocess.run([sys.executable, str(Path(__file__).with_name('verify-connector3-diagnostic.py')),
                    str(diagnostic)], check=True)
    root_report = json.loads((rootfs / 'persistent-rootfs-verification.json').read_text())
    helper = Path(__file__).resolve().parents[1]/'board/connector3/raumfeld-suspend'
    if root_report.get('suspend_helper_sha256') != hashlib.sha256(helper.read_bytes()).hexdigest():
        raise ValueError('Root filesystem must contain the current sleep helper')
    firmware_hashes = {name: hashlib.sha256(data).hexdigest() for name, data in embedded.items()}
    for name, digest in firmware_hashes.items():
        if root_report['content_audit']['hardware_firmware_sha256'].get('lib/firmware/' + name) != digest:
            raise ValueError('Built-in firmware differs from the audited root filesystem: ' + name)
    for name, digest in root_report['artifacts_sha256'].items():
        if hashlib.sha256((rootfs / name).read_bytes()).hexdigest() != digest:
            raise ValueError(f'Persistent filesystem artifact changed after verification: {name}')
    monitor_path = None
    monitor_qualified = False
    monitor = Path(__file__).resolve().parents[1] / 'output/connector3-monitor-production'
    if (monitor / 'hardware-verification.json').exists():
        qualified = json.loads((monitor / 'hardware-verification.json').read_text())
        monitor_path = monitor / 'tee-raw.bin'
        if not qualified['production_qualified'] or hashlib.sha256(monitor_path.read_bytes()).hexdigest() != qualified['sha256']:
            raise ValueError('Production monitor differs from the qualified binary')
        monitor_qualified = True
    if not monitor_qualified or not qualified.get('cpu_suspend_timer_frequency_restored'):
        raise ValueError('Deep sleep requires the hardware-qualified timer-fixed monitor')
    image, report = sign_fit(linux / 'arch/arm/boot/zImage', dtb, diagnostic, signing, 'persistent boot', monitor_path)
    report['monitor_production_qualified'] = monitor_qualified
    destination.mkdir(parents=True, exist_ok=True)
    (destination / 'galdrun.itb').write_bytes(image)
    shutil.copyfile(linux / 'arch/arm/boot/zImage', destination / 'zImage')
    shutil.copyfile(dtb, destination / 'connector3.dtb')
    shutil.copyfile(rootfs / 'rootfs.ubi', destination / 'rootfs.ubi')
    report.update({'purpose': 'normal NAND-boot candidate with release defaults; not a USB installer',
                   'protected_mtd': [0, 1, 2, 3, 4], 'writable_mtd': [5],
                   'root_mount_read_only': True, 'rootfs': root_report,
                   'built_in_radio_sha256': DEVICE_RADIO_SHA256, 'wifi_sleep_clock_hz': 32768,
                   'built_in_firmware_sha256': firmware_hashes,
                   'cpu_core_deep_sleep': True, 'ethernet_magic_wake': True,
                   'mcu_touch_wake': True, 'wlan_magic_wake': False,
                   'wlan_restart_on_resume': True,
                   'hardware_verified': False, 'persistent_install_verified': False, 'release_ready': False})
    (destination / 'persistent-verification.json').write_text(json.dumps(report, indent=2) + '\n')
    names = ('galdrun.itb', 'zImage', 'connector3.dtb', 'rootfs.ubi')
    (destination / 'SHA256SUMS').write_text(''.join(
        hashlib.sha256((destination / name).read_bytes()).hexdigest() + '  ' + name + '\n' for name in names))
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    verify(*map(lambda value: Path(value).resolve(), sys.argv[1:]))
