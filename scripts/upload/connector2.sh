#!/usr/bin/env bash
# connector2.sh - Update the Galdrun client on a Raumfeld Connector 2 over SSH.
# Replaces the client only; settings, room ID and firmware stay. A new firmware is
# installed with the USB stick instead (hardware/buildroot/README.md). Windows version: connector2.ps1.
# SSH is off on the device until you turn it on in its settings page (Remote login).
#
# Usage:
#   scripts/upload/connector2.sh --ip 192.168.1.38
#   scripts/upload/connector2.sh --ip 192.168.77.1 --identity ~/keys/connector2
#
# Options (a missing --ip is prompted for):
#   --ip        IPv4 address of the Connector 2 (setup Wi-Fi: 192.168.77.1).
#   --user      SSH user (default root).
#   --identity  SSH private key; default hardware/buildroot/private/connector2/id_ed25519 when
#               it exists (its .pub built in via GALDRUN_SSH_PUBKEY). Without a key, ssh asks for the password
#               twice (copy, then install); it is never stored.
#
# What it does: copies releases/client/linux-armv6 (build it first: ./build-client.sh
# linux-armv6) and connector2-remote.sh to /tmp, checks that the new binary starts there, then
# stops the client, replaces binary, libraries and licenses, keeps the old binary as
# galdrun-client.previous and starts it again.
set -euo pipefail
here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
root=$(cd -- "$here/../.." && pwd)
ip='' user=root identity=''
while (( $# )); do
    case $1 in
        --ip) ip=${2:?}; shift 2 ;;
        --user) user=${2:?}; shift 2 ;;
        --identity) identity=${2:?}; shift 2 ;;
        *) echo "Unknown option: $1 (see the top of $0)" >&2; exit 2 ;;
    esac
done
[[ -n $ip ]] || read -rp 'Connector 2 IP address: ' ip
[[ $ip =~ ^([0-9]{1,3}\.){3}[0-9]{1,3}$ ]] || { echo 'Enter a valid IPv4 address.' >&2; exit 2; }
[[ $user =~ ^[a-z_][a-z0-9_-]*$ ]] || { echo 'Enter a valid Linux username.' >&2; exit 2; }
release="$root/releases/client/linux-armv6"
[[ -f $release/galdrun-client ]] || { echo 'Build linux-armv6 first: ./build-client.sh linux-armv6' >&2; exit 1; }
default_key="$root/hardware/buildroot/private/connector2/id_ed25519"
[[ -n $identity || ! -f $default_key ]] || identity=$default_key
ssh_options=(-o StrictHostKeyChecking=accept-new -o ConnectTimeout=10)
[[ -z $identity ]] || ssh_options+=(-i "$identity")
target="$user@$ip"

staging=$(mktemp -d)
trap 'rm -rf "$staging"' EXIT
tar --format ustar -cf "$staging/galdrun-client.tar" -C "$release" .
echo "Copying linux-armv6 client to $target ..."
scp "${ssh_options[@]}" "$staging/galdrun-client.tar" "$here/connector2-remote.sh" "$target:/tmp/"
ssh "${ssh_options[@]}" "$target" 'sh /tmp/connector2-remote.sh'
