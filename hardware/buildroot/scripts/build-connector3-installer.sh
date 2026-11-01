#!/usr/bin/env bash
# Default is a controller-protected check/backup USB image. No hardware access.
set -euo pipefail
project=$(cd -- "$(dirname -- "$0")/.." && pwd)
[[ $# -ge 2 && $# -le 3 ]] || { echo "Usage: $0 <SSH public key> <FIT signing-key directory> [--write]" >&2; exit 2; }
export GALDRUN_SSH_PUBKEY GALDRUN_CONNECTOR3_INSTALL_WRITE=0
GALDRUN_SSH_PUBKEY=$(realpath -- "$1")
signing=$(realpath -- "$2")
[[ $# == 2 || $3 == --write ]] || exit 2
if [[ $# == 3 ]]; then
    GALDRUN_CONNECTOR3_INSTALL_WRITE=1
fi
source "$project/PrepareHost.sh"
jobs=${JOBS:-16}
[[ $jobs =~ ^[0-9]+$ && $jobs -ge 1 && $jobs -le 32 ]] || exit 2
exec 9>"$work/build.lock"
flock -n 9 || { echo 'Another firmware build is running.' >&2; exit 1; }
mode=check
[[ $GALDRUN_CONNECTOR3_INSTALL_WRITE == 0 ]] || mode=write
output="$work/connector3-installer-$mode"
destination="$project/private/connector3/installer-$mode"
persistent="$project/output/connector3-persistent"
rootfs="$project/output/connector3-persistent-rootfs"
export GALDRUN_CONNECTOR3_INSTALL_INPUTS="$output/inputs"
mkdir -p "$destination" "$output/inputs" "$project/logs" "$work/external"
python3 - "$persistent" "$rootfs" "$output/inputs" "$project" "$mode" <<'PY'
from pathlib import Path
import hashlib,json,sys,tarfile
persistent,rootfs,inputs,project=map(Path,sys.argv[1:5]);mode=sys.argv[5]
report=json.loads((persistent/'persistent-verification.json').read_text())
if not report['signature_verified'] or report['protected_mtd']!=[0,1,2,3,4]:
    raise ValueError('Verified persistent image and factory protection required')
if hashlib.sha256((persistent/'galdrun.itb').read_bytes()).hexdigest()!=report['fit_sha256']:
    raise ValueError('Persistent FIT changed after verification')
if hashlib.sha256((persistent/'rootfs.ubi').read_bytes()).hexdigest()!=report['rootfs']['artifacts_sha256']['rootfs.ubi']:
    raise ValueError('Persistent rootfs changed after verification')
if mode=='write':
    qualification=project/'output/connector3-monitor-production/hardware-verification.json'
    if not qualification.is_file():
        raise ValueError('Write-capable USB images require a hardware-qualified production monitor')
    monitor=json.loads(qualification.read_text())
    if not monitor['production_qualified'] or monitor['sha256']!=report['monitor_sha256']:
        raise ValueError('Persistent FIT must use the hardware-qualified production monitor')
with tarfile.open(inputs/'payload.tar','w') as archive:
    for name in ('galdrun.itb','rootfs.ubi'):
        archive.add(persistent/name,arcname=name,recursive=False)
(inputs/'payload.tar.sha256').write_text(hashlib.sha256((inputs/'payload.tar').read_bytes()).hexdigest()+'  /run/payload.tar\n')
(inputs/'payload.sha256').write_text(''.join(hashlib.sha256((persistent/name).read_bytes()).hexdigest()+'  '+name+'\n' for name in ('galdrun.itb','rootfs.ubi')))
lines=[]
for number,name in enumerate(('rootfs.ubifs','settings.ubifs')):
    data=(rootfs/name).read_bytes()
    if hashlib.sha256(data).hexdigest()!=report['rootfs']['artifacts_sha256'][name]:
        raise ValueError('Volume readback input changed: '+name)
    lines.append(f'{number} {len(data)} {hashlib.sha256(data).hexdigest()}\n')
(inputs/'volume-readback').write_text(''.join(lines))
PY
rsync -a --delete --exclude downloads --exclude logs --exclude output --exclude reference --exclude private \
    "$project/" "$work/external/"
board="$work/external/board/connector3/installer"
chmod 0755 "$board/init.sh" "$board/post-build.sh" "$board/install-usb.sh"
args=(-C "$work/buildroot-2025.02.18" "O=$output" "BR2_EXTERNAL=$work/external" "BR2_DL_DIR=$project/downloads")
make "${args[@]}" "BR2_DEFCONFIG=$board/defconfig" defconfig >"$project/logs/connector3-installer-$mode.log" 2>&1
# Existing caches must re-extract OpenSSH when shared patches change.
ssh_patches=$(find "$work/external/board/common/patches/openssh" -type f -print0 | sort -z | xargs -0 sha256sum | sha256sum)
if [[ $(cat "$output/.openssh-patches" 2>/dev/null || true) != "$ssh_patches" ]]; then
    make "${args[@]}" openssh-dirclean >>"$project/logs/connector3-installer-$mode.log" 2>&1
fi
make "${args[@]}" legal-info >>"$project/logs/connector3-installer-$mode.log" 2>&1
make "${args[@]}" -j"$jobs" >>"$project/logs/connector3-installer-$mode.log" 2>&1
printf '%s\n' "$ssh_patches" >"$output/.openssh-patches"
python3 "$project/scripts/pack-connector3-installer.py" "$output" "$work/connector3-storage-check" \
    "$work/connector3-diagnostic" "$signing" "$destination" "$mode"
