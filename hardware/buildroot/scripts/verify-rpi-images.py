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
    require(board in ('rpi1', 'rpi3'), 'Unknown Pi board')
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
    dtbs = ['bcm2708-rpi-b-rev1', 'bcm2708-rpi-b', 'bcm2708-rpi-b-plus'] if board == 'rpi1' else ['bcm2710-rpi-3-b']
    profiles = json.loads((target / 'usr/share/galdrun-audio-profiles.json').read_text())
    overlays = ['disable-bt'] + [p['overlay'] for p in profiles if p['overlay']]
    required = ['config.txt', 'cmdline.txt', 'galdrun-audio.txt', 'bootcode.bin', 'start.elf', 'fixup.dat', 'zImage']
    required += [name + '.dtb' for name in dtbs] + ['overlays/' + name + '.dtbo' for name in overlays]
    for name in required:
        path = boot / name
        require(path.is_file() and path.stat().st_size > 0, 'Missing boot file: ' + name)
        fat = subprocess.check_output(['mtype', '-i', str(image) + '@@1048576', '::' + name])
        require(hashlib.sha256(fat).hexdigest() == sha256(path), 'FAT content mismatch: ' + name)
        if name.endswith(('.dtb', '.dtbo')):
            require(path.read_bytes()[:4] == b'\xd0\x0d\xfe\xed', 'Invalid device tree: ' + name)
    config = (boot / 'config.txt').read_text()
    require('kernel=zImage\n' in config and 'enable_uart=1\n' in config, 'Missing kernel/serial configuration')
    require('gpu_mem=32\n' in config and 'dtoverlay=\n' in config, 'Wrong firmware memory/automatic HAT policy')
    require('include galdrun-audio.txt\n' in config, 'Missing managed audio profile')
    require(board != 'rpi3' or 'dtoverlay=disable-bt\n' in config, 'Pi 3 must route PL011 away from Bluetooth')
    require('root=/dev/mmcblk0p2' in (boot / 'cmdline.txt').read_text(), 'Wrong root partition')
    require('CONFIG_SND_BCM2835=y\n' in (images / 'linux.config').read_text(),
            'Onboard audio must register before the initial client scan')
    modules = list((target / 'lib/modules').glob('6.12.112*/modules.dep'))
    require(len(modules) == 1, 'Missing/ambiguous Linux 6.12.112 module dependencies')
    dependencies = modules[0].read_text().replace('-', '_')
    require((target / 'usr/lib/liblzma.so.5').exists(), 'Missing XZ runtime for compressed module autoloading')
    require('raumfeld' not in dependencies and 'davinci_mcasp' not in dependencies, 'Unexpected Connector audio modules')
    required_modules = ['snd_usb_audio', 'snd_soc_bcm2835_i2s', 'ath9k_htc', 'rt2800usb', 'mt7601u', 'mt76x0u', 'mt76x2u', 'rtl8xxxu', 'rtw88_8822bu', 'rtw88_8822cu', 'rtw88_8821cu', 'rtw88_8723du', 'mt7921u']
    required_modules += [p['module'].replace('-', '_') for p in profiles if p.get('module')]
    for module in required_modules:
        require(module + '.ko' in dependencies, 'Missing module: ' + module)
    if board == 'rpi3':
        require('brcmfmac.ko' in dependencies, 'Missing internal Wi-Fi driver')
        firmware = target / 'lib/firmware/brcm'
        for name in ('brcmfmac43430-sdio.bin', 'brcmfmac43430-sdio.raspberrypi,3-model-b.txt'):
            require((firmware / name).is_file(), 'Missing Pi 3 Wi-Fi firmware/board data: ' + name)
    for name in ('etc/init.d/S02raumfeld-led-boot', 'etc/init.d/S40raumfeld-hardware', 'etc/init.d/S96raumfeld-status-leds', 'usr/sbin/raumfeld-suspend', 'etc/asound.conf', 'root/.ssh/authorized_keys', 'etc/galdrun-ssh.conf'):
        require(not (target / name).exists(), 'Unexpected Connector service or shared credential: ' + name)
    defaults = json.loads((target / 'opt/galdrun-client/client-default.json').read_text())
    require(defaults['deepSleepAfterSeconds'] == 0 and defaults['inputs'] == [] and defaults['clientId'] == '', 'Unsafe Pi defaults')
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
    require(elf[:6] == b'\x7fELF\x01\x01' and struct.unpack_from('<H', elf, 18)[0] == 40, 'Client must be 32-bit little-endian ARM ELF')
    require(struct.unpack_from('<I', elf, 36)[0] & 0x400, 'Client must use the ARM hard-float ABI')
    manifest = dict(board=board, buildroot='2025.02.18', linux='6.12.112',
        linuxCommit='9cf0f5c001b790d53689b4aadca873db169cbc44',
        bootFirmwareCommit='5476720d52cf579dc1627715262b30ba1242525e',
        clientTarget='linux-armv6' if board == 'rpi1' else 'linux-armv7',
        client=dict(sha256=sha256(client), elf='ELF32 ARM EABI hard-float', interpreter='/lib/ld-linux-armhf.so.3',
                    qemuCpu='arm1176' if board == 'rpi1' else 'cortex-a7'),
        audioProfiles=profiles,
        validation='host image structure, modules and client QEMU ABI; hardware certification pending',
        partitions=partitions, bootFiles={name: sha256(boot / name) for name in required},
        artifacts={name: dict(bytes=(images / name).stat().st_size, sha256=sha256(images / name)) for name in ('sdcard.img', 'boot.vfat', 'rootfs.ext4', 'linux.config', 'buildroot.config')})
    (images / 'firmware-manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    print('PASS:', board, 'SD layout, boot files, modules, firmware, appliance defaults')


if __name__ == '__main__':
    verify(Path(sys.argv[1]), sys.argv[2], Path(sys.argv[3]))
