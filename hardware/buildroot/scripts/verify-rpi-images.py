"""Validate an SD image and its installed appliance, then write its release manifest."""
from pathlib import Path
import hashlib
import json
import struct
import subprocess
import sys


def require(condition, message):
    if not condition:
        raise ValueError(message)


def sha256(path):
    with path.open('rb') as stream:
        digest = hashlib.sha256()
        for chunk in iter(lambda: stream.read(1024 * 1024), b''):
            digest.update(chunk)
        return digest.hexdigest()


def verify(images, board, target):
    require(board in ('rpi1', 'rpi3', 'rpi4', 'rpi5'), 'Unknown Pi board')
    modern = board in ('rpi4', 'rpi5')
    require((target / 'usr/sbin/galdrun-restart').stat().st_mode & 0o111, 'Restart helper is not executable')
    require('audio-profile-scanned.txt' in (target / 'etc/init.d/S41galdrun-audio-profile').read_text(),
            'Missing profile-change audio rescan on boot')
    image = images / 'sdcard.img'
    with image.open('rb') as stream:
        mbr = stream.read(512)
        require(mbr[510:] == b'\x55\xaa', 'Missing DOS partition table')
        partitions = []
        for index, expected in enumerate((0x0c, 0x83)):
            flag, _, kind, _, start, sectors = struct.unpack_from('<B3sB3sII', mbr, 446 + 16 * index)
            require(kind == expected and sectors > 0, 'Wrong FAT/ext4 partition layout')
            require(flag == (0x80 if index == 0 else 0), 'Wrong boot partition flag')
            require(start * 512 >= (1024 * 1024 if index == 0 else partitions[0]['end']), 'Overlapping partitions')
            end = (start + sectors) * 512
            require(end <= image.stat().st_size, 'Partition exceeds SD image')
            partitions.append(dict(type=kind, startSector=start, sectors=sectors, end=end))
        stream.seek(partitions[1]['startSector'] * 512 + 1024 + 56)
        require(stream.read(2) == b'\x53\xef', 'Root partition is not ext4')
    boot = images / 'rpi-boot'
    # Firmware customises the B/B+ DTBs for Pi 1 A/A+ and Pi 3 A+.
    dtbs = (['bcm2708-rpi-b-rev1', 'bcm2708-rpi-b', 'bcm2708-rpi-b-plus',
             'bcm2708-rpi-zero', 'bcm2708-rpi-zero-w'] if board == 'rpi1'
            else ['bcm2711-rpi-4-b'] if board == 'rpi4'
            else ['bcm2712-rpi-5-b', 'bcm2712d0-rpi-5-b', 'bcm2712-d-rpi-5-b'] if board == 'rpi5'
            else ['bcm2709-rpi-2-b', 'bcm2710-rpi-2-b', 'bcm2710-rpi-3-b', 'bcm2710-rpi-3-b-plus',
                  'bcm2710-rpi-zero-2-w', 'bcm2710-rpi-zero-2'])
    profiles = json.loads((target / 'usr/share/galdrun-audio-profiles.json').read_text())
    overlays = ([] if board == 'rpi5' else ['disable-bt']) + ([f'vc4-kms-v3d-pi{board[-1]}'] if modern else []) + [p['overlay'] for p in profiles if p['overlay']]
    firmware_files = [] if board == 'rpi5' else ['start4.elf', 'fixup4.dat'] if board == 'rpi4' else ['bootcode.bin', 'start.elf', 'fixup.dat']
    kernel_image = 'Image' if modern else 'zImage'
    required = ['config.txt', 'cmdline.txt', 'galdrun-audio.txt', kernel_image] + firmware_files
    required += [name + '.dtb' for name in dtbs] + ['overlays/' + name + '.dtbo' for name in overlays]
    for name in required:
        path = boot / name
        require(path.is_file() and path.stat().st_size > 0, 'Missing boot file: ' + name)
        fat = subprocess.check_output(['mtype', '-i', str(image) + '@@1048576', '::' + name])
        require(hashlib.sha256(fat).hexdigest() == sha256(path), 'FAT content mismatch: ' + name)
        if name.endswith(('.dtb', '.dtbo')):
            require(path.read_bytes()[:4] == b'\xd0\x0d\xfe\xed', 'Invalid device tree: ' + name)
    config = (boot / 'config.txt').read_text()
    require(f'kernel={kernel_image}\n' in config and 'enable_uart=1\n' in config, 'Missing kernel/serial configuration')
    require((board == 'rpi5' or 'gpu_mem=32\n' in config) and 'dtoverlay=\n' in config, 'Wrong firmware memory/automatic HAT policy')
    require('include galdrun-audio.txt\n' in config, 'Missing managed audio profile')
    bt_config = ('dtoverlay=disable-bt\ndtoverlay=vc4-kms-v3d-pi4,cma-64\n' if board == 'rpi4'
                 else 'dtoverlay=vc4-kms-v3d-pi5\n' if board == 'rpi5'
                 else '[pi0w]\ndtoverlay=disable-bt\n[all]\n' if board == 'rpi1'
                 else '[pi3]\ndtoverlay=disable-bt\n[pi02]\ndtoverlay=disable-bt\n[all]\n')
    require(config.endswith(bt_config) and config.count('dtoverlay=disable-bt\n') == (0 if board == 'rpi5' else 2 if board == 'rpi3' else 1),
            'Bluetooth overlay must only target wireless Pi models, retaining PL011')
    require('root=/dev/mmcblk0p2' in (boot / 'cmdline.txt').read_text(), 'Wrong root partition')
    if modern:
        require('arm_64bit=1\n' in config, 'Missing ARM64 boot mode')
        require('console=' + ('ttyAMA10' if board == 'rpi5' else 'ttyAMA0') + ',115200' in
                (boot / 'cmdline.txt').read_text(), 'Wrong board serial console')
        require(board == 'rpi5' or ('start_file=start4.elf\n' in config and 'fixup_file=fixup4.dat\n' in config),
                'Wrong Pi 4 boot firmware selection')
    require('CONFIG_SND_BCM2835=y\n' in (images / 'linux.config').read_text(),
            'Onboard audio must register before the initial client scan')
    modules = list((target / 'lib/modules').glob('6.12.112*/modules.dep'))
    require(len(modules) == 1, 'Missing/ambiguous Linux 6.12.112 module dependencies')
    dependencies = modules[0].read_text().replace('-', '_')
    require((target / 'usr/lib/liblzma.so.5').exists(), 'Missing XZ runtime for compressed module autoloading')
    require('raumfeld' not in dependencies and 'davinci_mcasp' not in dependencies, 'Unexpected Connector audio modules')
    required_modules = ['snd_usb_audio', 'snd_soc_bcm2835_i2s', 'ath9k_htc', 'rt2800usb', 'mt7601u', 'mt76x0u', 'mt76x2u', 'rtl8xxxu', 'rtw88_8822bu', 'rtw88_8822cu', 'rtw88_8821cu', 'rtw88_8723du', 'mt7921u']
    required_modules += ['asix', 'ax88179_178a', 'r8152'] # USB Ethernet for A/A+ and Zero
    required_modules += [p['module'].replace('-', '_') for p in profiles if p.get('module')]
    for module in required_modules:
        require(module + '.ko' in dependencies, 'Missing module: ' + module)
    kernel_config = (images / 'linux.config').read_text()
    require(modern or 'CONFIG_USB_DWCOTG=y\n' in kernel_config, 'Missing built-in USB OTG/host controller')
    if modern:
        for symbol in ('CONFIG_ARM64_4K_PAGES', 'CONFIG_GPIO_SYSFS', 'CONFIG_BCMGENET' if board == 'rpi4' else 'CONFIG_MACB'):
            require(symbol + '=y\n' in kernel_config, 'Missing ARM64 setting: ' + symbol)
        for symbol in ('CONFIG_PCIE_BRCMSTB', 'CONFIG_USB_XHCI_HCD', 'CONFIG_MMC_SDHCI_BRCMSTB'):
            require(symbol + '=y\n' in kernel_config, 'Missing PCIe/USB/SD driver: ' + symbol)
        require('vc4.ko' in dependencies and 'snd_soc_hdmi_codec.ko' in dependencies, 'Missing HDMI audio drivers')
        require((target / 'etc/init.d/S42galdrun-gpio').stat().st_mode & 0o111, 'Missing stable GPIO controller discovery')
        if board == 'rpi5':
            for symbol in ('CONFIG_MFD_RP1', 'CONFIG_PINCTRL_RP1', 'CONFIG_COMMON_CLK_RP1'):
                require(symbol + '=y\n' in kernel_config, 'Missing RP1 setting: ' + symbol)
            require('designware_i2s.ko' in dependencies, 'Missing RP1 I2S driver')
    require('CONFIG_USB_NET_SMSC95XX=y\n' in kernel_config or 'smsc95xx.ko' in dependencies,
            'Missing Pi 1/2/3 USB Ethernet driver')
    require('brcmfmac.ko' in dependencies and 'CONFIG_BRCMFMAC_SDIO=y\n' in kernel_config,
            'Missing internal SDIO Wi-Fi driver')
    firmware = target / 'lib/firmware/brcm'
    if board == 'rpi1':
        wifi_files = [f'brcmfmac43430-sdio.raspberrypi,model-zero-w.{suffix}'
                      for suffix in ('bin', 'txt', 'clm_blob')]
    elif not modern:
        require('CONFIG_USB_LAN78XX=y\n' in kernel_config or 'lan78xx.ko' in dependencies,
                'Missing Pi 3 B+ LAN78xx Ethernet driver')
        wifi_files = ['brcmfmac43430-sdio.bin', 'brcmfmac43430-sdio.raspberrypi,3-model-b.txt']
        wifi_files += [f'brcmfmac43455-sdio.raspberrypi,{model}.{suffix}'
                       for model in ('3-model-b-plus', '3-model-a-plus')
                       for suffix in ('bin', 'txt', 'clm_blob')]
        # The pinned SDIO driver names Zero 2 W's 43436S/43436 firmware
        # 43430/43430b0, respectively. CLM is separate only on the latter.
        wifi_files += [f'brcmfmac{chip}-sdio.raspberrypi,model-zero-2-w.{suffix}'
                       for chip, suffixes in [('43430', ('bin', 'txt')),
                                              ('43430b0', ('bin', 'txt', 'clm_blob'))]
                       for suffix in suffixes]
    else:
        wifi_files = [f'brcmfmac43455-sdio.raspberrypi,{board[-1]}-model-b.{suffix}'
                      for suffix in ('bin', 'txt', 'clm_blob')]
    for name in wifi_files:
        require((firmware / name).is_file() and (firmware / name).stat().st_size > 0,
                'Missing Pi Wi-Fi firmware/board data: ' + name)
    for name in ('etc/init.d/S02raumfeld-led-boot', 'etc/init.d/S40raumfeld-hardware', 'etc/init.d/S96raumfeld-status-leds', 'usr/sbin/raumfeld-suspend', 'etc/asound.conf', 'root/.ssh/authorized_keys', 'etc/galdrun-ssh.conf'):
        require(not (target / name).exists(), 'Unexpected Connector service or shared credential: ' + name)
    defaults = json.loads((target / 'opt/galdrun-client/client-default.json').read_text())
    require(defaults['deepSleepAfterSeconds'] == 0 and defaults['inputs'] == [] and defaults['clientId'] == '', 'Unsafe Pi defaults')
    wiring = json.loads((target / 'usr/share/galdrun-inputs.json').read_text())
    require(wiring['inputGpioChip'] == ('/dev/galdrun-gpio' if modern else '/dev/gpiochip0'),
            'Wrong header GPIO controller')
    require((target / 'etc/init.d/S94galdrun-inputs').stat().st_mode & 0o111,
            'Missing executable Pi input provisioning service')
    for header in (26, 40):
        configured = json.loads((target / f'opt/galdrun-client/client-default-{header}pin.json').read_text())
        expected = dict(defaults, inputGpioChip=wiring['inputGpioChip'], inputs=[i for i in wiring['inputs']
                        if header == 40 or i['id'] in wiring['header26InputIds']])
        require(configured == expected, 'Wrong Pi input defaults for header ' + str(header))
    require('CONFIG_GPIO_CDEV=y\n' in (images / 'linux.config').read_text(), 'Missing GPIO character device')
    board_config = json.loads((target / 'etc/galdrun-board.json').read_text())
    require(board_config['board'] == 'raspberrypi-' + board, 'Wrong board identity')
    require(not board_config.get('hardwareClockControl'), 'Uncertified sample-clock control')
    network = dict(line.split('=', 1) for line in (target / 'etc/raumfeld-network.conf').read_text().splitlines()
                   if line and not line.startswith('#'))
    require(network.get('AP_SSID') == 'Galdrun-Pi' and len(network.get('AP_PASSWORD', '')) >= 8, 'Missing Pi setup hotspot defaults')
    require(board_config.get('sshSettings') and board_config.get('networkSettings'), 'Missing web SSH/network capabilities')
    root_password = next((line.split(':')[1] for line in (target / 'etc/shadow').read_text().splitlines() if line.startswith('root:')), '')
    require(root_password in ('*', '!'), 'Root must start locked without a shared password')
    client = target / 'opt/galdrun-client/galdrun-client'
    with client.open('rb') as stream:
        elf = stream.read(52)
    require(elf[:6] == (b'\x7fELF\x02\x01' if modern else b'\x7fELF\x01\x01') and
            struct.unpack_from('<H', elf, 18)[0] == (183 if modern else 40), 'Wrong client ELF class/architecture')
    require(modern or struct.unpack_from('<I', elf, 36)[0] & 0x400, 'Client must use the ARM hard-float ABI')
    manifest = dict(board=board,
        supportedModels=['Raspberry Pi 1 Model A/B (26-pin)', 'Raspberry Pi 1 Model A+/B+',
                         'Raspberry Pi Zero', 'Raspberry Pi Zero W/WH'] if board == 'rpi1'
                        else [f'Raspberry Pi {board[-1]} Model B (all RAM variants; physical qualification pending)'] if modern
                        else ['Raspberry Pi 2 Model B (rev 1.0/1.1/1.2)',
                              'Raspberry Pi 3 Model B', 'Raspberry Pi 3 Model B+', 'Raspberry Pi 3 Model A+',
                              'Raspberry Pi Zero 2 W (with or without headers)'],
        buildroot='2025.02.18', linux='6.12.112',
        linuxCommit='9cf0f5c001b790d53689b4aadca873db169cbc44',
        bootFirmwareCommit=None if board == 'rpi5' else '5476720d52cf579dc1627715262b30ba1242525e',
        clientTarget='linux-armv8' if modern else 'linux-armv6' if board == 'rpi1' else 'linux-armv7',
        client=dict(sha256=sha256(client), elf='ELF64 AArch64' if modern else 'ELF32 ARM EABI hard-float',
                    interpreter='/lib/ld-linux-aarch64.so.1' if modern else '/lib/ld-linux-armhf.so.3',
                    qemuCpu='cortex-a76' if board == 'rpi5' else 'cortex-a72' if board == 'rpi4' else 'arm1176' if board == 'rpi1' else 'cortex-a7'),
        audioProfiles=profiles,
        validation='host image structure, modules and client QEMU ABI; hardware certification pending',
        partitions=partitions, bootFiles={name: sha256(boot / name) for name in required},
        artifacts={name: dict(bytes=(images / name).stat().st_size, sha256=sha256(images / name)) for name in ('sdcard.img', 'boot.vfat', 'rootfs.ext4', 'linux.config', 'buildroot.config')})
    (images / 'firmware-manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    print('PASS:', board, 'SD layout, boot files, modules, firmware, appliance defaults')


if __name__ == '__main__':
    verify(Path(sys.argv[1]), sys.argv[2], Path(sys.argv[3]))
