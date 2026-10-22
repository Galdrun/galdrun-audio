#!/usr/bin/env bash
# linux.sh - Upload and start the matching Linux client release on a Raspberry Pi
# (DietPi/Dropbear) or any Linux reachable over SSH. Windows version: linux.ps1.
#
# Usage:
#   scripts/upload/linux.sh --ip 192.168.1.31 --user dietpi
#   scripts/upload/linux.sh --ip 192.168.1.31 --user dietpi --server 192.168.1.20:4711
#   scripts/upload/linux.sh --ip 192.168.1.31 --user dietpi --identity ~/.ssh/id_ed25519
#
# Options (missing --ip/--user are prompted for):
#   --ip        IPv4 address of the device.
#   --user      SSH username; needs sudo (DietPi's dietpi/root have passwordless sudo) for the
#               boot service and missing ALSA packages.
#   --server    Optional server address (host:port) the client connects to. Without it the
#               client keeps the server in its client.json, or a server attaches it after
#               finding its discovery beacon.
#   --identity  SSH private key for non-interactive logins. Without it, ssh prompts for the
#               password on each connection (2-3 times); it is never stored.
#
# What it does: detects the remote architecture (ARMv6/7/8/9, x86/x64), streams the matching
# releases/client/<platform> folder over SSH stdin, extracts it to ~/galdrun-client, installs
# ALSA when missing and installs galdrun-client.service (enabled at boot, restarted after
# crashes) using linux-remote.sh. Existing client.json and audio device cache are preserved.
# Output also goes to scripts/.run/upload-client-<IP>.log; on the device check /run/galdrun-client/client.log and journalctl -u galdrun-client.
set -euo pipefail
here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
root=$(cd -- "$here/../.." && pwd)
ip='' user='' server='' identity=''
while (( $# )); do
    case $1 in
        --ip) ip=${2:?}; shift 2 ;;
        --user) user=${2:?}; shift 2 ;;
        --server) server=${2:?}; shift 2 ;;
        --identity) identity=${2:?}; shift 2 ;;
        *) echo "Unknown option: $1 (see the top of $0)" >&2; exit 2 ;;
    esac
done
[[ -n $ip ]] || read -rp 'Device IP address: ' ip
[[ -n $user ]] || read -rp 'SSH username: ' user
[[ $ip =~ ^([0-9]{1,3}\.){3}[0-9]{1,3}$ ]] || { echo 'Enter a valid IPv4 address.' >&2; exit 2; }
[[ $user =~ ^[a-z_][a-z0-9_-]*$ ]] || { echo 'Enter a valid Linux username.' >&2; exit 2; }
if [[ -n $server ]] && ! [[ $server =~ ^[a-zA-Z0-9][a-zA-Z0-9.-]*:([0-9]{1,5})$ && ${BASH_REMATCH[1]} -ge 1 && ${BASH_REMATCH[1]} -le 65535 ]]; then
    echo 'Server must be hostname:port or IPv4:port.' >&2; exit 2
fi
target="$user@$ip"
ssh_options=(-o StrictHostKeyChecking=accept-new -o ConnectTimeout=10)
[[ -z $identity ]] || ssh_options+=(-i "$identity")

echo 'SSH will prompt for your password when needed; it is not stored.'
mapfile -t hardware < <(ssh "${ssh_options[@]}" "$target" 'uname -m; getconf LONG_BIT')
(( ${#hardware[@]} == 2 )) || { echo 'Could not detect the remote architecture.' >&2; exit 1; }
machine=${hardware[0]} bits=${hardware[1]}
case $machine in
    aarch64) if [[ $bits == 64 ]]; then platform=linux-armv8; else platform=linux-armv7; fi ;;
    armv6*) platform=linux-armv6 ;;
    armv7*|armv8l) platform=linux-armv7 ;;
    x86_64) if [[ $bits == 64 ]]; then platform=linux-x64; else platform=linux-x86; fi ;;
    i[3-6]86) platform=linux-x86 ;;
    *) echo "Unsupported remote architecture: $machine ($bits-bit)" >&2; exit 1 ;;
esac
release="$root/releases/client/$platform"
[[ -f $release/galdrun-client ]] || { echo "Build $platform first: $release" >&2; exit 1; }
echo "Uploading $platform to $target"

server_args=''
[[ -z $server ]] || server_args="--server $server "
remote=$(sed -e "s|@SERVER_ARGS@|$server_args|" -e "s|@USER@|$user|" "$here/linux-remote.sh")
encoded=$(printf '%s\n' "$remote" | base64 -w0)
remote_script=".galdrun-client-$$.sh"
# The script travels in the command; stdin carries the release archive.
command="umask 077; printf %s $encoded | base64 -d > $remote_script; bash $remote_script 2>&1; result=\$?; rm -f $remote_script; exit \$result"
mkdir -p "$root/scripts/.run"
log="$root/scripts/.run/upload-client-$ip.log"
tar --format ustar -czf - -C "$release" . | ssh "${ssh_options[@]}" "$target" "$command" | tee "$log"
if grep -q SERVICE_SUDO_REQUIRED "$log"; then
    echo 'Installing boot service: enter the SSH/sudo password when prompted.'
    ssh "${ssh_options[@]}" -tt "$target" 'sudo bash "$HOME/galdrun-client/install-service.sh"'
fi
echo 'Deployment complete.'
