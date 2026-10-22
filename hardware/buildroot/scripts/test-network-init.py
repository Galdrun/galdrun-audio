"""A slow supervisor shutdown must complete before the init script starts again."""
from pathlib import Path
import os
import subprocess
import tempfile

source = Path(__file__).resolve().parents[1] / "board/common/overlay/etc/init.d/S45raumfeld-network"
with tempfile.TemporaryDirectory(prefix="raumfeld-network-init-") as directory:
    root = Path(directory)
    pid = root / "network.pid"
    pid.write_text(str(os.getpid()))
    script = root / "init"
    script.write_text(source.read_text().replace("/run/raumfeld-network.pid", str(pid)))
    script.chmod(0o755)
    daemon = root / "start-stop-daemon"
    daemon.write_text('''#!/usr/bin/python3
import os,subprocess,sys
from pathlib import Path
root=Path(os.environ['MOCK_ROOT'])
if '-K' in sys.argv:
 subprocess.Popen(['/usr/bin/python3','-c',
  "import time;from pathlib import Path;time.sleep(1.2);Path("+repr(str(root/'network.pid'))+").unlink()"],
  stdin=subprocess.DEVNULL,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
else:
 assert not (root/'network.pid').exists(), 'Started before old supervisor exited'
 (root/'started').touch()
''')
    daemon.chmod(0o755)
    env = dict(os.environ, MOCK_ROOT=str(root), PATH=f"{root}:/usr/bin:/bin")
    subprocess.run([str(script), "restart"], env=env, check=True, timeout=5)
    assert (root / "started").exists(), "Restart silently left networking stopped"
    print("PASS: restart waits for a slow shutdown before starting the supervisor")
