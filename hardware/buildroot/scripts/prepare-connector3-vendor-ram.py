"""Prepare the built RAM userland on a Streamer running the SSH-enabled beta.

Linux/WSL: python3 prepare-connector3-vendor-ram.py IP KNOWN_HOSTS PRIVATE_SETUP_JSON
Only prepares /run; prints the separate command that starts the network handover.
No vendor binaries are copied into the image and no persistent service is installed.
"""
from pathlib import Path
import hashlib
import ipaddress
import json
import subprocess
import sys

project = Path(__file__).resolve().parents[1]
host = str(ipaddress.IPv4Address(sys.argv[1]))
known_hosts = Path(sys.argv[2]).resolve()
setup = json.loads(Path(sys.argv[3]).read_text(encoding="utf-8-sig"))
archive = project / "output/connector3-ram/rootfs.tar.gz"
root = "/run/galdrun-connector3"
ssh = ["ssh", "-o", "BatchMode=yes", "-o", "StrictHostKeyChecking=yes",
       "-o", f"UserKnownHostsFile={known_hosts}", "-o", "ConnectTimeout=5", f"root@{host}"]


def remote(command, **kwargs):
    result = subprocess.run([*ssh, "set -e\n" + command], capture_output=True, timeout=120, **kwargs)
    if result.returncode:
        raise RuntimeError(result.stderr.decode(errors="replace"))
    return result.stdout


def upload(path, data, mode="600"):
    remote(f"umask 077; cat > {path}; chmod {mode} {path}", input=data)


model = remote("cat /sys/firmware/devicetree/base/model").decode().strip("\0\n")
if "Teufel Streamer" not in model or remote("uname -r").strip() != b"4.1.45":
    raise SystemExit("This helper requires the inspected Streamer beta kernel (4.1.45).")
if b"tmpfs /run tmpfs" not in remote("cat /proc/mounts"):
    raise SystemExit("/run must be tmpfs")
remote(f"if [ -e {root} ]; then echo 'RAM root already exists; use a fresh boot.' >&2; exit 1; fi")

ssid, password = setup["ssid"], setup.get("password", "")
if not (0 < len(ssid.encode()) <= 32) or (password and not 8 <= len(password.encode()) <= 63):
    raise SystemExit("Invalid private Wi-Fi settings")
if any(c in ssid + password for c in "\r\n\0"):
    raise SystemExit("Wi-Fi settings must be single-line values")
remote(f"umask 077; mkdir -p {root}; chmod 700 {root}")
blob = archive.read_bytes()
upload(f"{root}.tar.gz", blob)
if hashlib.sha256(remote(f"cat {root}.tar.gz")).digest() != hashlib.sha256(blob).digest():
    raise SystemExit("RAM archive readback hash mismatch")
remote(f"zcat {root}.tar.gz | tar xf - -C {root}; chmod 700 {root}")
remote(f"mkdir -p {root}/run/dbus; "
       f"mount -o bind /dev {root}/dev; mount -o bind /proc {root}/proc; "
       f"mount -o bind /sys {root}/sys; mount -o bind /var/run/dbus {root}/run/dbus")

network = (project / "board/common/overlay/etc/galdrun-network.conf").read_text()
network = network.replace("WIFI_SSID=\n", f"WIFI_SSID={ssid}\n")
network = network.replace("WIFI_PASSWORD=\n", f"WIFI_PASSWORD={password}\n")
upload(f"{root}/etc/galdrun-network.conf", network.encode())
existing = remote("if [ -f /run/galdrun-stock-runtime/client.json ]; then "
                  "cat /run/galdrun-stock-runtime/client.json; else echo '{}'; fi")
config = json.loads(existing)
previous = project / "reference/connector3-ssh-archive/stock-runtime-status.json"
if not config.get("clientId") and previous.exists():
    config["clientId"] = json.loads(previous.read_text(encoding="utf-8-sig")).get("status", {}).get("clientId", "")
config.setdefault("active", True)
config.setdefault("audioDeviceId", "hw:CARD=Audio,DEV=1")
config.setdefault("name", "Connector 3 RAM test")
config.setdefault("standbyAfterSeconds", 0)
config.setdefault("deepSleepAfterSeconds", 0)
config["volumeControl"] = "software"
inputs = config.setdefault("inputs", [])
touch = "Connector 3 touch buttons"
definitions = [("Preset 1", "2", "action1"), ("Preset 2", "3", "action2"),
               ("Preset 3", "4", "action3"), ("Previous", "165", "previous"),
               ("Play/Pause", "164", "togglePause"), ("Next", "163", "next"),
               ("Multipurpose", "148", "action4"), ("Volume -", "114", "volumeDown"),
               ("Volume +", "115", "volumeUp")]
for label, key, action in definitions:
    if any(item.get("device") == touch and item.get("key") == key for item in inputs):
        continue
    binding = {"action": action}
    if action.startswith("volume"):
        binding.update(scope="room", step=0.05)
    inputs.append({"id": label, "kind": "button", "device": touch, "key": key,
                   "bindings": {"click": binding}})
upload(f"{root}/run/client.json", json.dumps(config).encode())

enter = f"{root}/lib/ld-linux-armhf.so.3 --library-path {root}/lib:{root}/usr/lib {root}/bin/busybox chroot {root}"
print(remote(f"{enter} /usr/sbin/galdrun-connector3-buttons --self-test").decode().strip())
print(remote(f"{enter} /usr/bin/env LD_LIBRARY_PATH=/opt/galdrun-client/lib /opt/galdrun-client/galdrun-client --version").decode().strip())
remote(f"{enter} /bin/sh -c 'export PATH=/usr/sbin:/usr/bin:/sbin:/bin; "
       "for tool in ip iw wpa_supplicant wpa_passphrase ethtool udhcpc start-stop-daemon hostapd dnsmasq; "
       "do command -v \"$tool\" >/dev/null || exit 1; done'")

start = r'''#!/bin/sh
set -eu
export PATH=/usr/sbin:/usr/bin:/sbin:/bin
root=/run/galdrun-connector3
enter() {
    "$root/lib/ld-linux-armhf.so.3" --library-path "$root/lib:$root/usr/lib" \
        "$root/bin/busybox" chroot "$root" "$@"
}
# A host-side recovery still works if the new network manager or client fails.
(
    sleep 120
    if [ ! -f "$root/run/verified" ]; then
        echo 'RAM network handover was not verified; restoring ConnMan.'
        enter /etc/init.d/S45raumfeld-network stop || :
        killall wpa_supplicant 2>/dev/null || :
        /etc/init.d/S45connman start
        sleep 40
        if ! busybox ip -4 -o addr show dev wlan0 scope global | grep -q ' inet '; then
            sync
            /sbin/reboot
        fi
    fi
) >"$root/run/recovery.log" 2>&1 &
for service in client buttons; do
    if [ -f "$root/run/$service.pid" ]; then
        kill -TERM "$(cat "$root/run/$service.pid")" 2>/dev/null || :
    fi
done
sleep 1
printf '%s\n' '{"board":"connector3-ram","networkSettings":true}' >"$root/etc/galdrun-board.json"
if [ -f /run/galdrun-stock-runtime/client.pid ]; then
    kill -TERM "$(cat /run/galdrun-stock-runtime/client.pid)" 2>/dev/null || :
fi
echo 'Stopping vendor connection manager'
/etc/init.d/S45connman stop
killall wpa_supplicant 2>/dev/null || :
sleep 2
echo 'Starting Galdrun connection manager'
enter /bin/sh -c 'echo $$ >/run/raumfeld-network.pid; exec /usr/sbin/raumfeld-network' \
    >"$root/run/network.log" 2>&1 &
echo 'Starting touch bridge'
enter /usr/sbin/galdrun-connector3-buttons >"$root/run/buttons.log" 2>&1 &
echo $! >"$root/run/buttons.pid"
sleep 2
echo 'Starting client'
enter /bin/sh -c 'export LD_LIBRARY_PATH=/opt/galdrun-client/lib; exec /opt/galdrun-client/galdrun-client --config /run/client.json --audio-cache /run/audio_devices.json --log-file /run/client.log' \
    >"$root/run/client-console.log" 2>&1 &
echo $! >"$root/run/client.pid"
'''
upload(f"{root}/start.sh", start.encode(), "700")
print("Prepared RAM userland; existing networking is still running.")
print(f"Start via SSH: nohup {root}/start.sh >{root}/run/start.log 2>&1 </dev/null &")
print(f"After verifying the client API, create {root}/run/verified within 120 seconds.")
