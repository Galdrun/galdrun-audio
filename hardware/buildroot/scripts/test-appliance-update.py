"""Run the BusyBox updater for Pi/Connector identities with an isolated init service."""
from pathlib import Path
import io
import os
import subprocess
import tarfile
import tempfile

repo = Path(__file__).resolve().parents[3]
source = (repo / 'scripts/upload/connector2-remote.sh').read_text()
for board in ('raspberrypi-rpi1', 'raspberrypi-rpi3', 'raumfeld-connector2', 'unsupported'):
    with tempfile.TemporaryDirectory(prefix='galdrun-appliance-update-') as temp:
        root = Path(temp)
        installed = root / 'client'
        (installed / 'lib').mkdir(parents=True)
        (installed / 'licenses').mkdir()
        (installed / 'galdrun-client').write_text('old binary')
        marker = root / 'board.json'
        marker.write_text('{"board":"' + board + '"}\n')
        service = root / 'init'
        service.write_text('#!/bin/sh\nprintf "%s\\n" "$1" >> "' + str(root / 'service.log') + '"\n')
        service.chmod(0o755)
        archive = root / 'client.tar'
        script = root / 'update'
        text = source.replace('/opt/galdrun-client', str(installed)).replace('/tmp/galdrun-client.tar', str(archive))
        text = text.replace('/etc/galdrun-board.json', str(marker)).replace('/etc/init.d/S95galdrun-client', str(service))
        text = text.replace('sleep 2', 'sleep 0.01')
        script.write_text(text)
        buf = io.BytesIO()
        with tarfile.open(fileobj=buf, mode='w:gz') as tar:
            for name, data in {'galdrun-client': b'#!/bin/sh\necho galdrun-test\n', 'lib/libatomic.so.1': b'library',
                               'licenses/LICENSE': b'license', 'build-info.txt': b'test build'}.items():
                info = tarfile.TarInfo(name)
                info.size = len(data)
                tar.addfile(info, io.BytesIO(data))
        result = subprocess.run(['sh', str(script), '--stdin'], input=buf.getvalue(), capture_output=True, timeout=5)
        if board == 'unsupported':
            assert result.returncode != 0 and not (root / 'service.log').exists()
            assert (installed / 'galdrun-client').read_text() == 'old binary'
        else:
            assert result.returncode == 0, result.stdout + result.stderr
            assert (installed / 'galdrun-client.previous').read_text() == 'old binary'
            assert (installed / 'build-info.txt').read_text() == 'test build'
            assert (root / 'service.log').read_text().splitlines() == ['stop', 'start', 'status']
        assert not archive.exists()
print('PASS: gzip stream update for both Pi images and Connector, previous binary retained, unknown board rejected')
