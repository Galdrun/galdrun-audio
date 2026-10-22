"""Host-only LED pattern and bounded status-monitor integration checks."""
from pathlib import Path
import json
import socketserver
import subprocess
import tempfile
import threading
import time

project = Path(__file__).resolve().parents[1]
repo = project.parent.parent
source = project / "board/connector2/status-leds.cpp"
json_source = repo / "shared/json/src/Json.cpp"
includes = "-I" + str(repo / "shared/json/include")
with tempfile.TemporaryDirectory(prefix="raumfeld-led-test-") as directory:
    root = Path(directory)
    patterns = root / "patterns"
    subprocess.run(["g++", "-std=c++17", "-O2", "-Wall", "-Wextra", "-Werror", includes,
                    str(project / "scripts/test-status-leds.cpp"), str(json_source),
                    "-o", str(patterns)], check=True)
    subprocess.run([str(patterns)], check=True)
    board = root / "board.json"
    config = root / "client.json"
    text = source.read_text()
    for old, new in (("/etc/galdrun-board.json", str(board)),
                     ("/etc/galdrun/client.json", str(config)),
                     ("/sys/class/leds/raumfeld:1/", str(root / "led1") + "/"),
                     ("/sys/class/leds/raumfeld:2/", str(root / "led2") + "/")):
        assert text.count(json.dumps(old)) == 1
        text = text.replace(json.dumps(old), json.dumps(new))
    fixture = root / "monitor.cpp"
    fixture.write_text(text)
    binary = root / "monitor"
    subprocess.run(["g++", "-std=c++17", "-O2", "-Wall", "-Wextra", "-Werror", includes,
                    str(fixture), str(json_source), "-o", str(binary)], check=True)
    board.write_text('{"statusLeds":false}')
    subprocess.run([str(binary)], check=True, timeout=2)
    assert not (root / "led1").exists(), "Disabled capability touched LED files"
    for number in (1, 2):
        led = root / f"led{number}"
        led.mkdir()
        (led / "trigger").write_text("none\n")
        (led / "brightness").write_text("0\n")
    board.write_text('{"statusLeds":true}')
    response = {"status": {"server": {"state": "connected"},
                           "session": {"state": "established"},
                           "audio": {"deviceSelected": True, "playing": False}}}
    malformed = False

    class Handler(socketserver.StreamRequestHandler):
        def handle(self):
            assert json.loads(self.rfile.readline()) == {"command": "status"}
            self.wfile.write(b"invalid\n" if malformed else (json.dumps(response) + "\n").encode())

    server = socketserver.ThreadingTCPServer(("127.0.0.1", 0), Handler)
    server.daemon_threads = True
    config.write_text(json.dumps({"controlPort": server.server_address[1]}))
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    process = subprocess.Popen([str(binary)])

    def levels():
        assert process.poll() is None, "LED monitor exited unexpectedly"
        return tuple((root / f"led{number}/brightness").read_text().strip() for number in (1, 2))

    try:
        time.sleep(1.3)
        assert levels() == ("1", "1"), "Connected idle client is not ready"
        for state in ("standby", "standbyManual"):
            response["status"]["power"] = {"state": state}
            time.sleep(1.3)
            assert levels() == ("0", "0"), f"{state} did not switch off both LEDs"
        response["status"]["power"] = {"state": "on"}
        time.sleep(1.3)
        assert levels() == ("1", "1"), "Power on did not restore status LEDs"
        config.write_text(json.dumps({"controlPort": server.server_address[1], "ledsOff": True}))
        time.sleep(1.3)
        assert levels() == ("0", "0"), "LED mode OFF did not switch off both LEDs"
        config.write_text(json.dumps({"controlPort": server.server_address[1], "ledsOff": False}))
        time.sleep(1.3)
        assert levels() == ("1", "1"), "LED status mode did not recover"
        response["status"]["server"]["state"] = "notConfigured"
        time.sleep(1.3)
        observed = set()
        for _ in range(25):
            a, b = levels()
            assert a == "1"
            observed.add(b)
            time.sleep(.1)
        assert observed == {"0", "1"}, "Setup indication did not blink"
        response["status"]["server"]["state"] = "connected"
        time.sleep(1.3)
        assert levels() == ("1", "1"), "Ready indication did not recover"
        malformed = True
        time.sleep(1.3)
        assert levels() == ("1", "1"), "Transient API failure bypassed retry grace"
        time.sleep(5)
        observed = set()
        for _ in range(12):
            a, b = levels()
            assert a == "1"
            observed.add(b)
            time.sleep(.1)
        assert observed == {"0", "1"}, "Persistent API failure did not indicate error"
        malformed = False
        time.sleep(1.3)
        assert levels() == ("1", "1"), "Monitor did not recover after API failure"
    finally:
        process.terminate()
        process.wait(timeout=2)
        server.shutdown()
        server.server_close()
    assert process.returncode == 0
    assert all((root / f"led{number}/brightness").read_text().strip() == "0" for number in (1, 2))
    print("PASS: board opt-in, real TCP/JSON transport, idle/setup, failure grace, recovery and clean shutdown")
