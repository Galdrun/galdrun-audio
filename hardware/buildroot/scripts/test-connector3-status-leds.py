"""Exercise the shared status monitor against real TCP and Unix socket peers."""
from pathlib import Path
import json
import socketserver
import subprocess
import tempfile
import threading
import time

project = Path(__file__).resolve().parents[1]
repo = project.parent.parent
with tempfile.TemporaryDirectory(prefix='connector3-led-test-') as directory:
    root = Path(directory)
    board, config, mcu = (root / name for name in ('board.json', 'client.json', 'mcu.sock'))
    source = (project / 'board/connector2/status-leds.cpp').read_text().replace(
        '"/etc/galdrun-board.json"', json.dumps(str(board)))
    (root / 'monitor.cpp').write_text(source)
    transport = (repo / 'client/src/audio/linux/Connector3Mcu.cpp').read_text().replace(
        '"/run/galdrun-connector3-mcu.sock"', json.dumps(str(mcu)))
    (root / 'transport.cpp').write_text(transport)
    binary = root / 'monitor'
    subprocess.run(['g++', '-std=c++17', '-O2', '-Wall', '-Wextra', '-Werror', '-DCONNECTOR3_STATUS_LEDS',
        '-I' + str(repo / 'shared/json/include'), '-I' + str(repo / 'client/src/audio/linux'),
        str(root / 'monitor.cpp'), str(root / 'transport.cpp'), str(repo / 'shared/json/src/Json.cpp'),
        '-o', str(binary)], check=True)
    board.write_text('{"statusLeds":false}')
    subprocess.run([str(binary), str(config)], check=True, timeout=2)
    patterns = []
    response = {'status': {'power': {'state': 'on'}, 'server': {'state': 'connected'},
        'session': {'state': 'established'}, 'audio': {'deviceSelected': True}}}

    class Client(socketserver.StreamRequestHandler):
        def handle(self):
            assert json.loads(self.rfile.readline()) == {'command': 'status'}
            self.wfile.write((json.dumps(response) + '\n').encode())

    class Mcu(socketserver.StreamRequestHandler):
        def handle(self):
            command, value = self.rfile.readline().decode().split()
            assert command == 'indication'
            patterns.append(int(value))
            self.wfile.write(b'ok 0 53\n')

    client = socketserver.ThreadingTCPServer(('127.0.0.1', 0), Client)
    service = socketserver.ThreadingUnixStreamServer(str(mcu), Mcu)
    for server in (client, service):
        server.daemon_threads = True
        threading.Thread(target=server.serve_forever, daemon=True).start()
    config.write_text(json.dumps({'controlPort': client.server_address[1]}))
    board.write_text('{"statusLeds":true}')
    process = subprocess.Popen([str(binary), str(config)])

    def expect(pattern):
        deadline = time.monotonic() + 3
        while time.monotonic() < deadline:
            assert process.poll() is None
            if patterns and patterns[-1] == pattern:
                return
            time.sleep(.05)
        raise AssertionError(f'Expected indication {pattern}, got {patterns}')

    try:
        expect(10)
        response['status']['server']['state'] = 'connecting'
        expect(5)
        response['status']['server']['state'] = 'notConfigured'
        expect(6)
        response['status']['server']['state'] = 'connected'
        response['status']['power']['state'] = 'standby'
        expect(0)
        response['status']['power']['state'] = 'on'
        expect(10)
        config.write_text(json.dumps({'controlPort': client.server_address[1], 'ledsOff': True}))
        expect(0)
        config.write_text(json.dumps({'controlPort': client.server_address[1], 'ledsOff': False}))
        expect(10)
    finally:
        process.terminate()
        process.wait(timeout=5)
        for server in (client, service):
            server.shutdown()
            server.server_close()
    assert process.returncode == 0 and patterns[-1] == 0
print('PASS: Connector 3 MCU LED transport, ready/connecting/setup/standby, saved off switch and shutdown')
