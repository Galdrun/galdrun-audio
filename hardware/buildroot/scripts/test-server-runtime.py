"""Smoke-test the installed Pi server with its own libraries under QEMU user mode."""
from pathlib import Path
import json
import os
import socket
import subprocess
import sys
import tempfile
import time
import urllib.request


def verify(root, board):
    qemu, cpu = {'rpi3': ('qemu-arm', 'cortex-a7'), 'rpi4': ('qemu-aarch64', 'cortex-a72'),
                 'rpi5': ('qemu-aarch64', 'cortex-a76')}[board]
    libraries = f'{root}/usr/lib:{root}/lib'
    runner = [qemu, '-cpu', cpu, '-L', str(root), '-E', 'LD_BIND_NOW=1']
    subprocess.run([*runner, '-E', f'LD_LIBRARY_PATH={root}/opt/galdrun-core/lib:{libraries}',
                    str(root / 'opt/galdrun-core/galdrun-core'), '--version'], check=True, timeout=15)
    defaults = list((root / 'opt/galdrun-client').glob('client-default*.json'))
    assert defaults, 'client default configuration is missing'
    for config in defaults:
        assert json.loads(config.read_text()).get('active', True) is True, f'{config}: client must default on'
    service = (root / 'etc/init.d/S91galdrun-server').read_text()
    assert 'DOTNET_SYSTEM_GLOBALIZATION_INVARIANT=1' not in service, 'ICU must remain enabled'
    runner += ['-E', f'LD_LIBRARY_PATH={libraries}']
    if board == 'rpi3':
        # Work around the observed QEMU 10.2.1 ARM32/.NET 11 W^X startup crash.
        # Scope the emulation workaround here; keep production runtime defaults.
        runner += ['-E', 'DOTNET_EnableWriteXorExecute=0']
    environment = os.environ.copy()
    environment.pop('DOTNET_SYSTEM_GLOBALIZATION_INVARIANT', None)
    with tempfile.TemporaryDirectory(prefix='galdrun-pi-server-') as directory:
        work = Path(directory)
        with socket.socket() as listener, socket.socket() as unused_core:
            listener.bind(('127.0.0.1', 0))
            port = listener.getsockname()[1]
            unused_core.bind(('127.0.0.1', 0))
            core_port = unused_core.getsockname()[1]
        config = work / 'server.json'
        config.write_text(json.dumps({'dataDirectory': str(work / 'data'), 'logging': {'file': ''},
            'core': {'port': core_port, 'discoveryPort': 0},
            'media': {'port': 0}, 'api': {'port': port}, 'gui': {'port': 0}}))

        def request(path, body=None):
            req = urllib.request.Request(f'http://127.0.0.1:{port}/api/v1/{path}',
                data=None if body is None else json.dumps(body).encode(),
                headers={'Content-Type': 'application/json'})
            with urllib.request.urlopen(req, timeout=10) as response:
                return json.load(response)

        with (work / 'server.log').open('w+') as log:
            process = subprocess.Popen([*runner, str(root / 'opt/galdrun-server/galdrun-server'),
                '--config', str(config)], env=environment, stdout=log, stderr=log)
            try:
                deadline = time.monotonic() + 60
                while True:
                    assert process.poll() is None, f'server exited with {process.returncode}'
                    try:
                        request('system')
                        break
                    except OSError:
                        assert time.monotonic() < deadline, 'API startup timed out'
                        time.sleep(0.2)
                request('stations', {'name': 'Caf\u00e9 Ol\u00e9', 'streamUrl': 'http://radio.invalid/test'})
                found = request('providers/webradio/browse?node=stations&filter=cafe')
                assert [s['name'] for s in found['items']['stations']] == ['Caf\u00e9 Ol\u00e9'], 'ICU browse regression'
                # Also exercise JIT-generated request handlers after initial startup.
                for _ in range(10):
                    request('rooms')
                    request('system')
                assert (work / 'data/library.db').is_file(), 'SQLite was not initialized'
                process.terminate()
                assert process.wait(timeout=20) == 0, 'clean SIGTERM shutdown failed'
                print(f'PASS: {board} core ABI, enabled client defaults, server API, ICU browsing, SQLite and shutdown')
            except BaseException:
                log.flush()
                print((work / 'server.log').read_text())
                raise
            finally:
                if process.poll() is None:
                    process.kill()
                process.wait()


if __name__ == '__main__':
    verify(Path(sys.argv[1]).resolve(), sys.argv[2])
