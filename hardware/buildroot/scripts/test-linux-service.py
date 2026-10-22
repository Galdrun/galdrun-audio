"""Validate the actual Pi uploader's generated log policy with systemd."""
from pathlib import Path
import getpass
import subprocess
import tempfile

project = Path(__file__).resolve().parents[3]
text = (project / "scripts/upload/linux-remote.sh").read_text()
unit = text.split('cat > /etc/systemd/system/galdrun-client.service <<UNIT\n', 1)[1].split('\nUNIT', 1)[0]
assert '--log-file /run/galdrun-client/client.log' in unit
assert 'RuntimeDirectory=galdrun-client' in unit
assert 'StandardOutput=null' in unit and 'StandardError=null' in unit
with tempfile.TemporaryDirectory(prefix="galdrun-unit-") as temp:
    root = Path(temp)
    script = root / "run.sh"
    script.write_text('#!/bin/sh\nexit 0\n')
    script.chmod(0o755)
    unit = unit.replace('$deploy_user', getpass.getuser()).replace('$client_dir', temp).replace('@SERVER_ARGS@', '--server 192.168.1.20:4711 ')
    path = root / "galdrun-client.service"
    path.write_text(unit)
    subprocess.run(['systemd-analyze', 'verify', str(path)], check=True, capture_output=True)
print('PASS: Pi uploader unit uses bounded RAM logging, no duplicate collector, valid systemd directives')
