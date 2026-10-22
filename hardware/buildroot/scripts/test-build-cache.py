"""Run the real build driver with mock make/download verification in a temporary tree."""
from pathlib import Path
import json
import os
import shutil
import subprocess
import tempfile

project = Path(__file__).resolve().parents[1]


def executable(path, content):
    path.write_text(content)
    path.chmod(0o755)


with tempfile.TemporaryDirectory(prefix="galdrun-build-cache-") as temp:
    root = Path(temp)
    source = root / "project"
    commands = root / "commands"
    work = root / "cache"
    output = work / "connector2"
    for path in (source / "configs", source / "downloads/linux", source / "scripts",
                 source / "board/connector2", commands, output / "images",
                 output / "build/openssh-test", work / "buildroot-2025.02.18"):
        path.mkdir(parents=True, exist_ok=True)
    (source / "configs/raumfeld_connector2_defconfig").write_text("BR2_PACKAGE_OPENSSH=y\n")
    (source / "external.mk").write_text("")
    (source / "downloads/linux/linux-6.12.112.tar.xz").write_text("mock archive\n")
    (source / "scripts/build-usb-installer.py").write_text("# Packaging is outside this regression.\n")
    shutil.copyfile(project / "build.sh", source / "build.sh")
    executable(source / "PrepareHost.sh", '''#!/bin/bash
work=$MOCK_WORK
export PATH="$MOCK_COMMANDS:$PATH"
''')
    executable(commands / "sha256sum", '''#!/bin/sh
if [ "${1:-}" = -c ]; then cat >/dev/null; exit 0; fi
exec /usr/bin/sha256sum "$@"
''')
    executable(commands / "make", '''#!/usr/bin/python3
from pathlib import Path
import json,os,shutil,sys
root=Path(os.environ["MOCK_WORK"])
with (root/"make.jsonl").open("a") as log: log.write(json.dumps(sys.argv[1:])+"\\n")
if "openssh-dirclean" in sys.argv:
    for path in (root/"connector2/build").glob("openssh-*"): shutil.rmtree(path)
elif not any(target in sys.argv for target in ("raumfeld_connector2_defconfig","linux","linux-dirclean","busybox-dirclean")):
    (root/"connector2/build/openssh-test").mkdir(exist_ok=True)
    if os.environ.get("MOCK_FAIL_BUILD"): sys.exit(1)
''')
    common = source / "board/common/patches/openssh"
    common.mkdir(parents=True)
    patch = common / "0001-test.patch"
    patch.write_text("first patch\n")
    env = dict(os.environ, MOCK_WORK=str(work), MOCK_COMMANDS=str(commands))
    stamp = output / ".raumfeld-openssh-inputs"

    def run(clean_count, fail=False):
        log = work / "make.jsonl"
        log.write_text("")
        result = subprocess.run(["bash", str(source / "build.sh"), "connector2"],
            env=dict(env, **({"MOCK_FAIL_BUILD": "1"} if fail else {})),
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, timeout=15)
        assert (result.returncode != 0) == fail, result.stdout + result.stderr
        calls = [json.loads(line) for line in log.read_text().splitlines()]
        assert sum("openssh-dirclean" in call for call in calls) == clean_count, calls
        if not fail:
            assert stamp.is_file() and len(stamp.read_text().strip()) == 64

    run(1)  # Existing untracked package needs rebuilding once.
    run(0)  # Identical patch inputs reuse the cache.
    patch.touch()
    run(0)  # Timestamps alone must not trigger a rebuild.
    patch.write_text("modified patch\n")
    run(1)
    added = common / "0002-test.patch"
    added.write_text("second patch\n")
    run(1)
    added = added.rename(common / "0003-test.patch")
    run(1)  # Patch order/name is part of the applied input.
    added.unlink()
    run(1)
    shutil.rmtree(common)
    run(1)  # Removing the entire common patch directory also invalidates.
    common.mkdir()
    run(0)
    board = source / "board/connector2/patches/openssh"
    board.mkdir(parents=True)
    patch = board / "0001-board.patch"
    patch.write_text("board patch\n")
    run(1)
    previous = stamp.read_text()
    patch.write_text("changed board patch\n")
    run(1, fail=True)
    assert stamp.read_text() == previous, "Failed builds must not mark new patches as accepted"
    run(1)
    shutil.rmtree(output / "build/openssh-test")
    stamp.unlink()
    run(0)  # No existing package means no unnecessary dirclean.
    print("PASS: real build driver invalidates changed/added/renamed/deleted OpenSSH patches, reuses unchanged inputs and retries failures")
