#!/bin/sh
# Explicit check/backup/install/rollback commands. Never updates MTD0-MTD3.
set -eu
action=${1:---check}
case "$action" in --check|--backup|--install|--rollback) ;; *) echo 'Usage: galdrun-install --check|--backup|--install [BACKUP_SHA256]|--rollback BACKUP_SHA256' >&2; exit 2;; esac
fail() { echo "INSTALL REFUSED: $*" >&2; exit 1; }
backup=/media/galdrun-backup
model=$(tr -d '\000' </proc/device-tree/model)
case "$model" in
    'Galdrun Connector 3 installer check'|'Galdrun Connector 3 installer write'|'Galdrun Connector 3 read-only storage check') ;;
    *) fail "Unexpected model: $model";;
esac
[ "$(grep -c '^mtd[0-9]' /proc/mtd)" = 6 ] || fail 'Unexpected partition count'
number=0
for expected in 'u-boot:8388608' 'u-boot-env:524288' 'constants:524288' 'settings:134217728' 'fit:12582912' 'rootfs:380633088'; do
    base=/sys/class/mtd/mtd$number
    [ "$(cat "$base/name"):$(cat "$base/size")" = "$expected" ] || fail "Partition mismatch: mtd$number"
    for pair in erasesize:262144 writesize:4096 oobsize:256 ecc_strength:8 ecc_step_size:512; do
        [ "$(cat "$base/${pair%:*}")" = "${pair#*:}" ] || fail "Geometry mismatch: mtd$number $pair"
    done
    flags=$(cat "$base/flags")
    if [ "$number" -lt 4 ]; then
        [ $((flags & 0x400)) -eq 0 ] || fail "Factory partition mtd$number is not protected"
    fi
    [ "$(cat "$base/ecc_failures")" = 0 ] || fail "Uncorrectable ECC errors on mtd$number"
    number=$((number + 1))
done
for tool in sha256sum nanddump nandwrite flash_erase ubiformat ubiattach ubidetach head cmp; do
    command -v "$tool" >/dev/null || fail "Missing tool: $tool"
done
if grep -q ' ubifs ' /proc/mounts; then fail 'An installed filesystem is mounted'; fi
for device in /sys/class/ubi/ubi[0-9]*; do
    [ ! -e "$device/mtd_num" ] || fail 'An installed UBI device is attached'
done
if [ "$action" != --rollback ]; then
    (cd /payload && sha256sum -c /etc/galdrun-install.sha256) || fail 'Payload checksum mismatch'
    fit_size=$(wc -c </payload/galdrun.itb)
    [ "$fit_size" -gt 0 ] && [ "$fit_size" -le 12582912 ] || fail 'FIT exceeds MTD4'
    bad=$(cat /sys/class/mtd/mtd4/bad_blocks)
    bbt=$(cat /sys/class/mtd/mtd4/bbt_blocks)
    [ "$fit_size" -le $((12582912 - (bad + bbt) * 262144)) ] || fail 'FIT exceeds good MTD4 capacity'
    bad=$(cat /sys/class/mtd/mtd5/bad_blocks)
    bbt=$(cat /sys/class/mtd/mtd5/bbt_blocks)
    [ $((1452 - bad - bbt)) -ge $((768 + 128 + 2 + 2 + 34)) ] || fail 'Insufficient UBI bad-block reserve'
fi
echo 'PREFLIGHT PASSED: Connector 3 geometry, protected factory partitions and payload verified.'
[ "$action" != --check ] || exit 0

protected_hashes() {
    # Preserve bootloader bytes without applying the rootfs BCH layout to them.
    nanddump --quiet --noecc --omitoob --bb=skipbad /dev/mtd0 | sha256sum | awk '{print $1 "  bootloader-raw"}'
    sha256sum /dev/mtd1 /dev/mtd2 /dev/mtd3
}
verify_protected() {
    # Raw bootloader reads can contain transient bitflips: its ROM ECC layout
    # is not this kernel's BCH8 layout. Preserve the dump, never use it as an
    # exact identity check or restore target. MTD0 must stay write-protected.
    flags=$(cat /sys/class/mtd/mtd0/flags)
    [ $((flags & 0x400)) -eq 0 ] || return 1
    sha256sum /dev/mtd1 /dev/mtd2 /dev/mtd3 >/run/protected-current.sha256
    sed '/  bootloader-raw$/d' "${protected_saved:-$backup/protected.sha256}" >/run/protected-saved.sha256
    cmp /run/protected-saved.sha256 /run/protected-current.sha256
}
if [ "$action" = --backup ]; then
    grep -q ' /media vfat ' /proc/mounts || fail 'USB backup filesystem is not mounted'
    [ ! -e "$backup" ] || fail 'Existing backup must be retained; use another stick or archive it first'
    available=$(df -Pk /media | awk 'NR==2 {print $4}')
    [ "$available" -ge 614400 ] || fail 'Need at least 600 MiB free on USB'
    mount -o remount,rw /media || fail 'Cannot make USB writable for backups'
    mkdir -m 0700 "$backup"
    protected_hashes >"$backup/protected.sha256"
    for number in 0 1 2 3 4 5; do
        if [ "$number" = 0 ]; then
            nanddump --quiet --noecc --omitoob --bb=skipbad --file="$backup/mtd$number.bin" "/dev/mtd$number"
        else
            nanddump --quiet --omitoob --bb=skipbad --file="$backup/mtd$number.bin" "/dev/mtd$number"
        fi
    done
    number=0
    while read -r digest name; do
        if [ "$number" -gt 0 ]; then
            actual=$(sha256sum "$backup/mtd$number.bin" | cut -d ' ' -f 1)
            [ "$actual" = "$digest" ] || fail "Factory backup mtd$number differs from the independent read; retain it for investigation"
        fi
        number=$((number + 1))
    done <"$backup/protected.sha256"
    (cd "$backup" && sha256sum mtd0.bin mtd1.bin mtd2.bin mtd3.bin mtd4.bin mtd5.bin protected.sha256 >backup.sha256)
    sync
    sha256sum "$backup/backup.sha256"
    echo 'BACKUP COMPLETE: copy this manifest hash and the backup files to the host before any install.'
    exit 0
fi

pin=${2:-}
if [ -n "$pin" ] || [ "$action" = --rollback ]; then
    case "$pin" in ''|*[!0-9a-f]*) fail 'Supply the independently recorded backup manifest SHA256';; esac
    [ "${#pin}" = 64 ] || fail 'Invalid backup manifest SHA256'
    actual=$(sha256sum "$backup/backup.sha256" | cut -d ' ' -f 1)
    [ "$actual" = "$pin" ] || fail 'Backup manifest pin mismatch'
    (cd "$backup" && sha256sum -c backup.sha256) || fail 'Backup file checksum mismatch'
    verify_protected || fail 'Backup belongs to another device or factory state changed'
else
    # No backup, the normal install (like Connector 2): a failed write stops
    # here and the installer runs again from the stick; USB recovery is the
    # factory bootloader in MTD0, which nothing writes. The factory partitions
    # are still compared before and after.
    sha256sum /dev/mtd1 /dev/mtd2 /dev/mtd3 >/run/protected-before.sha256
    protected_saved=/run/protected-before.sha256
fi
[ -f /etc/galdrun-install-writes-enabled ] || fail 'This image is check/backup-only'
[ "$model" = 'Galdrun Connector 3 installer write' ] || fail 'A separate write-capable installer boot is required'
for number in 4 5; do
    flags=$(cat /sys/class/mtd/mtd$number/flags)
    [ $((flags & 0x400)) -ne 0 ] || fail "MTD$number remains write-protected"
done

write_raw() {
    number=$1
    image=$2
    length=$(wc -c <"$image")
    size=$(cat /sys/class/mtd/mtd$number/size)
    bad=$(cat /sys/class/mtd/mtd$number/bad_blocks)
    bbt=$(cat /sys/class/mtd/mtd$number/bbt_blocks)
    [ "$length" -le $((size - (bad + bbt) * 262144)) ] || fail "MTD$number no longer has capacity for the saved image"
    flash_erase -q "/dev/mtd$number" 0 0 || return 1
    nandwrite -q --pad "/dev/mtd$number" "$image" || return 1
    nanddump --quiet --omitoob --bb=skipbad --length="$length" "/dev/mtd$number" | sha256sum >/run/current.sha256
    expected=$(sha256sum "$image" | cut -d ' ' -f 1)
    [ "$(cut -d ' ' -f 1 /run/current.sha256)" = "$expected" ] || fail "MTD$number readback failed"
}
restore() {
    echo 'ROLLBACK: restoring original rootfs first, original FIT last.'
    write_raw 5 "$backup/mtd5.bin" || return 1
    write_raw 4 "$backup/mtd4.bin" || return 1
    verify_protected || return 1
    sync
}
if [ "$action" = --rollback ]; then restore; echo 'ROLLBACK COMPLETE'; exit 0; fi

changed=0
cleanup() {
    result=$?
    trap - EXIT INT TERM
    if [ "$result" -ne 0 ] && [ "$changed" = 1 ]; then
        for path in /run/old-settings /run/settings-seed /run/new-root; do
            if grep -q " $path " /proc/mounts; then
                umount "$path" || { echo 'Cannot unmount new filesystems; raw rollback stopped.' >&2; exit "$result"; }
            fi
        done
        if [ -e /sys/class/ubi/ubi0/mtd_num ]; then
            ubidetach /dev/ubi_ctrl -m 5 || { echo 'Cannot detach UBI; raw rollback stopped.' >&2; exit "$result"; }
        fi
        if [ -n "$pin" ]; then
            echo 'Installation failed; attempting rollback from the verified USB backup.' >&2
            restore || echo 'ROLLBACK FAILED: retain USB and use the explicit rollback command.' >&2
        else
            echo 'Installation failed and there is no backup: start the USB installer again.' >&2
        fi
    fi
    exit "$result"
}
trap cleanup EXIT
trap 'exit 1' INT TERM
# Verify the live originals still match the saved backup before the first erase.
if [ -n "$pin" ]; then
    for number in 4 5; do
        nanddump --quiet --omitoob --bb=skipbad "/dev/mtd$number" | sha256sum >/run/current.sha256
        expected=$(sha256sum "$backup/mtd$number.bin" | cut -d ' ' -f 1)
        [ "$(cut -d ' ' -f 1 /run/current.sha256)" = "$expected" ] || fail 'Original NAND changed after backup'
    done
fi
changed=1
# Keep Bluetooth pairings, the headphone room and the SBC quality across the
# reformat. Best effort: stock firmware has no GaldrunSettings volume. The
# adapter address derives from the Wi-Fi MAC, so the pairings stay valid.
keep=/run/settings-keep
rm -rf "$keep"
mkdir -p "$keep" /run/old-settings
if ubiattach /dev/ubi_ctrl -d 0 -m 5 -O 4096; then
    if mount -t ubifs -o ro ubi0:GaldrunSettings /run/old-settings; then
        for name in bluetooth headphones bluetooth-quality; do
            [ ! -e "/run/old-settings/$name" ] || cp -a "/run/old-settings/$name" "$keep/"
        done
        umount /run/old-settings
    fi
    ubidetach /dev/ubi_ctrl -m 5
fi
ubiformat /dev/mtd5 -y -s 4096 -O 4096 -f /payload/rootfs.ubi
ubiattach /dev/ubi_ctrl -d 0 -m 5 -O 4096
while read -r volume bytes digest; do
    case "$volume" in 0) name=RootFS;; 1) name=GaldrunSettings;; *) fail 'Unexpected readback volume';; esac
    [ "$(cat /sys/class/ubi/ubi0_$volume/name)" = "$name" ] || fail 'Installed volume name mismatch'
    head -c "$bytes" "/dev/ubi0_$volume" | sha256sum >/run/current.sha256
    [ "$(cut -d ' ' -f 1 /run/current.sha256)" = "$digest" ] || fail "Installed volume $name readback failed"
done </etc/galdrun-volume-readback
# Optional private deployment files stay outside the generic signed payload.
# Copy only these fixed data files, after checking their separate USB manifest.
if [ -d /media/galdrun-private ]; then
    (cd /media/galdrun-private && sha256sum -c SHA256SUMS) || fail 'Private settings checksum mismatch'
    mkdir -p /run/new-root /run/settings-seed
    mount -t ubifs -o ro ubi0:RootFS /run/new-root
    mount -t ubifs ubi0:GaldrunSettings /run/settings-seed
    cp -a /run/new-root/etc /run/settings-seed/etc
    for name in client.json galdrun-network.conf galdrun-cpu.conf galdrun-dac.json.Connector3 galdrun-dac.json.Connector3.defaults galdrun-ssh.conf ssh/sshd_config ssh/authorized_keys; do
        source=/media/galdrun-private/$name
        [ -f "$source" ] || continue
        if [ "$name" = client.json ]; then target=/run/settings-seed/client.json
        else target=/run/settings-seed/etc/$name; fi
        cp "$source" "$target"
        chmod 0600 "$target"
    done
    sync
    umount /run/settings-seed
    umount /run/new-root
fi
if [ -n "$(ls -A "$keep")" ]; then
    mkdir -p /run/settings-seed
    mount -t ubifs ubi0:GaldrunSettings /run/settings-seed
    cp -a "$keep"/. /run/settings-seed/
    sync
    umount /run/settings-seed
    echo "Kept settings: $(ls "$keep" | tr '\n' ' ')"
fi
ubidetach /dev/ubi_ctrl -m 5
write_raw 4 /payload/galdrun.itb
verify_protected || fail 'Factory partition checksum changed'
sync
changed=0
echo 'INSTALL COMPLETE: rootfs/volume/FIT readback passed; MTD0 protected; MTD1–MTD3 hashes unchanged.'
