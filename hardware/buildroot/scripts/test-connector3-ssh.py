"""Run the built ARM key generator and sshd config checks in an isolated chroot.

Usage: sudo python3 test-connector3-ssh.py <Connector 3 Buildroot work directory>
Needs qemu-arm. Opens no listener and never runs the diagnostic init script.
"""
from pathlib import Path
import json
import os
import shutil
import stat
import subprocess
import sys
import tempfile


def verify(output):
    output = Path(output).resolve()
    if os.geteuid() != 0:
        raise SystemExit("Run as root for the isolated chroot test")
    emulator = shutil.which("qemu-arm")
    if not emulator:
        raise SystemExit("qemu-arm is required")
    # /tmp may be nodev on the build host; use the Linux build filesystem.
    with tempfile.TemporaryDirectory(prefix="connector3-ssh-", dir=output.parent) as directory:
        root = Path(directory) / "root"
        root.mkdir()
        # Buildroot adds system users and device nodes during image assembly.
        # Test that final archive, not the pre-fakeroot target directory.
        with (output / "images/rootfs.cpio").open("rb") as archive:
            subprocess.run(["cpio", "-idm", "--quiet", "--no-absolute-filenames"],
                           stdin=archive, cwd=root, check=True)
        shutil.copy2(emulator, root / "qemu-arm")
        for name, minor in (("null", 3), ("zero", 5), ("random", 8), ("urandom", 9)):
            device = root / "dev" / name
            if not device.exists():
                os.mknod(device, stat.S_IFCHR | 0o666, os.makedev(1, minor))
        runner = ["chroot", str(root), "/qemu-arm", "-cpu", "cortex-a7"]
        subprocess.run([*runner, "/usr/bin/ssh-keygen", "-q", "-t", "ed25519",
                        "-N", "", "-f", "/etc/ssh/ssh_host_ed25519_key"],
                       check=True, timeout=30)
        subprocess.run([*runner, "/usr/sbin/sshd", "-t"], check=True, timeout=30)
        config = subprocess.run([*runner, "/usr/sbin/sshd", "-T"], check=True,
                                capture_output=True, text=True, timeout=30).stdout
        for setting in ("passwordauthentication no", "kbdinteractiveauthentication no",
                        "authenticationmethods publickey", "pubkeyauthentication yes",
                        "permitrootlogin without-password"):
            if setting not in config.splitlines():
                raise ValueError(f"Wrong effective SSH policy: {setting}")
    result = {"arm_host_key_generation": True, "arm_sshd_config_check": True,
              "password_login_disabled": True, "listener_opened": False,
              "hardware_ssh_tested": False}
    (output / "images/ssh-smoke-verification.json").write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(result))


if __name__ == "__main__":
    verify(sys.argv[1])
