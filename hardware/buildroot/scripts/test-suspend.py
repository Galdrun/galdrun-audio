"""Check suspend recovery order and cleanup against mocked board services."""
from pathlib import Path
import os
import subprocess
import tempfile

source = Path(__file__).resolve().parents[1] / "board/connector2/raumfeld-suspend"
with tempfile.TemporaryDirectory(prefix="raumfeld-suspend-") as directory:
    root = Path(directory)
    script = root / "suspend"
    text = source.read_text().replace("/sys/power/", str(root) + "/")
    text = text.replace("/etc/init.d/S95galdrun-client", str(root / "client"))
    text = text.replace("/etc/init.d/S96raumfeld-status-leds", str(root / "led-monitor"))
    text = text.replace("/sys/class/leds/", str(root / "leds") + "/")
    text = text.replace("/run/", str(root) + "/")
    text = text.replace("/sys/class/net/", str(root / "net") + "/")
    text = text.replace("/sys/module/", str(root / "module") + "/")
    text = text.replace("/etc/init.d/S45raumfeld-network", str(root / "network"))
    script.write_text(text)
    script.chmod(0o755)
    for name, body in {
        "id": "echo 0",
        "sync": 'echo sync >> "$MOCK_ROOT/events"',
        "logger": 'echo "logger $*" >> "$MOCK_ROOT/events"',
        "sleep": '''if [ "$1" = 0.15 ]; then
echo "leds $(cat "$MOCK_ROOT/leds/raumfeld:1/brightness") $(cat "$MOCK_ROOT/leds/raumfeld:2/brightness")" >> "$MOCK_ROOT/events"
fi''',
        "chronyc": '''echo "chronyc $*" >> "$MOCK_ROOT/events"
[ "$1" != waitsync ] || [ "$MOCK_SYNC_FAIL" = 0 ]''',
        "client": '''echo "client $1" >> "$MOCK_ROOT/events"
case "$1" in
status) [ -f "$MOCK_ROOT/running" ] ;;
stop) rm -f "$MOCK_ROOT/running" ;;
start) touch "$MOCK_ROOT/running" ;;
esac''',
        "led-monitor": '''echo "monitor $1" >> "$MOCK_ROOT/events"
case "$1" in
status) [ -f "$MOCK_ROOT/monitor-running" ] ;;
stop) rm -f "$MOCK_ROOT/monitor-running" ;;
start) touch "$MOCK_ROOT/monitor-running" ;;
esac''',
        "ethtool": '''if [ "$1" = -s ]; then echo "ethtool $*" >> "$MOCK_ROOT/events"; else echo '	Supports Wake-on: g'; fi''',
        "rmmod": 'echo "rmmod $*" >> "$MOCK_ROOT/events"',
        "modprobe": 'echo "modprobe $*" >> "$MOCK_ROOT/events"',
        "network": 'echo "network $1" >> "$MOCK_ROOT/events"',
        "curl": '''case "$*" in
*getPower*) printf '{"ok":true,"state":"%s"}\\n' "$MOCK_POWER" ;;
*standbyOn*) echo standbyOn >> "$MOCK_ROOT/events"; echo '{"ok":true,"state":"standbyManual"}' ;;
*) exit 1 ;;
esac''',
    }.items():
        path = root / name
        path.write_text("#!/bin/sh\n" + body + "\n")
        path.chmod(0o755)
    env = dict(os.environ, MOCK_ROOT=str(root), PATH=f"{root}:/usr/bin:/bin")
    # A wired link that can wake, and Wi-Fi beside it.
    for name, files in {"eth0": {"type": "1", "carrier": "1", "operstate": "up"},
                        "mlan0": {"type": "1", "operstate": "down"}}.items():
        (root / "net" / name).mkdir(parents=True)
        for file, value in files.items():
            (root / "net" / name / file).write_text(value + "\n")
    (root / "net" / "mlan0" / "phy80211").mkdir()
    (root / "module" / "mwifiex_sdio").mkdir(parents=True)
    for name in ("raumfeld:1", "raumfeld:2"):
        led = root / "leds" / name
        led.mkdir(parents=True)
        (led / "brightness").write_text("0\n")
        (led / "trigger").write_text("none\n")
    for power, sync_fail, write_fail, running in [
        ("standbyManual", False, False, True),
        ("on", False, False, True),
        ("standbyManual", True, False, True),
        ("standbyManual", False, True, True),
        ("on", False, False, False),
    ]:
        (root / "events").write_text("")
        wifi_used = power == "on"
        (root / "net" / "mlan0" / "operstate").write_text("up\n" if wifi_used else "down\n")
        (root / "mem_sleep").write_text("s2idle shallow [deep]\n")
        state = root / "state"
        if state.exists():
            if state.is_dir():
                state.rmdir()
            else:
                state.unlink()
        if write_fail:
            state.mkdir()
        else:
            state.write_text("")
        marker = root / "running"
        (root / "monitor-running").touch()
        marker.unlink(missing_ok=True)
        if running:
            marker.touch()
        result = subprocess.run([str(script)], env=dict(env,
            MOCK_POWER=power, MOCK_SYNC_FAIL=str(int(sync_fail))),
            capture_output=True, text=True, timeout=5)
        assert (result.returncode != 0) == (sync_fail or write_fail), result.stderr
        events = (root / "events").read_text().splitlines()
        assert marker.exists() == running, events
        assert (root / "monitor-running").exists(), events
        assert not (root / "raumfeld-suspend.lock").exists(), events
        assert [event for event in events if event.startswith("leds ")] == ["leds 1 0", "leds 0 1", "leds 1 0", "leds 0 1"], events
        assert events.index("monitor stop") < events.index("leds 1 0"), events
        assert events.index("chronyc offline") < events.index("sync"), events
        assert events.index("chronyc reset sources") < events.index("chronyc makestep 0.001 3"), events
        assert events.index("chronyc makestep 0.001 3") < events.index("chronyc online"), events
        if running:
            assert events.index("client stop") < events.index("chronyc offline"), events
            assert events.index("chronyc waitsync 30 0.01 0 1") < events.index("client start"), events
        assert ("standbyOn" in events) == (running and power == "standbyManual"), events
        # Wi-Fi is unloaded and Wake-on-LAN armed (cleared first) before sleep;
        # both are undone afterwards, failure or not.
        assert events.index("rmmod mwifiex_sdio mwifiex") < events.index("sync"), events
        assert events.index("ethtool -s eth0 wol d") < events.index("ethtool -s eth0 wol g") < events.index("sync"), events
        assert events.index("sync") < len(events) - 1 - events[::-1].index("ethtool -s eth0 wol d"), events
        assert events.index("sync") < events.index("modprobe mwifiex_sdio"), events
        assert ("network restart" in events) == wifi_used, events
    print("PASS: left/right/left/right precedes sleep; Wi-Fi unload and Wake-on-LAN arming; clock recovery and failure cleanup restore services/manual standby")
