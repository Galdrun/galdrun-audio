"""Check RAM recovery and noninteractive host-key startup without a device."""
from pathlib import Path
import subprocess
import tempfile


source = (Path(__file__).resolve().parents[1] / "board/connector3/ram/init.sh").read_text()
subprocess.run(["sh", "-n"], input=source, text=True, check=True)
start = source.index("[ -s /etc/ssh/ssh_host_ed25519_key ]")
end = source.index("/usr/sbin/sshd -E", start)
with tempfile.TemporaryDirectory(prefix="connector3-ram-init-") as directory:
    key = Path(directory) / "hostkey"
    command = source[start:end].replace("/etc/ssh/ssh_host_ed25519_key", str(key))
    for existing in (False, True):
        before = key.read_bytes() if existing else None
        subprocess.run(["sh", "-c", command], stdin=subprocess.DEVNULL,
                       check=True, timeout=10)
        assert key.is_file()
        if existing:
            assert key.read_bytes() == before, "Existing host key was replaced"

start = source.index("    if ! ip ")
condition = source[start:source.index("; then", start)]
for address in (False, True):
    for ssh in (False, True):
        mocks = ("ip() { " + ("echo ' inet 10.0.0.74/24';" if address else ":;") + " };\n"
                 "pidof() { return " + ("0" if ssh else "1") + "; };\n")
        result = subprocess.run(["sh", "-c", mocks + condition + "; then exit 0; else exit 1; fi"])
        assert (result.returncode == 0) == (not address or not ssh)
print("Connector 3 RAM init: host-key creation/reuse and recovery conditions passed")
