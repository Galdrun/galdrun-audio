"""Exercise the actual POSIX network supervisor with isolated mocked hardware."""
from pathlib import Path
import os
import signal
import subprocess
import tempfile


def check(scenario="baseline"):
    source = Path(__file__).resolve().parents[1] / "board/common/overlay"
    with tempfile.TemporaryDirectory(prefix="raumfeld-network-test-") as directory:
        root = Path(directory)
        no_ethernet = scenario in ("no-ethernet-ap", "wifi-no-ethernet")
        if not no_ethernet:
            (root / "sys/eth0").mkdir(parents=True)
        (root / "sys/wlan0/phy80211").mkdir(parents=True)
        (root / "sys/wlan0/address").write_text("02:11:22:33:44:00\n")
        if scenario == "wifi-radio-hotplug":
            (root / "sys/wlan1/phy80211").mkdir(parents=True)
            (root / "sys/wlan1/address").write_text("02:11:22:33:44:01\n")
        if scenario == "wifi-station-only":
            (root / "sys/wlan0/phy80211/name").write_text("phy0\n")
        if not no_ethernet:
            (root / "sys/eth0/carrier").write_text("1\n" if scenario in ("ethernet-static", "reload", "ethernet-no-lease") else "0\n")
        (root / "step").write_text("0")
        config = root / "network.conf"
        config.write_text((source / "etc/raumfeld-network.conf").read_text())
        (root / "hostname").write_text("connector2\n")
        user_config = root / "galdrun-network.conf"
        user_config.write_text((source / "etc/galdrun-network.conf").read_text())
        if scenario == "wifi-radio-hotplug":
            user_config.write_text(user_config.read_text().replace("WIFI_RADIO=\n", "WIFI_RADIO=02:11:22:33:44:01\n"))
        if scenario in ("wifi-dhcp", "reload"):
            user_config.write_text(user_config.read_text().replace("DEVICE_NAME=\n", "DEVICE_NAME=kitchen-connector\n"))
        if scenario in ("ethernet-10", "ethernet-100"):
            user_config.write_text(user_config.read_text().replace("ETH_SPEED=auto", "ETH_SPEED=" + scenario.split("-")[1]))
        if scenario == "ethernet-static":
            user_config.write_text(user_config.read_text().replace("ETH_IP_MODE=dhcp", "ETH_IP_MODE=static")
                                   .replace("ETH_IP=\n", "ETH_IP=10.0.0.38\n")
                                   .replace("ETH_GATEWAY=\n", "ETH_GATEWAY=10.0.0.1\n")
                                   .replace("ETH_DNS=\n", "ETH_DNS=1.1.1.1\n"))
        if scenario.startswith("wifi-"):
            user_config.write_text(user_config.read_text().replace("WIFI_SSID=\n", "WIFI_SSID=test'$(touch BAD)`id`=wifi\n")
                                   .replace("WIFI_PASSWORD=\n", "WIFI_PASSWORD==secret'$(touch BAD)=\n")
                                   .replace("WIFI_BSSID=\n", "WIFI_BSSID=aa:bb:cc:dd:ee:01\n"))
        if scenario == "wifi-static":
            user_config.write_text(user_config.read_text().replace("WIFI_IP_MODE=dhcp", "WIFI_IP_MODE=static")
                                   .replace("WIFI_IP=\n", "WIFI_IP=10.0.0.39\n")
                                   .replace("WIFI_GATEWAY=\n", "WIFI_GATEWAY=10.0.0.1\n"))
        script = (source / "usr/sbin/raumfeld-network").read_text()
        if scenario == "wifi-wpa-exit":
            # BusyBox ash accepts an empty PID as process group 0; reproduce that on host shells.
            script = '''kill() {
    if [ "$1" = -0 ] && [ "$2" = "" ]; then return 0; fi
    command kill "$@"
}
''' + script
        for original, replacement in (("/etc/raumfeld-network.conf", str(config)),
                                      ("/etc/galdrun-network.conf", str(user_config)),
                                      ("/etc/hostname", str(root / "hostname")),
                                      ("/etc/resolv.conf", str(root / "resolv.conf")),
                                      ("/usr/sbin/raumfeld-captive-portal", str(root / "bin/raumfeld-captive-portal")),
                                      ("/sys/class/net", str(root / "sys")),
                                      ("/run/raumfeld-network", str(root / "run")),
                                      ("/run/hostapd", str(root / "hostapd"))):
            if original not in script:
                raise ValueError("Supervisor changed; update test isolation")
            script = script.replace(original, replacement)
        (root / "network.sh").write_text(script)
        binary = root / "bin"
        binary.mkdir()
        mock = binary / "mock"
        mock.write_text('''#!/usr/bin/python3
import os,sys,time,subprocess,signal
from pathlib import Path
r=Path(os.environ['MOCK_ROOT']); scenario=os.environ['MOCK_SCENARIO']; name=Path(sys.argv[0]).name; args=sys.argv[1:]
with (r/'events').open('a') as f:f.write(name+' '+ ' '.join(args)+'\\n')
step=int((r/'step').read_text())
if name=='hostname':
 if args:(r/'current-hostname').write_text(args[0])
 else:print((r/'current-hostname').read_text())
elif name in ('udhcpc','raumfeld-captive-portal'):
 if name=='raumfeld-captive-portal':
  with (r/'children').open('a') as f:f.write(str(os.getpid())+'\\n')
 time.sleep(120)
elif name=='ip' and 'addr' in args:
 interface=args[args.index('dev')+1]; stored=r/(interface+'-address')
 if 'replace' in args:stored.write_text(args[args.index('replace')+1])
 elif 'flush' in args:stored.unlink(missing_ok=True)
 elif 'show' in args:
  address=stored.read_text() if stored.exists() else ''
  if interface=='eth0' and ((scenario=='baseline' and step==2) or scenario=='reload'):address='10.0.0.38/24'
  if interface=='wlan0' and scenario in ('wifi-dhcp','wifi-wpa-exit','wifi-no-ethernet') and step>=1 and (r/'run/wpa.pid').exists():address='10.0.0.39/24'
  if address:print('2: '+interface+' inet '+address+' scope global '+interface)
elif name=='iw' and 'link' in args:
 if scenario in ('wifi-static','wifi-dhcp','wifi-wpa-exit','wifi-no-ethernet') and (r/'run/wpa.pid').exists():print('Connected to 00:11:22:33:44:55')
elif name=='wpa_passphrase':
 assert sys.stdin.read()=="=secret'$(touch BAD)=\\n", 'Password changed while reading config'
 print('psk='+'a'*64)
elif name in ('hostapd','dnsmasq','wpa_supplicant'):
 p=subprocess.Popen(['/usr/bin/python3','-c','import time; time.sleep(120)'],start_new_session=True)
 with (r/'children').open('a') as f:f.write(str(p.pid)+'\\n')
 if name in ('hostapd','wpa_supplicant'):pidfile=Path(args[args.index('-P')+1])
 else:pidfile=r/'run/dnsmasq.pid'
 pidfile.write_text(str(p.pid))
elif name=='sleep':
 if args==['2']:time.sleep(0.05);sys.exit(0)
 step+=1
 (r/'step').write_text(str(step))
 if scenario=='wifi-radio-hotplug':
  if step==1:(r/'sys/wlan1/phy80211').rmdir()
  if step==2:(r/'sys/wlan1/phy80211').mkdir()
 if scenario=='wifi-wpa-exit' and step==1:
  p=r/'run/wpa.pid';os.kill(int(p.read_text()),signal.SIGTERM);p.unlink()
 if scenario=='baseline':(r/'sys/eth0/carrier').write_text(('1' if step in (1,2) else '0')+'\\n')
 if scenario=='reload' and step==1:
  p=r/'galdrun-network.conf';p.write_text(p.read_text().replace('ETH_IP_MODE=dhcp','ETH_IP_MODE=static').replace('ETH_IP=\\n','ETH_IP=10.0.0.38\\n').replace('DEVICE_NAME=kitchen-connector','DEVICE_NAME='))
 if step>={'wifi-failed':8,'wifi-station-only':8,'ethernet-no-lease':12}.get(scenario,4):os.kill(os.getppid(),signal.SIGTERM)
 time.sleep(0.05)
''')
        mock.chmod(0o755)
        for name in ("ip", "iw", "ethtool", "logger", "hostapd", "dnsmasq", "udhcpc", "sleep", "wpa_supplicant", "wpa_passphrase", "hostname", "raumfeld-captive-portal"):
            (binary / name).symlink_to(mock)
        try:
            env = dict(os.environ, MOCK_ROOT=str(root), MOCK_SCENARIO=scenario, PATH=f"{binary}:/usr/bin:/bin")
            result = subprocess.run(["/bin/sh", str(root / "network.sh")], env=env,
                                    capture_output=True, text=True, timeout=10)
            if result.returncode != 0:
                raise ValueError(result.stderr)
            events = (root / "events").read_text().splitlines()
            if any(e.startswith("hostapd ") for e in events):
                assert "raumfeld-captive-portal 192.168.77.1 80" in events, events
                dns = (root / "run/dnsmasq.conf").read_text()
                for expected in ("port=53", "listen-address=192.168.77.1", "except-interface=lo", "no-resolv", "no-hosts", "local=/#/",
                                 "address=/#/192.168.77.1", "dhcp-option=option:router,192.168.77.1",
                                 "dhcp-option=option:dns-server,192.168.77.1"):
                    assert expected in dns.splitlines(), dns
            else:
                assert not any(e.startswith("raumfeld-captive-portal ") for e in events), events
            expected_name = "kitchen-connector" if scenario in ("wifi-dhcp", "reload") else "connector2"
            assert f"hostname {expected_name}" in events, events
            assert all(f"-x hostname:{expected_name}" in e for e in events if e.startswith("udhcpc ")), events
            if scenario == "reload":
                assert (root / "current-hostname").read_text() == "connector2", events
            if no_ethernet:
                assert not any(e.startswith('ethtool ') or 'eth0' in e for e in events), events
            elif scenario in ("ethernet-10", "ethernet-100"):
                mask = {"ethernet-10": "0x003", "ethernet-100": "0x00c"}[scenario]
                assert f'ethtool -s eth0 autoneg on advertise {mask}' in events, events
            else:
                assert 'ethtool --disable-netlink -s eth0 autoneg on' in events, events
                assert not any(e.startswith('ethtool ') and 'advertise' in e for e in events), events
            # Link up but no lease for 30 s: renegotiate once, then wait two minutes.
            renegotiations = events.count("ethtool -r eth0")
            assert renegotiations == (1 if scenario == "ethernet-no-lease" else 0), events
            if scenario == "ethernet-no-lease":
                assert any("renegotiating" in e for e in events), events
                assert sum(e.startswith("udhcpc ") and "-i eth0 " in e for e in events) == 1, events
            elif scenario == "baseline":
                assert sum(e.startswith("hostapd ") for e in events) == 2, events
                assert sum(e.startswith("dnsmasq ") for e in events) == 2, events
                assert sum(e.startswith("udhcpc ") for e in events) == 1, events
                assert any("Ethernet DHCP ready; AP disabled" in e for e in events), events
                assert any(e == "ip -4 addr flush dev eth0" for e in events), events
            elif scenario == "ethernet-static":
                assert "ip addr replace 10.0.0.38/24 dev eth0" in events, events
                assert "ip route replace default via 10.0.0.1 dev eth0" in events, events
                assert (root / "resolv.conf").read_text() == "nameserver 1.1.1.1 # eth0\n"
                assert not any(e.startswith(("hostapd ", "udhcpc ")) for e in events), events
            elif scenario == "reload":
                assert events.index("sleep 2") < events.index("ip addr replace 10.0.0.38/24 dev eth0"), events
                assert any("Applying saved network settings" in e for e in events), events
            elif scenario in ("ethernet-10", "ethernet-100"):
                assert not any(e.startswith("wpa_supplicant ") for e in events), events
            elif scenario == "no-ethernet-ap":
                assert sum(e.startswith("hostapd ") for e in events) == 1, events
                assert not any(e.startswith(("udhcpc ", "wpa_supplicant ")) for e in events), events
            else:
                assert any(e.startswith("wpa_supplicant ") for e in events), events
                config_text = (root / "run/wpa.conf").read_text()
                assert " bssid=aa:bb:cc:dd:ee:01" in config_text, config_text
                assert "ssid=" + "test'$(touch BAD)`id`=wifi".encode().hex() in config_text, config_text
                assert "secret" not in config_text and "secret" not in "\n".join(events)
                assert not (root / "BAD").exists()
                if scenario == "wifi-static":
                    assert "ip addr replace 10.0.0.39/24 dev wlan0" in events, events
                    assert not any(e.startswith(("hostapd ", "udhcpc ")) for e in events), events
                elif scenario in ("wifi-dhcp", "wifi-wpa-exit", "wifi-no-ethernet"):
                    assert any(e.startswith("udhcpc ") and "-i wlan0 " in e for e in events), events
                    assert not any(e.startswith("hostapd ") for e in events), events
                    if scenario == "wifi-wpa-exit":
                        assert sum(e.startswith("wpa_supplicant ") for e in events) == 2, events
                elif scenario == "wifi-station-only":
                    assert not any(e.startswith('hostapd ') for e in events), events
                    assert sum(e.startswith('wpa_supplicant ') for e in events) == 1, events
                    assert (root / 'run/ap-supported').read_text() == '0\n'
                elif scenario == "wifi-radio-hotplug":
                    stations = [e.split(' -i ')[1].split()[0] for e in events if e.startswith('wpa_supplicant ')]
                    assert stations == ['wlan1', 'wlan0', 'wlan1'], events
                    assert 'ip -4 route flush dev wlan1' in events, events
                    assert 'ip -4 route flush dev wlan0' in events, events
                    assert not any(e.startswith('hostapd ') for e in events), events
                    assert (root / 'run/interface').read_text().strip() == 'wlan1'
                else:
                    assert any(e.startswith("hostapd ") for e in events), events
            assert not (root / "run/hostapd.pid").exists()
            assert not (root / "run/dnsmasq.pid").exists()
            assert not (root / "run/wpa.pid").exists()
            print("PASS:", scenario)
        finally:
            if (root / "children").exists():
                for pid in (root / "children").read_text().splitlines():
                    try:
                        os.kill(int(pid), signal.SIGTERM)
                    except ProcessLookupError:
                        pass


if __name__ == "__main__":
    for scenario in ("baseline", "ethernet-static", "ethernet-10", "ethernet-100", "ethernet-no-lease", "wifi-dhcp", "wifi-static",
                     "wifi-failed", "wifi-wpa-exit", "wifi-radio-hotplug", "wifi-station-only", "reload",
                     "no-ethernet-ap", "wifi-no-ethernet"):
        check(scenario)
