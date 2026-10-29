"""Check radio selection across cached local/generic RAM image builds."""
from pathlib import Path
import os
import subprocess
import tempfile


project = Path(__file__).resolve().parents[1]
source = (project / "board/connector3/ram/post-build.sh").read_text()
start = source.index('install -D -m 0644 "${GALDRUN_CONNECTOR3_WIFI_FIRMWARE:')
end = source.index('\ninstall -D -m 0644 "$firmware/imx/', start)
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
print("Connector 3 Wi-Fi firmware: local/generic/local cache switching passed")
