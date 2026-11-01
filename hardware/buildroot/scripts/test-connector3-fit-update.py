"""Exercise the FIT-only updater against files, including write/readback rollback."""
from pathlib import Path
import hashlib
import os
import subprocess
import tempfile


source = (Path(__file__).resolve().parents[1] / 'board/connector3/installer/update-fit.sh').read_text()
subprocess.run(['sh', '-n'], input=source, text=True, check=True)
with tempfile.TemporaryDirectory(prefix='connector3-fit-update-') as directory:
    root = Path(directory)
    for name in ('run/galdrun-fit-update', 'media/galdrun-backup', 'sys/class/mtd',
                 'proc/device-tree', 'etc', 'usr/sbin', 'dev', 'bin'):
        (root / name).mkdir(parents=True)
    capacity = 12582912
    old = b'previous signed FIT'
    new = b'new signed FIT with embedded firmware'
    saved = old + b'\xff' * (capacity - len(old))
    digest = lambda data: hashlib.sha256(data).hexdigest()
    (root / 'dev/mtd4').write_bytes(saved)
    (root / 'dev/mtd5').write_bytes(b'rootfs and private settings must survive')
    for number in range(5):
        base = root / f'sys/class/mtd/mtd{number}'
        base.mkdir()
        for name, value in dict(flags='0x0' if number < 4 else '0x400',
                                bad_blocks=0, bbt_blocks=0, ecc_failures=0).items():
            (base / name).write_text(str(value))
        if number < 4:
            (root / f'dev/mtd{number}').write_bytes(f'factory {number}'.encode())
    backup = root / 'media/galdrun-backup'
    protected = ''.join(digest((root / f'dev/mtd{n}').read_bytes()) + '  ' +
                        ('bootloader-raw' if n == 0 else str(root / f'dev/mtd{n}')) + '\n' for n in range(4))
    (backup / 'protected.sha256').write_text(protected)
    (backup / 'backup.sha256').write_text(digest(protected.encode()) + '  protected.sha256\n')
    pin = digest((backup / 'backup.sha256').read_bytes())
    (root / 'proc/device-tree/model').write_text('Galdrun Connector 3 installer write')
    (root / 'etc/galdrun-install-writes-enabled').touch()
    work = root / 'run/galdrun-fit-update'
    (work / 'new.itb').write_bytes(new)
    for path, body in ((root / 'usr/sbin/galdrun-install', 'exit "${PREFLIGHT_EXIT:-0}"'),
                       (root / 'bin/uname', "echo 6.12.112-galdrun-installer-write")):
        path.write_text('#!/bin/sh\n' + body + '\n')
        path.chmod(0o755)
    # Mock only flash tools. Hashes, comparisons and shell control flow are real.
    tool_source = '''#!/usr/bin/env python3
from pathlib import Path
import sys
root = Path(__file__).resolve().parents[1]
name = Path(sys.argv[0]).name
args = sys.argv[1:]
if name == 'nanddump':
    data = Path(args[-1]).read_bytes()
    length = next((int(a.split('=', 1)[1]) for a in args if a.startswith('--length=')), len(data))
    output = next(a.split('=', 1)[1] for a in args if a.startswith('--file='))
    Path(output).write_bytes(data[:length])
else:
    with (root / 'calls').open('a') as stream: stream.write(name + ' ' + ' '.join(args) + '\\n')
    target = Path(args[-3] if name == 'flash_erase' else args[-2])
    assert target == root / 'dev/mtd4', 'Writer escaped MTD4'
    if name == 'flash_erase': target.write_bytes(b'\\xff' * 12582912)
    else:
        failure = root / 'failure'
        mode = failure.read_text() if failure.exists() else ''
        if failure.exists(): failure.unlink()
        if mode == 'write': sys.exit(9)
        data = Path(args[-1]).read_bytes()
        if mode == 'readback': data = b'!' + data[1:]
        target.write_bytes(data + b'\\xff' * (12582912 - len(data)))
'''
    for name in ('nanddump', 'nandwrite', 'flash_erase'):
        path = root / 'bin' / name
        path.write_text(tool_source)
        path.chmod(0o755)
    command = source
    for prefix in ('/sys/', '/proc/', '/etc/', '/media/', '/dev/', '/run/', '/usr/sbin/'):
        command = command.replace(prefix, str(root) + prefix)
    script = root / 'update-fit.sh'
    script.write_text(command)
    env = dict(os.environ, PATH=str(root / 'bin') + ':' + os.environ['PATH'])
    args = [pin, digest(old), str(len(old)), digest(new)]

    def run(action, *extra):
        return subprocess.run(['sh', str(script), action, *args, *extra], env=env,
                              text=True, capture_output=True)

    result = run('--prepare')
    assert result.returncode == 0, result.stderr
    assert (work / 'current-mtd4.bin').read_bytes() == saved
    assert not (root / 'calls').exists(), 'Prepare invoked a writer'
    saved_pin = digest(saved)
    assert run('--prepare').returncode != 0, 'Existing backup overwritten'
    assert run('--update', '0' * 64).returncode != 0, 'Unverified backup accepted'
    env['PREFLIGHT_EXIT'] = '1'
    assert run('--update', saved_pin).returncode != 0, 'Preflight failure ignored'
    env.pop('PREFLIGHT_EXIT')
    for target in ('sys/class/mtd/mtd0/flags', 'sys/class/mtd/mtd4/flags'):
        path = root / target
        value = path.read_text()
        path.write_text('0x400' if 'mtd0/' in target else '0x0')
        assert run('--update', saved_pin).returncode != 0, target
        path.write_text(value)
    factory = root / 'dev/mtd2'
    value = factory.read_bytes()
    factory.write_bytes(b'changed factory state')
    assert run('--update', saved_pin).returncode != 0
    factory.write_bytes(value)
    (work / 'new.itb').write_bytes(b'tampered FIT')
    assert run('--update', saved_pin).returncode != 0
    (work / 'new.itb').write_bytes(new)
    assert not (root / 'calls').exists(), 'Refusal invoked a writer'
    mtd5 = (root / 'dev/mtd5').read_bytes()
    for failure in ('write', 'readback', None):
        (root / 'dev/mtd4').write_bytes(saved)
        if failure:
            (root / 'failure').write_text(failure)
        result = run('--update', saved_pin)
        if failure:
            assert result.returncode != 0 and 'FIT ROLLBACK COMPLETE' in result.stderr, result.stderr
            assert (root / 'dev/mtd4').read_bytes() == saved
        else:
            assert result.returncode == 0 and 'FIT UPDATE COMPLETE' in result.stdout, result.stderr
            assert (root / 'dev/mtd4').read_bytes()[:len(new)] == new
        assert (root / 'dev/mtd5').read_bytes() == mtd5
    assert all(str(root / 'dev/mtd4') in line for line in (root / 'calls').read_text().splitlines())
print('Connector 3 FIT-only updater: pin/protection/preflight gates, exact readback and rollback passed; MTD5 preserved')
