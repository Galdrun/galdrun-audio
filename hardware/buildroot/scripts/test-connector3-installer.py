"""Exercise refusal and rollback boundaries with files/mocked tools, never MTD."""
from pathlib import Path
import hashlib
import importlib.util
import json
import os
import subprocess
import tempfile


project = Path(__file__).resolve().parents[1]
source = (project / 'board/connector3/installer/install-usb.sh').read_text()
subprocess.run(['sh', '-n'], input=source, text=True, check=True)
with tempfile.TemporaryDirectory(prefix='connector3-installer-test-') as directory:
    root = Path(directory)
    spec = importlib.util.spec_from_file_location('installer_pack', project/'scripts/pack-connector3-installer.py')
    packer = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(packer)
    packer.__file__ = str(root/'scripts/pack.py')
    def refuse_pack(mode, message):
        try:
            packer.pack(root, root, root, root, root, mode)
        except ValueError as error:
            assert message in str(error), error
        else:
            raise AssertionError('Installer packer bypassed '+message)
    refuse_pack('invalid', 'mode must be')
    refuse_pack('write', 'hardware-qualified production monitor')
    qualification = root/'output/connector3-monitor-production'
    qualification.mkdir(parents=True)
    (qualification/'tee-raw.bin').write_bytes(b'monitor')
    digest = hashlib.sha256(b'monitor').hexdigest()
    (qualification/'hardware-verification.json').write_text(json.dumps({'production_qualified':True,'sha256':digest}))
    persistent = root/'output/connector3-persistent'
    persistent.mkdir()
    (persistent/'persistent-verification.json').write_text(json.dumps({'monitor_production_qualified':False,'monitor_sha256':digest}))
    refuse_pack('write', 'Persistent FIT must use')
    for name in ('sys/class/mtd', 'sys/class/ubi', 'proc/device-tree', 'payload', 'etc', 'media/galdrun-backup', 'dev', 'run', 'bin'):
        (root / name).mkdir(parents=True, exist_ok=True)
    model = root / 'proc/device-tree/model'
    model.write_text('Galdrun Connector 3 installer check')
    (root / 'proc/mtd').write_text(''.join(f'mtd{number}: partition\n' for number in range(6)))
    (root / 'proc/mounts').write_text('')
    sizes = (8388608, 524288, 524288, 134217728, 12582912, 380633088)
    names = ('u-boot', 'u-boot-env', 'constants', 'settings', 'fit', 'rootfs')
    for number in range(6):
        mtd = root / f'sys/class/mtd/mtd{number}'
        mtd.mkdir()
        for key, value in dict(name=names[number], size=sizes[number], erasesize=262144,
                               writesize=4096, oobsize=256, ecc_strength=8, ecc_step_size=512,
                               ecc_failures=0, flags='0x0', bad_blocks=0, bbt_blocks=0).items():
            (mtd / key).write_text(str(value))
        (root / f'dev/mtd{number}').write_bytes(f'original partition {number}'.encode())
    for name in ('galdrun.itb', 'rootfs.ubi'):
        (root / 'payload' / name).write_bytes(name.encode())
    (root / 'etc/galdrun-install.sha256').write_text(''.join(
        hashlib.sha256((root / 'payload' / name).read_bytes()).hexdigest() + '  ' + name + '\n'
        for name in ('galdrun.itb', 'rootfs.ubi')))
    calls = root / 'calls'
    for name in ('nanddump', 'nandwrite', 'flash_erase', 'ubiformat', 'ubiattach', 'ubidetach'):
        tool = root / 'bin' / name
        if name == 'nanddump':
            tool.write_text('''#!/bin/sh
output=
for argument do
    last=$argument
    case "$argument" in --file=*) output=${argument#--file=};; esac
done
if [ -n "$output" ]; then
    if [ "${GALDRUN_TEST_RAW_MISMATCH:-}" = 1 ] && [ "${last##*/}" = mtd1 ]; then
        printf 'inconsistent corrected read' >"$output"
    else cat "$last" >"$output"; fi
else cat "$last"; fi
''')
        else:
            tool.write_text(f'#!/bin/sh\necho "{name} $*" >>"{calls}"\nexit 9\n')
        tool.chmod(0o755)
    command = source
    for prefix in ('/sys/', '/proc/', '/payload', '/etc/', '/media/', '/dev/', '/run/'):
        command = command.replace(prefix, str(root) + prefix)
    script = root / 'install.sh'
    script.write_text(command)
    env = dict(os.environ, PATH=str(root / 'bin') + ':' + os.environ['PATH'])

    def run(*args):
        return subprocess.run(['sh', str(script), *args], env=env, capture_output=True, text=True)

    assert run('--check').returncode == 0
    assert not calls.exists(), 'Check mode invoked a mutating tool'
    boot_flags = root/'sys/class/mtd/mtd0/flags'
    boot_flags.write_text('0x400')
    assert run('--check').returncode != 0, 'Writable MTD0 accepted'
    boot_flags.write_text('0x0')
    protected = root / 'sys/class/mtd/mtd2/flags'
    protected.write_text('0x400')
    assert run('--check').returncode != 0
    protected.write_text('0x0')
    assert run('--install').returncode != 0
    assert not calls.exists()
    # Exercise the real backup branch; filesystem commands are mocked too.
    (root/'proc/mounts').write_text('/dev/sda1 /media vfat ro 0 0\n')
    for name, body in (('mount', 'exit 0'), ('sync', 'exit 0'),
                       ('df', "printf 'Filesystem 1024-blocks Used Available Capacity Mounted on\\nusb 2000000 0 2000000 0%% /media\\n'")):
        tool = root/'bin'/name
        tool.write_text('#!/bin/sh\n'+body+'\n')
        tool.chmod(0o755)
    for mismatch in (False, True):
        folder = root/'media'/('backup-inconsistent' if mismatch else 'backup-consistent')
        script.write_text(command.replace(str(root/'media/galdrun-backup'), str(folder)))
        env['GALDRUN_TEST_RAW_MISMATCH'] = str(int(mismatch))
        result = run('--backup')
        if mismatch:
            assert result.returncode != 0 and 'differs from the independent read' in result.stderr, result.stderr
            assert not (folder/'backup.sha256').exists()
        else:
            assert result.returncode == 0 and 'BACKUP COMPLETE' in result.stdout, result.stderr
    script.write_text(command)
    env.pop('GALDRUN_TEST_RAW_MISMATCH')
    (root/'proc/mounts').write_text('')
    assert not calls.exists(), 'Backup invoked a flash writer'
    backup = root / 'media/galdrun-backup'
    manifest_names = []
    for number in range(6):
        name = f'mtd{number}.bin'
        (backup / name).write_bytes((root / f'dev/mtd{number}').read_bytes())
        manifest_names.append(name)
    (backup / 'protected.sha256').write_text(''.join(
        hashlib.sha256((root / f'dev/mtd{number}').read_bytes()).hexdigest() + '  ' +
        ('bootloader-raw' if number == 0 else str(root / f'dev/mtd{number}')) + '\n'
        for number in range(4)))
    manifest_names.append('protected.sha256')
    # An unstable raw MTD0 fingerprint cannot veto this read-only partition.
    protected_path = backup/'protected.sha256'
    protected_path.write_text(protected_path.read_text().replace(
        hashlib.sha256((root/'dev/mtd0').read_bytes()).hexdigest(), '0'*64))
    (backup / 'backup.sha256').write_text(''.join(
        hashlib.sha256((backup / name).read_bytes()).hexdigest() + '  ' + name + '\n' for name in manifest_names))
    pin = hashlib.sha256((backup / 'backup.sha256').read_bytes()).hexdigest()
    result = run('--install', pin)
    assert result.returncode != 0 and 'check/backup-only' in result.stderr, result.stderr
    assert not calls.exists(), 'A pinned backup bypassed the check-only image gate'
    model.write_text('Galdrun Connector 3 installer write')
    (root / 'etc/galdrun-install-writes-enabled').touch()
    for number in (4, 5):
        (root / f'sys/class/mtd/mtd{number}/flags').write_text('0x400')
    # The old settings volume is attached and its Bluetooth state kept first.
    old = root/'old-settings'
    (old/'bluetooth/AA:BB').mkdir(parents=True)
    (old/'bluetooth/AA:BB/info').write_text('paired')
    (old/'bluetooth-quality').write_text('medium')
    (old/'client.json').write_text('{"clientId":"kept-room"}')
    (old/'etc').mkdir()
    for name, body in (('ubiattach', f'echo "ubiattach $*" >>"{calls}"'),
                       ('ubidetach', f'echo "ubidetach $*" >>"{calls}"'),
                       ('mount', f'case "$*" in *ubi0:GaldrunSettings*) for t do :; done; cp -a "{old}/." "$t";; esac'),
                       ('umount', 'exit 0')):
        (root/'bin'/name).write_text('#!/bin/sh\n'+body+'\n')
        (root/'bin'/name).chmod(0o755)
    # Default: a clean install keeps nothing and never attaches the old volume.
    result = run('--install', pin)
    assert result.returncode != 0
    keep = root/'run/settings-keep'
    assert not any(keep.iterdir()), 'A clean install kept settings'
    log = calls.read_text()
    assert 'ubiformat' in log and 'ubiattach' not in log.split('ubiformat')[0], (log, result.stderr)
    calls.unlink()
    # The persistent variant (KEEP_SETTINGS) carries the listed names over.
    env['GALDRUN_KEEP_SETTINGS'] = 'client.json bluetooth headphones bluetooth-quality'
    result = run('--install', pin)
    env.pop('GALDRUN_KEEP_SETTINGS')
    assert result.returncode != 0
    log = calls.read_text()
    assert log.index("ubiattach") < log.index("ubiformat"), (log, result.stderr)
    assert (keep/'bluetooth/AA:BB/info').read_text() == 'paired'
    assert (keep/'bluetooth-quality').read_text() == 'medium'
    assert (keep/'client.json').read_text() == '{"clientId":"kept-room"}', 'The room must survive a reinstall'
    assert not (keep/'etc').exists(), 'Kept settings beyond Bluetooth state'
    assert 'ubiformat' in log and 'flash_erase' in log, (result.stdout, result.stderr, log)
    assert 'nandwrite' not in log and f'{root}/dev/mtd4' not in log, log
    assert all(f'{root}/dev/mtd{number}' not in log for number in range(4)), log
    # Without a backup (the self-installing USB image): the same install path,
    # no rollback attempt, factory partitions never written.
    calls.unlink()
    result = run('--install')
    assert result.returncode != 0
    log = calls.read_text()
    assert 'ubiformat' in log and 'no backup' in result.stderr, (result.stdout, result.stderr, log)
    assert 'ROLLBACK' not in result.stdout, result.stdout
    assert all(f'{root}/dev/mtd{number}' not in log for number in range(4)), log
print('Connector 3 installer: protection/pin gates, inconsistent-backup refusal, failed-root rollback boundary, backup-free install and clean/keep settings switch passed')
