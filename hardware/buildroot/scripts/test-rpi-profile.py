"""Exercise the actual profile helper with isolated boot files and mocked mounts."""
from pathlib import Path
import json
import os
import subprocess
import tempfile

project = Path(__file__).resolve().parents[1]
profiles = json.loads((project / 'board/rpi/audio-profiles.json').read_text())
inno = next(p for p in profiles if p['id'] == 'inno-maker-hifi-dac')
assert inno['overlay'] == 'allo-boss-dac-pcm512x-audio'
assert inno['module'] == 'snd-soc-allo-boss-dac'
for board in ('rpi1', 'rpi3', 'rpi4', 'rpi5'):
    config = (project / f'configs/raumfeld_{board}_defconfig').read_text()
    assert all('overlays/' + p['overlay'] in config for p in profiles if p['overlay'])
    boot_config = (project / f'board/{board}/config.txt').read_text()
    bt_config = ('dtoverlay=disable-bt\ndtoverlay=vc4-kms-v3d-pi4,cma-64\n' if board == 'rpi4'
                 else 'dtoverlay=vc4-kms-v3d-pi5\n' if board == 'rpi5'
                 else '[pi0w]\ndtoverlay=disable-bt\n[all]\n' if board == 'rpi1'
                 else '[pi3]\ndtoverlay=disable-bt\n[pi02]\ndtoverlay=disable-bt\n[all]\n')
    assert boot_config.endswith(bt_config)
with tempfile.TemporaryDirectory(prefix='galdrun-pi-profile-') as temp:
    root = Path(temp)
    boot = root / 'boot'
    (boot / 'overlays').mkdir(parents=True)
    (boot / 'config.txt').write_text((project / 'board/rpi3/config.txt').read_text())
    active = boot / 'galdrun-audio.txt'
    active.write_text('# Galdrun audio profile: usb\n')
    model = root / 'model'
    model.write_bytes(b'Raspberry Pi 3 Model B Rev 1.2\0')
    tsv = root / 'profiles.tsv'
    tsv.write_text(''.join(p['id'] + '\t' + p['overlay'] + '\n' for p in profiles))
    for p in profiles:
        if p['overlay']:
            (boot / 'overlays' / (p['overlay'] + '.dtbo')).write_bytes(b'test overlay')
    commands = root / 'bin'
    commands.mkdir()
    for name in ('mount', 'sync'):
        script = commands / name
        script.write_text('#!/bin/sh\nprintf "%s\\n" "$*" >> "$PROFILE_TEST_LOG"\n')
        script.chmod(0o755)
    helper = root / 'profile'
    source = (project / 'board/rpi/galdrun-audio-profile').read_text()
    for old, new in [('boot=/boot', 'boot=' + str(boot)), ('/proc/device-tree/model', str(model)),
                     ('/usr/share/galdrun-audio-profiles.tsv', str(tsv)),
                     ('/run/galdrun-profile.lock', str(root / 'lock'))]:
        source = source.replace(old, new)
    helper.write_text(source)
    env = dict(os.environ, PATH=f'{commands}:/usr/bin:/bin', PROFILE_TEST_LOG=str(root / 'mount.log'))

    def run(profile, ok=True):
        result = subprocess.run(['sh', str(helper), profile], env=env, capture_output=True, text=True, timeout=5)
        assert (result.returncode == 0) == ok, result.stdout + result.stderr
        assert not (root / 'lock').exists()
        return result

    for profile in ('../bad', 'unknown', 'hifiberry-dacplus,24db_digital_gain'):
        before = active.read_bytes()
        run(profile, False)
        assert active.read_bytes() == before
    for p in profiles:
        before = active.read_bytes()
        run(p['id'])
        assert active.read_text().startswith('# Galdrun audio profile: ' + p['id'] + '\n')
        assert (boot / 'galdrun-audio.previous.txt').read_bytes() == before
        assert active.read_text().count('dtoverlay=') == bool(p['overlay'])
    run('rollback')
    for name in ('Raspberry Pi 2 Model B Rev 1.0',
                 'Raspberry Pi 2 Model B Rev 1.1',
                 'Raspberry Pi 2 Model B Rev 1.2',
                 'Raspberry Pi 2 Model B rev 1.2',
                 'Raspberry Pi 3 Model B Plus Rev 1.3',
                 'Raspberry Pi 3 Model A Plus Rev 1.0',
                 'Raspberry Pi Zero 2 W Rev 1.0',
                 'Raspberry Pi Model B Plus Rev 1.2',
                 'Raspberry Pi Model A Plus Rev 1.1',
                 'Raspberry Pi Zero Rev 1.2',
                 'Raspberry Pi Zero Rev 1.3',
                 'Raspberry Pi Zero W Rev 1.1'):
        model.write_bytes(name.encode() + b'\0')
        for p in profiles:
            run(p['id'])
    for board, name in [('rpi4', 'Raspberry Pi 4 Model B Rev 1.4'), ('rpi5', 'Raspberry Pi 5 Model B Rev 1.0')]:
        (boot / 'config.txt').write_text((project / f'board/{board}/config.txt').read_text())
        model.write_bytes(name.encode() + b'\0')
        for p in profiles:
            run(p['id'])
    (boot / 'config.txt').write_text((project / 'board/rpi3/config.txt').read_text())
    print('PASS: all audio profiles accepted on supported 40-pin Pi 1/2/3/4/5/Zero models')
    for name in ('Raspberry Pi Model A Rev 2', 'Raspberry Pi Model B Rev 2',
                 'Unknown board'):
        model.write_bytes(name.encode() + b'\0')
        before = active.read_bytes()
        run('pcm5102a', False)
        assert active.read_bytes() == before
    run('usb')
    model.write_bytes(b'Raspberry Pi 3 Model B Rev 1.2\0')
    config = boot / 'config.txt'
    config.write_text(config.read_text() + 'dtoverlay=unmanaged-audio\n')
    before = active.read_bytes()
    run('pcm5102a', False)
    assert active.read_bytes() == before
    assert (root / 'mount.log').read_text().splitlines()[-1].startswith('-o remount,ro ')
    (boot / 'galdrun-audio.previous.txt').write_text('dtoverlay=unmanaged-audio\n')
    run('rollback', False)
    print('PASS: all', len(profiles), 'profiles, backup/rollback, injection, board/pin conflicts and read-only remount')

    settings = root / 'settings'
    settings.mkdir()
    cache = settings / 'audio_devices.json'
    startup = root / 'startup'
    source = (project / 'board/rpi/overlay/etc/init.d/S41galdrun-audio-profile').read_text()
    source = source.replace('/boot/', str(boot) + '/').replace('/etc/galdrun/', str(settings) + '/')
    source = source.replace('/run/galdrun-audio-boot.txt', str(root / 'boot-profile.txt'))
    startup.write_text(source)

    def start():
        subprocess.run(['sh', str(startup), 'start'], check=True)

    cache.write_text('old devices')
    start()
    assert not cache.exists(), 'First boot must scan'
    cache.write_text('current devices')
    start()
    assert cache.read_text() == 'current devices', 'Unchanged profile must retain its cache'
    for profile in ('inno-maker-hifi-dac', 'usb'):
        active.write_text('# Galdrun audio profile: ' + profile + '\n')
        start()
        assert not cache.exists(), 'Changed or restored profile must scan on boot'
        cache.write_text('new devices')
    print('PASS: startup rescans on first boot/profile change/rollback, retaining cache on unchanged boots')
