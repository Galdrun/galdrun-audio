"""Host-only checks for client service persistence and USB installer guards/write scope."""
from pathlib import Path
import hashlib
import io
import json
import os
import re
import subprocess
import tarfile
import tempfile
import time

project = Path(__file__).resolve().parents[1]


def executable(path, text):
    path.write_text(text)
    path.chmod(0o755)


def test_service(root):
    binary = root / "opt/galdrun-client/galdrun-client"
    binary.parent.mkdir(parents=True)
    binary.write_text("unused: daemon is mocked")
    (binary.parent / "client-default.json").write_text('{"clientId":""}\n')
    (root / "run").mkdir()
    (root / "etc").mkdir()
    (root / "proc").mkdir()
    mounts = root / "proc/mounts"
    mounts.write_text(f"logram {root / 'run/log'} tmpfs rw,size=4096k 0 0\n")
    commands = root / "commands"
    commands.mkdir()
    executable(commands / "testkill", "#!/bin/sh\n[ -f \"$MOCK_ROOT/running\" ]\n")
    executable(commands / "start-stop-daemon", '''#!/usr/bin/python3
from pathlib import Path
import json,os,subprocess,sys
r=Path(os.environ['MOCK_ROOT'])
if '-K' in sys.argv:
 subprocess.Popen(['/usr/bin/python3','-c',
  "import time;from pathlib import Path;time.sleep(0.4);Path("+repr(str(r/'running'))+").unlink()"],
  stdin=subprocess.DEVNULL,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
else:
 assert not (r/'running').exists(), 'Duplicate client or restart before shutdown'
 (r/'running').touch()
 (r/'run/galdrun-client.pid').write_text('999999')
 with (r/'starts').open('a') as f: f.write(json.dumps({'args':sys.argv,'env':dict(os.environ)})+'\\n')
''')
    source = (project / "board/common/overlay/etc/init.d/S95galdrun-client").read_text()
    source = source.replace("/proc/mounts", str(mounts))
    for name in ("/opt", "/run", "/etc"):
        source = source.replace(name + "/", str(root / name[1:]) + "/")
    source = source.replace("XDG_CONFIG_HOME=/etc", f"XDG_CONFIG_HOME={root / 'etc'}")
    source = source.replace("kill -0", str(commands / "testkill") + " -0")
    script = root / "service"
    executable(script, source)
    env = dict(os.environ, MOCK_ROOT=str(root), PATH=f"{commands}:/usr/sbin:/usr/bin:/bin")
    run = lambda action: subprocess.run([str(script), action], env=env, check=True, timeout=5)
    run("start")
    run("start")
    assert len((root / "starts").read_text().splitlines()) == 1
    config = root / "etc/galdrun/client.json"
    assert config.stat().st_mode & 0o777 == 0o600
    config.write_text('{"clientId":"existing-room","server":"10.0.0.6:4711"}\n')
    before = config.read_bytes()
    started = time.monotonic()
    run("restart")
    assert time.monotonic() - started >= 0.4
    assert config.read_bytes() == before
    assert len((root / "starts").read_text().splitlines()) == 2
    run("status")
    run("stop")
    assert not (root / "run/galdrun-client.pid").exists()
    # A stale PID file must not prevent stop/start or replace existing settings.
    (root / "run/galdrun-client.pid").write_text('999999')
    run("stop")
    run("start")
    record = json.loads((root / "starts").read_text().splitlines()[-1])
    assert record["env"]["LD_LIBRARY_PATH"] == str(binary.parent / "lib")
    assert "--config" in record["args"] and "--audio-cache" in record["args"]
    assert "--log-file" in record["args"]
    assert str(root / "run/log/galdrun-client.log") in record["args"]
    run("stop")
    mounts.write_text("")
    run("start")
    record = json.loads((root / "starts").read_text().splitlines()[-1])
    assert "--log-file" not in record["args"], "Missing log mount must disable file logging"
    run("stop")
    print("PASS: single client start, private config, slow restart, stale PID and config preservation")


def test_installer(root):
    for name in ("proc/device-tree", "sys/class/mtd/mtd8", "dev", "tmp", "mnt", "payload", "commands"):
        (root / name).mkdir(parents=True)
    model = root / "proc/device-tree/model"
    model.write_bytes(b"Custom Raumfeld Connector 2\0")
    mtd = root / "proc/mtd"
    geometry = '''dev:    size   erasesize  name
mtd0: 00020000 00020000 "SPL"
mtd1: 00020000 00020000 "SPL.backup1"
mtd2: 00020000 00020000 "SPL.backup2"
mtd3: 00020000 00020000 "Firmware Data"
mtd4: 00100000 00020000 "U-Boot"
mtd5: 00020000 00020000 "U-Boot Env"
mtd6: 00600000 00020000 "Kernel"
mtd7: 00040000 00020000 "DTB"
mtd8: 0f820000 00020000 "File System"
'''
    mtd.write_text(geometry)
    (root / "proc/mounts").write_text("")
    page = root / "sys/class/mtd/mtd8/writesize"
    oob = root / "sys/class/mtd/mtd8/oobsize"
    page.write_text("2048\n")
    oob.write_text("128\n")
    for index in range(9):
        (root / f"dev/mtd{index}").write_bytes(bytes([index]) * 32)
    payload = root / "payload"
    (payload / "kernel.uImage").write_bytes(b"test kernel")
    (payload / "dts.cramfs").write_bytes(b"test DTB")
    contents = {"boot/uImage": b"test kernel", "etc/client.json": b'{"clientId":""}'}
    with tarfile.open(payload / "rootfs.tgz", "w:gz") as archive:
        for name, content in contents.items():
            member = tarfile.TarInfo(name)
            member.size = len(content)
            archive.addfile(member, io.BytesIO(content))
    (payload / "rootfs-files.md5").write_text("".join(
        f"{hashlib.md5(content).hexdigest()}  {name}\n" for name, content in contents.items()))
    (payload / "payload.md5").write_text("".join(
        f"{hashlib.md5((payload / name).read_bytes()).hexdigest()}  {name}\n"
        for name in ("rootfs.tgz", "kernel.uImage", "dts.cramfs", "rootfs-files.md5")))
    commands = root / "commands"
    executable(commands / "tar", '''#!/bin/sh
for arg do
    case "$arg" in -*z*) echo 'Recovery tar has no gzip option' >&2; exit 1 ;; esac
done
exec /bin/tar "$@"
''')
    mock = '''#!/usr/bin/python3
from pathlib import Path
import os,sys
r=Path(os.environ['MOCK_ROOT']); tool=Path(sys.argv[0]).name; args=sys.argv[1:]
with (r/'commands.log').open('a') as f: f.write(tool+' '+' '.join(args)+'\\n')
if tool=='ubiformat': assert str(r/'dev/mtd8') in args
elif tool=='flash_erase':
 p=Path(args[1]); assert p.name in ('mtd6','mtd7'); p.write_bytes(b'')
elif tool=='nandwrite':
 p=Path(args[-2]); assert p.name in ('mtd6','mtd7'); p.write_bytes(Path(args[-1]).read_bytes())
elif tool=='nanddump':
 p=Path(args[args.index('-f')+1]); data=Path(args[-1]).read_bytes()
 if os.environ.get('CORRUPT_READBACK'): data=b'corrupt'
 p.write_bytes(data)
'''
    for name in ("ubidetach", "ubiformat", "ubiattach", "ubimkvol", "flash_erase", "nandwrite", "nanddump", "mount", "umount", "sync"):
        executable(commands / name, mock)
    source = (project / "board/connector2/install-usb.sh").read_text()
    paths = ("/proc", "/sys", "/dev", "/tmp", "/mnt", "/payload")
    source = re.sub("|".join(re.escape(name) + r"(?=/|\b)" for name in paths),
                    lambda match: str(root / match.group()[1:]), source)
    script = root / "installer"
    executable(script, source)
    startup = root / "startup"
    executable(startup, (project / "board/connector2/start-usb.sh").read_text().replace(
        "/usr/sbin/galdrun-install", str(script)))
    executable(commands / "reboot", mock)
    env = dict(os.environ, MOCK_ROOT=str(root), PATH=f"{commands}:/usr/bin:/bin")
    log = root / "commands.log"

    def run(action="--check", expected=0, **extra):
        invocation = [str(startup)] if action == "--install" else [str(script), action]
        result = subprocess.run(invocation, env=dict(env, **extra), capture_output=True, timeout=10)
        assert (result.returncode == 0) == (expected == 0), result.stdout + result.stderr
        return result

    run()
    assert not log.exists(), "Preflight invoked a write command"
    for target, invalid in ((model, b"Other board\0"), (mtd, geometry.replace("00600000", "00500000").encode()),
                            (page, b"4096\n"), (oob, b"64\n"),
                            (root / "proc/mounts", b"ubi0:RootFS /mnt ubifs rw 0 0\n"),
                            (payload / "kernel.uImage", b"corrupt kernel")):
        original = target.read_bytes()
        target.write_bytes(invalid)
        run("--install", expected=1)
        assert not log.exists(), "Invalid preflight reached write commands"
        target.write_bytes(original)
    protected = {i: (root / f"dev/mtd{i}").read_bytes() for i in range(6)}
    for number in (1, 2):
        led = root / f"sys/class/leds/raumfeld:{number}"
        led.mkdir(parents=True)
        (led / "trigger").write_text("none\n")
        (led / "brightness").write_text("0\n")
    run("--install")
    writes = log.read_text()
    assert f"ubiformat -q {root / 'dev/mtd8'} -s 512 -O 2048 -y" in writes
    assert "nandwrite" in writes and "nanddump" in writes
    assert writes.splitlines()[-2:] == ["sync ", "reboot -f"], "Verified installation did not sync/reboot"
    assert all((root / f"dev/mtd{i}").read_bytes() == content for i, content in protected.items())
    assert not (root / "mnt/boot/uImage").exists()
    assert all((root / f"sys/class/leds/raumfeld:{number}/brightness").read_text().strip() == "1"
               for number in (1, 2)), "Successful install did not leave both LEDs on"
    log.write_text("")
    result = run("--install", expected=1, CORRUPT_READBACK="1")
    assert b"readback failed" in result.stderr
    assert b"INSTALL COMPLETE:" not in result.stdout
    assert "reboot" not in log.read_text(), "Failed installation rebooted"
    print("PASS: preflight rejects six unsafe inputs before writes, mock install touches only mtd6-mtd8, verified install reboots, readback failure stops without reboot")


with tempfile.TemporaryDirectory(prefix="connector2-client-image-") as directory:
    root = Path(directory)
    service = root / "service"
    installer = root / "installer"
    service.mkdir()
    installer.mkdir()
    test_service(service)
    test_installer(installer)
