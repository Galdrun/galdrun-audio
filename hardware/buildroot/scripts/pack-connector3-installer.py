"""Assemble a signed installer FIT with an authenticated appended USB payload."""
from pathlib import Path
import gzip
import hashlib
import json
import os
import shutil
import subprocess
import sys
from connector3_content import audit_files
from connector3_fit import sign_fit


def pack(output, storage, diagnostic, signing, destination, mode):
    project = Path(__file__).resolve().parents[1]
    if mode not in ('check', 'write'):
        raise ValueError('Installer mode must be check or write')
    qualification = project / 'output/connector3-monitor-production/hardware-verification.json'
    monitor_qualified = False
    if qualification.is_file():
        monitor_hardware = json.loads(qualification.read_text())
        monitor_qualified = (monitor_hardware['production_qualified'] and
            hashlib.sha256((qualification.parent / 'tee-raw.bin').read_bytes()).hexdigest() == monitor_hardware['sha256'])
    if mode == 'write':
        if not monitor_qualified:
            raise ValueError('Write-capable USB images require a hardware-qualified production monitor')
        persistent = json.loads((project / 'output/connector3-persistent/persistent-verification.json').read_text())
        if not persistent['monitor_production_qualified'] or persistent['monitor_sha256'] != monitor_hardware['sha256']:
            raise ValueError('Persistent FIT must use the hardware-qualified production monitor')
    qualified = json.loads((project / 'output/connector3-storage-check/hardware-verification.json').read_text())
    if not qualified['cold_boot_verified']:
        raise ValueError('Qualified storage cold boot required')
    source = next(path for path in storage.glob('linux-*') if (path / 'arch/arm/boot/zImage').is_file()
                  and hashlib.sha256((path / 'arch/arm/boot/zImage').read_bytes()).hexdigest() == qualified['kernel_sha256'])
    linux = output / ('linux-' + qualified['kernel_sha256'][:16])
    if not (linux / 'Makefile').exists():
        subprocess.run(['cp', '-a', '--reflink=auto', str(source), str(linux)], check=True)
    shutil.copyfile(source / '.config', linux / '.config')
    board = project / 'board/connector3'
    dts = linux / 'arch/arm/boot/dts/nxp/imx'
    shutil.copyfile(board / 'imx7d-galdrun-connector3-storage.dtsi', dts / 'imx7d-galdrun-connector3-storage.dtsi')
    shutil.copyfile(board / 'storage-check/imx7d-galdrun-connector3-storage-check.dts', dts / 'imx7d-galdrun-connector3-storage-check.dts')
    tree = '#include "imx7d-galdrun-connector3-storage-check.dts"\n'
    tree += f'/ {{ model = "Galdrun Connector 3 installer {mode}"; }};\n'
    # The front-panel MCU's UART (as in the NAND image), for the progress lights.
    tree += ('&uart3 { pinctrl-names = "default"; pinctrl-0 = <&pinctrl_uart3>; '
             'assigned-clocks = <&clks IMX7D_UART3_ROOT_SRC>; '
             'assigned-clock-parents = <&clks IMX7D_OSC_24M_CLK>; status = "okay"; };\n'
             '&iomuxc { pinctrl_uart3: uart3grp { fsl,pins = < '
             'MX7D_PAD_UART3_RX_DATA__UART3_DCE_RX 0x60 '
             'MX7D_PAD_UART3_TX_DATA__UART3_DCE_TX 0x60 >; }; };\n')
    if mode == 'write':
        tree += '&gpmi { /delete-property/ galdrun,read-only; partitions { partition@8900000 { /delete-property/ read-only; }; partition@9500000 { /delete-property/ read-only; }; }; };\n'
    (dts / 'imx7d-galdrun-connector3-installer.dts').write_text(tree)
    subprocess.run([str(linux / 'scripts/config'), '--file', str(linux / '.config'),
                    '--set-str', 'INITRAMFS_SOURCE', str(output / 'images/rootfs.cpio'),
                    '--set-str', 'LOCALVERSION', '-galdrun-installer-' + mode,
                    '--disable', 'MTD_PARTITIONED_MASTER', '--disable', 'MTD_CMDLINE_PARTS'], check=True)
    args = ['make', '-C', str(linux), 'ARCH=arm', 'CROSS_COMPILE=' + str(diagnostic / 'host/bin/arm-linux-')]
    log = project / ('logs/connector3-installer-' + mode + '-kernel.log')
    with log.open('w') as stream:
        subprocess.run([*args, 'olddefconfig'], stdout=stream, stderr=subprocess.STDOUT, check=True)
        subprocess.run([*args, '-j' + os.environ.get('JOBS', '16'), 'zImage',
                        'nxp/imx/imx7d-galdrun-connector3-installer.dtb'], stdout=stream, stderr=subprocess.STDOUT, check=True)
    # Audit the actual initramfs, not just Buildroot's target staging tree.
    archive = (output / 'images/rootfs.cpio').read_bytes()
    files, offset = {}, 0
    while archive[offset:offset + 6] == b'070701':
        fields = [int(archive[offset + 6 + i * 8:offset + 14 + i * 8], 16) for i in range(13)]
        size, namesize = fields[6], fields[11]
        name = archive[offset + 110:offset + 110 + namesize - 1].decode().removeprefix('./')
        offset = (offset + 110 + namesize + 3) & ~3
        files[name] = (fields[1], archive[offset:offset + size])
        offset = (offset + size + 3) & ~3
        if name == 'TRAILER!!!': break
    else:
        raise ValueError('Incomplete installer CPIO')
    audit = audit_files(files, output / 'legal-info/manifest.csv')
    for installed, source_name in (('init', 'init.sh'), ('usr/sbin/galdrun-install', 'install-usb.sh'),
                                   ('usr/sbin/galdrun-update-fit', 'update-fit.sh')):
        if files[installed][1] != (board / 'installer' / source_name).read_bytes():
            raise ValueError('Installer archive contains stale source: ' + installed)
    for tool in ('nanddump', 'nandwrite', 'flash_erase', 'ubiformat', 'ubiattach', 'ubidetach',
                 'galdrun-install', 'galdrun-update-fit'):
        data = files['usr/sbin/' + tool][1]
        if not data: raise ValueError('Empty installer tool: ' + tool)
    for forbidden in ('fw_setenv', 'flash_otp_write', 'flash_otp_erase', 'nandtest'):
        if 'usr/sbin/' + forbidden in files: raise ValueError('Unexpected destructive tool: ' + forbidden)
    if ('etc/galdrun-install-writes-enabled' in files) != (mode == 'write'):
        raise ValueError('Installer write marker differs from kernel profile')
    if b'PRIVATE KEY' in files['root/.ssh/authorized_keys'][1]:
        raise ValueError('SSH private key in installer')
    for name in ('payload.tar.sha256', 'payload.sha256', 'volume-readback'):
        target = {'payload.tar.sha256': 'galdrun-payload.sha256', 'payload.sha256': 'galdrun-install.sha256',
                  'volume-readback': 'galdrun-volume-readback'}[name]
        if files['etc/' + target][1] != (output / 'inputs' / name).read_bytes():
            raise ValueError('Signed installer payload manifest mismatch')
    dtb = dts / 'imx7d-galdrun-connector3-installer.dtb'
    get = diagnostic / 'host/bin/fdtget'
    controller = '/soc/nand-controller@33002000'
    def exists(node, name):
        return subprocess.run([str(get), str(dtb), node, name], capture_output=True).returncode == 0
    if exists(controller, 'galdrun,read-only') != (mode == 'check'):
        raise ValueError('Controller guard differs from installer mode')
    for number, address in enumerate(('0', '800000', '880000', '900000', '8900000', '9500000')):
        if exists(controller + '/partitions/partition@' + address, 'read-only') != (mode == 'check' or number < 4):
            raise ValueError('Incorrect installer partition protection')
    monitor = project / 'output/connector3-monitor-production/tee-raw.bin'
    monitor_report = json.loads((project / 'output/connector3-monitor-production/monitor-verification.json').read_text())
    if hashlib.sha256(monitor.read_bytes()).hexdigest() != monitor_report['sha256']:
        raise ValueError('Monitor candidate changed')
    fit, report = sign_fit(linux / 'arch/arm/boot/zImage', dtb, diagnostic, signing,
                           'installer ' + mode, monitor)
    payload = (output / 'inputs/payload.tar').read_bytes()
    image = fit + bytes(0xC00000 - len(fit)) + payload
    hardware = destination / 'hardware-verification.json'
    if hardware.exists():
        hardware.replace(destination / 'hardware-verification.previous.json')
    (destination / 'connect3.img').write_bytes(image)
    (destination / 'connect3.img.sha256').write_text(hashlib.sha256(image).hexdigest() + '  connect3.img\n')
    report.update({'purpose': 'USB installer ' + mode, 'mode': mode, 'content_audit': audit,
                   'monitor_production_qualified': monitor_qualified,
                   'factory_protected_mtd': [0, 1, 2, 3], 'all_nand_read_only': mode == 'check',
                   'payload_sha256': hashlib.sha256(payload).hexdigest(),
                   'payload_hash_covered_by_fit_signature': True, 'usb_sha256': hashlib.sha256(image).hexdigest(),
                   'hardware_verified': False, 'persistent_install_verified': False})
    (destination / 'installer-verification.json').write_text(json.dumps(report, indent=2) + '\n')
    shutil.copyfile(dtb, destination / 'connector3.dtb')
    shutil.copyfile(linux / 'arch/arm/boot/zImage', destination / 'zImage')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    pack(*(Path(value).resolve() for value in sys.argv[1:6]), sys.argv[6])
