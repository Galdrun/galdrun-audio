"""Check Pi wiring, actual default generation and first-boot provisioning (Linux)."""
from pathlib import Path
import json
import re
import subprocess
import sys
import tempfile

project = Path(__file__).resolve().parents[1]
wiring = json.loads((project / 'board/rpi/inputs.json').read_text())
reserved = {pin for pins in wiring['reservedGpios'].values() for pin in pins}
profiles = json.loads((project / 'board/rpi/audio-profiles.json').read_text())
for profile in profiles:
    pins = re.search(r'GPIO([0-9,]+)', profile.get('pins', ''))
    if pins:
        assert set(map(int, pins[1].split(','))) <= reserved, profile['id']
ids = [i['id'] for i in wiring['inputs']]
assert len(ids) == len(set(ids))
assert set(wiring['header26InputIds']) <= set(ids)
pins = []
for input in wiring['inputs']:
    pins += [input['pin']] if input['kind'] == 'button' else [input['pinA'], input['pinB']]
    assert input['pull'] == 'up'
assert len(pins) == len(set(pins)) == 10
assert not set(pins) & reserved
assert set(pins) <= {7, 8, 9, 10, 11, 12, 13, 23, 24, 26, 27}

# Execute the same Python generator embedded in post-build, not a second copy.
post_build = (project / 'board/rpi/post-build.sh').read_text()
generator = post_build.split("<<'PY'\n", 1)[1].split('\nPY', 1)[0]
service = (project / 'board/rpi/overlay/etc/init.d/S94galdrun-inputs').read_text()
subprocess.run(['sh', '-n'], input=service, text=True, check=True)
with tempfile.TemporaryDirectory(prefix='galdrun-pi-inputs-') as temp:
    root = Path(temp)
    client = root / 'opt/galdrun-client'
    client.mkdir(parents=True)
    config_dir = root / 'etc/galdrun'
    model = root / 'model'
    config = config_dir / 'client.json'
    for board in ('rpi1', 'rpi3', 'rpi4', 'rpi5'):
        baseline = (project / f'board/{board}/client-default.json').read_bytes()
        (client / 'client-default.json').write_bytes(baseline)
        subprocess.run([sys.executable, '-', str(client), str(project / 'board/rpi/inputs.json'),
                        str(project / f'board/{board}/client-board.json'), str(root / 'wiring.json')],
                       input=generator, text=True, check=True)
        assert (client / 'client-default.json').read_bytes() == baseline
        for header, count in ((26, 5), (40, 8)):
            generated = json.loads((client / f'client-default-{header}pin.json').read_text())
            assert len(generated['inputs']) == count
            assert generated['inputGpioChip'] == ('/dev/galdrun-gpio' if board in ('rpi4', 'rpi5') else '/dev/gpiochip0')
            assert generated['clientId'] == ''
            assert generated['deepSleepAfterSeconds'] == 0
            assert generated['name'] == json.loads(baseline)['name']
            if header == 26:
                used = {i[key] for i in generated['inputs'] for key in ('pin', 'pinA', 'pinB') if key in i}
                assert used == {7, 8, 9, 10, 11, 23, 24}

    script = root / 'provision'
    script.write_text(service.replace('/etc/galdrun', str(config_dir))
                      .replace('/opt/galdrun-client', str(client))
                      .replace('/proc/device-tree/model', str(model)))

    def run(action='start', ok=True):
        result = subprocess.run(['sh', str(script), action], capture_output=True, text=True, timeout=5)
        assert (result.returncode == 0) == ok, result.stdout + result.stderr

    run()  # No device tree: leave the no-input fallback for S95.
    assert not config.exists()
    for name, header in [('Raspberry Pi 2 Model B Rev 1.0', 40),
                         ('Raspberry Pi 2 Model B Rev 1.1', 40),
                         ('Raspberry Pi 2 Model B Rev 1.2', 40),
                         ('Raspberry Pi 2 Model B rev 1.2', 40),
                         ('Raspberry Pi 3 Model B Rev 1.2', 40),
                         ('Raspberry Pi 3 Model B Plus Rev 1.3', 40),
                         ('Raspberry Pi 3 Model A Plus Rev 1.0', 40),
                         ('Raspberry Pi Zero 2 W Rev 1.0', 40),
                         ('Raspberry Pi Zero 2 W Rev 1.1', 40),
                         ('Raspberry Pi Model B Plus Rev 1.2', 40),
                         ('Raspberry Pi Model A Plus Rev 1.1', 40),
                         ('Raspberry Pi Model A Plus Rev 1.2', 40),
                         ('Raspberry Pi Model A Rev 2', 26),
                         ('Raspberry Pi Model B Rev 1', 26),
                         ('Raspberry Pi Model B Rev 2', 26),
                         ('Raspberry Pi Zero Rev 1.2', 40),
                         ('Raspberry Pi Zero Rev 1.3', 40),
                         ('Raspberry Pi Zero W Rev 1.1', 40),
                         ('Raspberry Pi 4 Model B Rev 1.4', 40),
                         ('Raspberry Pi 5 Model B Rev 1.0', 40),
                         ('Unknown board', None)]:
        model.write_bytes(name.encode() + b'\0')
        run('stop')
        assert not config.exists()
        run()
        if header is None:
            assert not config.exists()
            continue
        assert config.read_bytes() == (client / f'client-default-{header}pin.json').read_bytes()
        assert config.stat().st_mode & 0o777 == 0o600
        assert config_dir.stat().st_mode & 0o777 == 0o700
        # Bindings, identity, custom wiring and explicit inputs: [] all survive.
        config.write_text('{"clientId":"existing-room","inputs":[]}\n')
        before = config.read_bytes()
        run()
        assert config.read_bytes() == before
        config.unlink()
    model.write_bytes(b'Raspberry Pi 3 Model B Rev 1.2\0')
    (client / 'client-default-40pin.json').unlink()
    run(ok=False)
    assert not config.exists(), 'Failed provisioning must not leave a partial config'
    run('invalid', ok=False)
print('PASS: reserved pins, 40/26-pin layouts, all images, private first-boot config and settings preservation')

with tempfile.TemporaryDirectory(prefix='galdrun-gpio-label-') as temp:
    root = Path(temp)
    model = root / 'model'
    devices = root / 'devices'
    devices.mkdir()
    controller = devices / 'platform-controller'
    controller.mkdir()
    gpio = root / 'gpio'
    gpio.mkdir()
    dev = root / 'dev'
    dev.mkdir()
    alias = dev / 'galdrun-gpio'
    script = root / 'gpio-init'
    source = (project / 'board/rpi/overlay/etc/init.d/S42galdrun-gpio').read_text()
    script.write_text(source.replace('/proc/device-tree/model', str(model))
                      .replace('/sys/class/gpio', str(gpio)).replace('/dev/', str(dev) + '/'))
    chip = gpio / 'gpiochip512'
    chip.mkdir()
    for label, name in [('pinctrl-rp1', 'Raspberry Pi 5 Model B Rev 1.0'),
                        ('pinctrl-bcm2711', 'Raspberry Pi 4 Model B Rev 1.4')]:
        model.write_bytes(name.encode() + b'\0')
        (chip / 'label').write_text(label + '\n')
        for number in (4, 0):
            target = controller / f'gpiochip{number}'
            target.mkdir(exist_ok=True)
            (target / 'dev').write_text(f'254:{number}\n')
            link = chip / 'device'
            link.unlink(missing_ok=True)
            link.symlink_to(controller)
            subprocess.run(['sh', str(script), 'start'], check=True)
            assert alias.readlink() == dev / f'gpiochip{number}'
            (target / 'dev').unlink()
            result = subprocess.run(['sh', str(script), 'start'], capture_output=True)
            assert result.returncode != 0 and not alias.is_symlink()
            target.rmdir()
        target.mkdir()
        (target / 'dev').write_text('254:0\n')
        # A duplicate label must fail rather than select a controller arbitrarily.
        duplicate = gpio / 'gpiochip600'
        duplicate.mkdir()
        (duplicate / 'label').write_text(label + '\n')
        (duplicate / 'device').symlink_to(controller)
        result = subprocess.run(['sh', str(script), 'start'], capture_output=True)
        assert result.returncode != 0 and not alias.is_symlink()
        (duplicate / 'label').unlink()
        (duplicate / 'device').unlink()
        duplicate.rmdir()
        (chip / 'label').write_text('other-controller\n')
        result = subprocess.run(['sh', str(script), 'start'], capture_output=True)
        assert result.returncode != 0 and not alias.is_symlink()
        (target / 'dev').unlink()
        target.rmdir()
    model.write_bytes(b'Raspberry Pi 3 Model B Rev 1.2\0')
    subprocess.run(['sh', str(script), 'start'], check=True)
    assert not alias.is_symlink()
print('PASS: Pi 4/RP1 GPIO labels, changed chip numbers, missing/ambiguous controllers and legacy bypass')
