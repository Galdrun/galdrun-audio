"""Check the Linux packages and smoke-test the self-contained x64 server (Linux or WSL)."""
import json
from pathlib import Path
import socket
import sqlite3
import subprocess
import tempfile
import time
import urllib.error
import urllib.request

root = Path(__file__).resolve().parents[2]
# ELF e_machine: 62 x86-64, 40 ARM, 183 AArch64.
for rid, machine in (("linux-x64", 62), ("linux-armv7", 40), ("linux-armv8", 183),
                     ("linux-armv9", 183)):
    release = root / "releases" / "server" / rid
    for name in ("galdrun-server", "libcoreclr.so", "libhostfxr.so", "libe_sqlite3.so"):
        header = (release / name).read_bytes()[:20]
        assert header[:4] == b"\x7fELF" and int.from_bytes(header[18:20], "little") == machine, name
    runtime = json.loads((release / "galdrun-server.runtimeconfig.json").read_text())["runtimeOptions"]
    assert "includedFrameworks" in runtime and "frameworks" not in runtime, rid
    for name in ("README.md", "server.example.json", "galdrun-server.service", "build-info.txt"):
        assert (release / name).is_file(), name
    assert len(list((release / "licenses").glob("*/THIRD-PARTY-NOTICES.TXT"))) == 2
    print(f"{rid}: native architecture and bundled runtimes verified")

release = root / "releases" / "server" / "linux-x64"
executable = release / "galdrun-server"
executable.chmod(executable.stat().st_mode | 0o111)
with tempfile.TemporaryDirectory(prefix="galdrun-server-test-") as temporary:
    directory = Path(temporary)
    with socket.socket() as port:
        port.bind(("127.0.0.1", 0))
        api_port = port.getsockname()[1]
    config = directory / "server.json"
    config.write_text(json.dumps({
        "dataDirectory": "data", "logging": {"file": ""},
        "media": {"port": 0}, "api": {"port": api_port},
        "core": {"discoveryPort": 0}
    }))
    with (directory / "server.log").open("w+") as log:
        server = subprocess.Popen([str(executable), "--config", str(config)], stdout=log, stderr=log)
        try:
            deadline = time.monotonic() + 30
            while True:
                assert server.poll() is None, "Server exited during startup"
                try:
                    with urllib.request.urlopen(f"http://127.0.0.1:{api_port}/api/v1/system", timeout=1) as response:
                        assert response.status == 200
                        json.load(response)
                    break
                except (urllib.error.URLError, TimeoutError):
                    assert time.monotonic() < deadline, "API startup timed out"
                    time.sleep(0.1)
            database = directory / "data" / "library.db"
            assert database.is_file(), "SQLite database was not initialized"
            with sqlite3.connect(database) as connection:
                assert connection.execute("SELECT count(*) FROM sqlite_master WHERE type='table'").fetchone()[0] > 0
            server.terminate()
            assert server.wait(timeout=15) == 0, "SIGTERM shutdown failed"
            print("linux-x64: API, SQLite initialization and clean SIGTERM shutdown verified")
        except BaseException:
            log.flush()
            print((directory / "server.log").read_text())
            raise
        finally:
            if server.poll() is None:
                server.kill()
                server.wait()
