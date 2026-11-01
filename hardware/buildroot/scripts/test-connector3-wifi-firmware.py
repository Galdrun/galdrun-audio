"""Check radio selection across cached local/generic RAM image builds."""
from pathlib import Path
import os
import subprocess
import tempfile


project = Path(__file__).resolve().parents[1]
source = (project / "board/connector3/ram/post-build.sh").read_text()
start = source.index('install -D -m 0644 "${GALDRUN_CONNECTOR3_WIFI_FIRMWARE:')
end = source.index('\n# Keep source attribution', start)
command = source[start:end]
with tempfile.TemporaryDirectory(prefix="connector3-radio-") as directory:
    root = Path(directory)
    upstream = root / "upstream/mrvl/pcie8897_uapsta.bin"
    upstream.parent.mkdir(parents=True)
    upstream.write_bytes(b"upstream radio")
    local = root / "local.bin"
    local.write_bytes(b"device radio")
    target = root / "target"
    env = dict(os.environ, firmware=str(upstream.parent.parent), target=str(target))
    for selected in (local, None, local):
        env["GALDRUN_CONNECTOR3_WIFI_FIRMWARE"] = str(selected) if selected else ""
        subprocess.run(["sh", "-ec", command], env=env, check=True)
        assert (target / "lib/firmware/mrvl/pcie8897_uapsta.bin").read_bytes() == (
            selected if selected else upstream).read_bytes()
    # A cached approved build must leave neither notices nor firmware behind
    # when switching back to upstream; arbitrary local bytes must fail closed.
    notice_dir = target / "usr/share/licenses/connector3-radio"
    notice_dir.mkdir(parents=True)
    for name in ("LICENCE.Marvell", "source.json"):
        (notice_dir / name).write_text("stale local record")
    env["board"] = str(project / "board/connector3/ram")
    env["GALDRUN_CONNECTOR3_WIFI_FIRMWARE"] = ""
    full_command = source[start:source.index('\ninstall -D -m 0644 "$firmware/imx/', start)]
    subprocess.run(["bash", "-ec", full_command], env=env, check=True)
    assert not any(notice_dir.iterdir())
    assert (target / "lib/firmware/mrvl/pcie8897_uapsta.bin").read_bytes() == upstream.read_bytes()
    env["GALDRUN_CONNECTOR3_WIFI_FIRMWARE"] = str(local)
    rejected = subprocess.run(["bash", "-ec", full_command], env=env, capture_output=True)
    assert rejected.returncode != 0, "Unapproved local firmware accepted"
print("Connector 3 Wi-Fi firmware: local/generic/local cache switching passed")
