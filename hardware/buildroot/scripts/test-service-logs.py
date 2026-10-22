"""Check log RAM mount, idempotence and disabled-file fallback without real mounts."""
from pathlib import Path
import os
import subprocess
import tempfile

project = Path(__file__).resolve().parents[1]
overlay = project / "board/common/overlay"
for failed in (False, True):
    with tempfile.TemporaryDirectory(prefix="raumfeld-logs-") as temp:
        root = Path(temp)
        (root / "run").mkdir()
        (root / "proc").mkdir()
        mounts = root / "proc/mounts"
        mounts.write_text("")
        commands = root / "commands"
        commands.mkdir()
        mount = commands / "mount"
        mount.write_text('''#!/usr/bin/python3
import os,sys
from pathlib import Path
root=Path(os.environ['MOCK_ROOT'])
assert sys.argv[1:5] == ['-t','tmpfs','-o','size=4m,nr_inodes=128,nosuid,nodev,noexec,mode=0750']
with (root/'mount-calls').open('a') as out: out.write('mount\\n')
if os.environ['FAIL_MOUNT'] == '1': sys.exit(1)
(root/'proc/mounts').write_text('logram '+str(root/'run/log')+' tmpfs rw,size=4096k 0 0\\n')
''')
        mount.chmod(0o755)
        chown = commands / "chown"
        chown.write_text("#!/bin/sh\nexit 0\n")
        chown.chmod(0o755)
        source = (overlay / "etc/init.d/S00logram").read_text()
        source = source.replace("/run/", str(root / "run") + "/").replace("/proc/mounts", str(mounts))
        script = root / "init"
        script.write_text(source)
        env = dict(os.environ, MOCK_ROOT=str(root), FAIL_MOUNT=str(int(failed)),
                   PATH=f"{commands}:{os.environ['PATH']}")
        result = subprocess.run(["sh", str(script), "start"], env=env, capture_output=True)
        assert result.returncode == int(failed), result.stderr
        defaults = (overlay / "etc/default/syslogd").read_text()
        defaults = defaults.replace("/run/", str(root / "run") + "/").replace("/proc/mounts", str(mounts))
        args = subprocess.check_output(["sh", "-c", defaults + '\nprintf "%s" "$SYSLOGD_ARGS"'], env=env).decode()
        if failed:
            assert args == "-O /dev/null -s 0", args
            for name in ("wtmp", "btmp", "lastlog"):
                assert os.readlink(root / "run/log" / name) == "/dev/null"
        else:
            assert args == f"-O {root / 'run/log/messages'} -s 256 -b 1 -l 7", args
            for name in ("wtmp", "btmp", "lastlog"):
                assert os.readlink(root / "run/log" / name) == "/dev/null"
            assert os.readlink(root / "run/galdrun-client.log") == "log/galdrun-client.log"
            subprocess.run(["sh", str(script), "start"], env=env, check=True)
            assert (root / "mount-calls").read_text() == "mount\n"
        print("PASS:", "mount failure disables file retention" if failed else "4 MiB log mount, bounded syslog, disabled login history, idempotent start")
