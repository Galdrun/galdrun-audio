"""Run the firmware scan worker with mocked iw, including timeout and cleanup."""
from pathlib import Path
import os
import subprocess
import tempfile

source = Path(__file__).resolve().parents[1] / "board/common/overlay/usr/sbin/raumfeld-wifi-scan"
for scenario in ("success", "failed", "timeout", "cancelled", "oversized",
                 "single-down", "single-up", "single-failed", "single-claimed", "missing"):
    with tempfile.TemporaryDirectory(prefix="raumfeld-scan-") as directory:
        root = Path(directory)
        script = root / "scan.sh"
        script.write_text(source.read_text().replace("state=/run/raumfeld-network", f"state={root}")
                          .replace("sleep 15", "sleep 0.3"))
        iw = root / "iw"
        iw.write_text('''#!/bin/sh
case "$*" in
 dev) [ "$SCENARIO" = missing ] || echo 'Interface mlan0' ;;
 'dev mlan0 interface add galdrunscan0 type managed')
  case "$SCENARIO" in single-*) exit 1 ;; esac ;;
 'dev mlan0 info') echo ' type managed' ;;
 'dev mlan0 scan')
  [ "$SCENARIO" = single-up ] || [ -f "$SCAN_ROOT/up" ] || { echo 'Network is down' >&2; exit 1; }
  if [ "$SCENARIO" = single-failed ]; then echo 'driver busy' >&2; exit 1; fi
  if [ "$SCENARIO" = single-claimed ]; then touch "$SCAN_ROOT/wpa.pid"; fi
  printf 'BSS aa:bb:cc:dd:ee:01\\n\\tSSID: Test\\n' ;;
 'dev galdrunscan0 scan')
  case "$SCENARIO" in
   success) printf 'BSS aa:bb:cc:dd:ee:01\\n\\tSSID: Test\\n' ;;
   failed) echo 'driver busy' >&2; exit 1 ;;
   oversized) head -c 1048576 /dev/zero ;;
   *) sleep 5 & child=$!; trap 'kill "$child"; wait "$child"; exit 1' TERM; wait "$child" ;;
  esac ;;
 'dev galdrunscan0 del') echo deleted > "$SCAN_ROOT/deleted" ;;
esac
''')
        ip = root / "ip"
        ip.write_text('''#!/bin/sh
case "$*" in
 '-o link show dev mlan0')
  if [ "$SCENARIO" = single-up ]; then echo '3: mlan0: <BROADCAST,MULTICAST,UP,LOWER_UP>'
  else echo '3: mlan0: <BROADCAST,MULTICAST>' ; fi ;;
 'link set mlan0 up') touch "$SCAN_ROOT/up" ;;
 'link set mlan0 down') touch "$SCAN_ROOT/down" ;;
esac
''')
        iw.chmod(0o755)
        ip.chmod(0o755)
        (root / "scan.request").touch()
        environment = dict(os.environ, PATH=f"{root}:{os.environ['PATH']}", SCENARIO=scenario, SCAN_ROOT=directory)
        process = subprocess.Popen(["sh", str(script)], env=environment, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        if scenario == "cancelled":
            import time
            for _ in range(100):
                if (root / "scan.txt.next").exists():
                    break
                time.sleep(0.01)
            process.terminate()
        stdout, stderr = process.communicate(timeout=3)
        success = scenario in ("success", "single-down", "single-up", "single-claimed")
        assert process.returncode == (0 if success else 1), (scenario, stdout, stderr)
        assert (root / "deleted").exists() == (not scenario.startswith("single-") and scenario != "missing"), scenario
        if scenario.startswith("single-"):
            assert (root / "up").exists() == (scenario != "single-up"), scenario
            assert (root / "down").exists() == (scenario not in ("single-up", "single-claimed")), scenario
        assert not (root / "scan.request").exists() and not (root / "scan.pending").exists(), scenario
        if success:
            assert "SSID: Test" in (root / "scan.txt").read_text()
            assert not (root / "scan.error").exists()
        elif scenario != "cancelled":
            assert (root / "scan.error").stat().st_size > 0
            assert (root / "scan.error").stat().st_size <= 131072
        print("PASS: Wi-Fi scan", scenario)
