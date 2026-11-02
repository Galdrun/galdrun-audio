"""Exercise suspend cleanup, manual standby and saved settings without hardware."""
from pathlib import Path
import subprocess
import tempfile


source = (Path(__file__).resolve().parents[1] / 'board/connector3/raumfeld-suspend').read_text()
subprocess.run(['sh', '-n'], input=source, text=True, check=True)
for scenario in ('ethernet', 'wifi', 'manual', 'kernel-error', 'stop-error', 'locked',
                 'radio', 'radio-kernel-error', 'radio-unbind-error', 'radio-bind-error'):
    with tempfile.TemporaryDirectory(prefix='connector3-suspend-') as directory:
        root = Path(directory)
        for name in ('run', 'settings', 'sys/power', 'sys/class/tty/ttymxc2/power',
                     'sys/class/net/eth0', 'sys/module/printk/parameters'):
            (root / name).mkdir(parents=True, exist_ok=True)
        config = root / 'settings/client.json'
        config.write_text('{"clientId":"saved-room","ledsOff":true}\n')
        before = config.read_bytes()
        (root / 'sys/power/mem_sleep').write_text('s2idle [deep]\n')
        state = root / 'sys/power/state'
        if 'kernel-error' in scenario:
            state.mkdir()
        else:
            state.write_text('')
        wake = root / 'sys/class/tty/ttymxc2/power/wakeup'
        wake.write_text('disabled\n')
        console = root / 'sys/module/printk/parameters/console_suspend'
        console.write_text('Y\n')
        (root / 'sys/class/net/eth0/type').write_text('1\n')
        (root / 'sys/class/net/eth0/carrier').write_text('0\n' if scenario == 'wifi' else '1\n')
        for pid in ('111', '222'):
            (root / ('run/pid-' + pid)).touch()
        if scenario == 'locked':
            (root / 'run/raumfeld-suspend.lock').mkdir()
        trace = root / 'trace'
        trace.touch()
        if scenario.startswith('radio'):
            driver = root / 'sys/bus/pci/drivers/mwifiex_pcie'
            driver.mkdir(parents=True)
            (driver / '0000:01:00.0').symlink_to('device')
            (root / 'sys/class/net/mlan0/phy80211').mkdir(parents=True)
        network = root / 'network-service'
        network.write_text(f'#!/bin/sh\nprintf "network %s\\n" "$1" >>"{trace}"\n')
        network.chmod(0o700)
        power = 'standbyManual' if scenario == 'manual' else 'on'
        harness = f'''
taskroot='{root}'
scenario='{scenario}'
trace() {{ printf '%s\\n' "$*" >>"$taskroot/trace"; }}
echo() {{
    if [ "$*" = 0000:01:00.0 ]; then
        driver="$taskroot/sys/bus/pci/drivers/mwifiex_pcie"
        if [ -L "$driver/$1" ]; then
            trace 'radio detach'
            rm "$driver/$1"
            rmdir "$taskroot/sys/class/net/mlan0/phy80211"
            [ "$scenario" != radio-unbind-error ] || return 1
        else
            trace 'radio bind'
            [ "$scenario" != radio-bind-error ] || return 1
            ln -s device "$driver/$1"
            mkdir "$taskroot/sys/class/net/mlan0/phy80211"
        fi
    fi
    printf '%s\\n' "$*"
}}
id() {{ echo 0; }}
pidof() {{ case "$1" in galdrun-client) echo 111;; galdrun-status-leds) echo 222;; esac; }}
kill() {{
    if [ "$1" = -TERM ]; then
        trace "stop $2"
        rm "$taskroot/run/pid-$2"
        [ "$scenario:$2" != stop-error:111 ]
    else
        test -f "$taskroot/run/pid-$2"
    fi
}}
nohup() {{
    trace "start $1"
    case "$1" in */galdrun-client) touch "$taskroot/run/pid-111";;
        */galdrun-status-leds) touch "$taskroot/run/pid-222";; esac
}}
wget() {{
    trace "api $*"
    printf '%s\\n' '{{"ok":true,"state":"{power}"}}'
}}
ethtool() {{
    if [ "$1" = -s ]; then trace "wol $2 $4";
    else printf 'Supports Wake-on: g\\nWake-on: d\\n'; fi
}}
sync() {{ :; }}
sleep() {{ :; }}
logger() {{ trace "error $*"; }}
'''
        command = source
        for prefix in ('/settings', '/run', '/sys'):
            command = command.replace(prefix, str(root) + prefix)
        command = command.replace('/etc/init.d/S45raumfeld-network', str(network))
        result = subprocess.run(['sh', '-c', harness + command], capture_output=True, text=True)
        expected_success = scenario in ('ethernet', 'wifi', 'manual', 'radio')
        assert (result.returncode == 0) == expected_success, (scenario, result.stderr)
        assert config.read_bytes() == before, scenario
        assert wake.read_text() == 'disabled\n', scenario
        assert console.read_text() == 'Y\n', scenario
        events = trace.read_text()
        if scenario == 'locked':
            assert 'stop ' not in events and 'start ' not in events, events
            continue
        assert not (root / 'run/raumfeld-suspend.lock').exists(), scenario
        assert 'start /opt/galdrun-client/galdrun-client' in events, (scenario, events)
        assert 'start /usr/sbin/galdrun-status-leds' in events, (scenario, events)
        if scenario == 'wifi':
            assert 'wol ' not in events, events
        elif scenario not in ('stop-error', 'radio-unbind-error'):
            assert events.count('wol eth0 d') == 2 and events.count('wol eth0 g') == 1, events
        if scenario == 'manual':
            assert 'standbyOn' in events, events
        if scenario.startswith('radio'):
            for event in ('network stop', 'radio detach', 'radio bind', 'network start'):
                assert event in events, (scenario, events)
        if expected_success:
            assert state.read_text() == 'mem\n', scenario
print('Connector 3 suspend helper: Ethernet/WLAN policy, manual standby, settings preservation and failure cleanup passed')
