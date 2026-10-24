"""Host-only checks of raumfeld-ssh, the settings page's SSH switch: password rules and what it writes."""
from pathlib import Path
import hashlib
import os
import subprocess
import tempfile
import time

project = Path(__file__).resolve().parents[1]


def executable(path, text):
    path.write_text(text)
    path.chmod(0o755)


with tempfile.TemporaryDirectory(prefix="connector2-ssh-") as directory:
    root = Path(directory)
    shadow, conf, log, commands = root / "shadow", root / "ssh.conf", root / "sshd.log", root / "commands"
    commands.mkdir()
    shadow.write_text("root::::::::\nsshd:*:::::::\n")
    # A stand-in crypt: "$6$<salt>$" + sha256 of the password; chpasswd writes the same.
    executable(commands / "cryptpw", '''#!/usr/bin/python3
import hashlib,sys
a=sys.argv; salt=a[a.index('-S')+1]; pw=sys.stdin.read()
print('$6$'+salt+'$'+hashlib.sha256(pw.encode()).hexdigest())
''')
    executable(commands / "chpasswd", f'''#!/usr/bin/python3
import hashlib,sys
from pathlib import Path
user,pw=sys.stdin.read().rstrip('\\n').split(':',1); assert user=='root' and '-c' in sys.argv
p=Path({str(shadow)!r}); lines=p.read_text().splitlines()
p.write_text(''.join(('root:$6$salt$'+hashlib.sha256(pw.encode()).hexdigest()+':::::::' if l.startswith('root:') else l)+'\\n' for l in lines))
''')
    sshd = root / "S50sshd"
    executable(sshd, f"#!/bin/sh\necho \"$1\" >> '{log}'\n")
    source = (project / "board/common/overlay/usr/sbin/raumfeld-ssh").read_text()
    for name, path in (("/etc/galdrun-ssh.conf", conf), ("/etc/shadow", shadow), ("/etc/init.d/S50sshd", sshd)):
        assert source.count(name) == 1, name
        source = source.replace(name, str(path))
    script = root / "raumfeld-ssh"
    executable(script, source)
    env = dict(os.environ, PATH=f"{commands}:/usr/bin:/bin")

    def run(enabled, current="", new="", fails=None):
        result = subprocess.run([str(script)], input=f"{enabled}\n{current}\n{new}\n", env=env,
                                capture_output=True, text=True, timeout=10)
        if fails:
            assert result.returncode != 0 and fails in result.stderr, result.stderr
        else:
            assert result.returncode == 0, result.stderr
            # The init script runs in the background.
            for _ in range(50):
                if log.exists() and log.read_text().splitlines()[-1:] == [("start" if enabled else "stop")]:
                    break
                time.sleep(0.05)
            else:
                raise AssertionError("sshd was not started/stopped")

    run(1, fails="choose a password")
    assert not conf.exists(), "SSH turned on without a password"
    run(1, new="short", fails="at least 8")
    run(2, new="long enough", fails="expected 1")
    run(1, new="first secret")
    assert conf.read_text() == "SSH_ENABLED=1\n" and (conf.stat().st_mode & 0o777) == 0o600
    first = shadow.read_text()
    assert hashlib.sha256(b"first secret").hexdigest() in first
    run(0, fails="enter the current password")
    run(0, current="wrong guess", fails="is wrong")
    assert conf.read_text() == "SSH_ENABLED=1\n", "a wrong password changed the setting"
    run(1, current="first secret", new="second secret")
    assert hashlib.sha256(b"second secret").hexdigest() in shadow.read_text()
    run(0, current="second secret")
    assert conf.read_text() == "SSH_ENABLED=0\n"
    for locked in ("*", "!"):
        shadow.write_text(f"root:{locked}:::::::\nsshd:*:::::::\n")
        run(1, fails="choose a password")
        assert conf.read_text() == "SSH_ENABLED=0\n"
        run(1, new="fresh Pi password")
        assert hashlib.sha256(b"fresh Pi password").hexdigest() in shadow.read_text()
        run(0, current="fresh Pi password")
    print("PASS: SSH needs a password to turn on, every change needs the current one, short/wrong input changes nothing")
