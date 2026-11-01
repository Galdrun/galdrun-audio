"""Exercise first-boot seeding and restart preservation without mounting storage."""
from pathlib import Path
import json
import socket
import subprocess
import tempfile


source=(Path(__file__).resolve().parents[1]/'board/connector3/persistent/init.sh').read_text()
subprocess.run(['sh','-n'],input=source,text=True,check=True)
start=source.index('if [ ! -d /settings/etc ]; then')
end=source.index('mount --bind',start)
with tempfile.TemporaryDirectory(prefix='connector3-persistent-init-') as directory:
    root=Path(directory)
    startup=source[source.index('mount -t proc'):source.index('exec 0<')]
    for already_mounted in (False,True):
        probe=root/('mounted-dev' if already_mounted else 'unmounted-dev')
        probe.mkdir()
        mounts=probe/'mounts'
        mounts.write_text('devtmpfs /dev devtmpfs rw 0 0\n' if already_mounted else '')
        pts=probe/'dev/pts'
        calls=probe/'calls'
        harness=f'''mount() {{
            printf '%s\\n' "$2" >>"{calls}"
            [ "$2" != devpts ] || [ -d "{pts}" ]
        }}
'''
        command=startup.replace('/proc/mounts',str(mounts)).replace('/dev/pts',str(pts))
        subprocess.run(['sh','-ec',harness+command],check=True)
        assert pts.is_dir()
        assert calls.read_text().splitlines().count('devtmpfs')==(0 if already_mounted else 1)
    settings=root/'settings'
    etc=root/'defaults-etc'
    defaults=root/'client-default.json'
    settings.mkdir()
    etc.mkdir()
    (etc/'network.conf').write_text('generic defaults\n')
    defaults.write_text('{"clientId":""}\n')
    # Simulate an interrupted first copy: partial data must not be promoted.
    (settings/'etc.new').mkdir()
    (settings/'etc.new/partial').write_text('interrupted')
    command=source[start:end].replace('/settings',str(settings)).replace(
        '/opt/galdrun-client/client-default.json',str(defaults)).replace(
        'cp -a /etc ',f'cp -a "{etc}" ')
    command='failed() { echo "$*" >&2; exit 1; };\n'+command
    subprocess.run(['sh','-ec',command],check=True)
    assert (settings/'etc/network.conf').read_text()=='generic defaults\n'
    assert not (settings/'etc/partial').exists()
    (settings/'etc/network.conf').write_text('saved private settings\n')
    (settings/'client.json').write_text('{"clientId":"existing-room","ledsOff":true}\n')
    (settings/'etc/galdrun-dac.json.Connector3').write_text('{"Output Level":53}\n')
    # New defaults on a later boot must not replace the saved room or settings.
    (etc/'network.conf').write_text('new generic defaults\n')
    subprocess.run(['sh','-ec',command],check=True)
    assert (settings/'etc/network.conf').read_text()=='saved private settings\n'
    assert json.loads((settings/'client.json').read_text())['clientId']=='existing-room'
    assert json.loads((settings/'client.json').read_text())['ledsOff'] is True
    assert (settings/'etc/galdrun-dac.json.Connector3').read_text()=='{"Output Level":53}\n'
    guard=source[source.index('for attempt in '):source.index('export LD_LIBRARY_PATH=')]
    endpoint=root/'mcu.sock'
    guard=guard.replace('/run/galdrun-connector3-mcu.sock',str(endpoint))
    guard='failed() { exit 99; }; sleep() { :; };\n'+guard
    assert subprocess.run(['sh','-ec',guard]).returncode==99, 'Client startup passed without MCU readiness'
    with socket.socket(socket.AF_UNIX,socket.SOCK_STREAM) as listener:
        listener.bind(str(endpoint))
        subprocess.run(['sh','-ec',guard],check=True)
print('Connector 3 persistent init: devtmpfs/devpts, interrupted seed, saved identity/LED/audio settings and MCU readiness gate passed')
