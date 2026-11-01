#!/bin/sh
# Run only in the qualified USB write profile. Never erase/program MTD5.
set -eu
fail() { echo "FIT UPDATE REFUSED: $*" >&2; exit 1; }
action=${1:-}
case "$action" in --prepare|--update) ;; *) fail 'Usage: update-fit --prepare|--update BACKUP_PIN OLD_FIT_SHA256 OLD_FIT_BYTES NEW_FIT_SHA256 [SAVED_MTD4_SHA256]';; esac
[ "$#" -eq 5 ] || { [ "$action" = --update ] && [ "$#" -eq 6 ]; } || fail 'Unexpected arguments'
backup_pin=$2
old_fit=$3
old_bytes=$4
new_fit=$5
saved_pin=${6:-}
for digest in "$backup_pin" "$old_fit" "$new_fit"; do
    case "$digest" in ''|*[!0-9a-f]*) fail 'Invalid SHA256';; esac
    [ "${#digest}" = 64 ] || fail 'Invalid SHA256 length'
done
case "$old_bytes" in ''|*[!0-9]*) fail 'Invalid old FIT length';; esac
[ "$old_bytes" -gt 0 ] && [ "$old_bytes" -le 12582912 ] || fail 'Invalid old FIT length'
work=/run/galdrun-fit-update
backup=/media/galdrun-backup
[ "$(uname -r)" = '6.12.112-galdrun-installer-write' ] || fail 'USB write kernel required'
[ "$(tr -d '\000' </proc/device-tree/model)" = 'Galdrun Connector 3 installer write' ] || fail 'USB write model required'
[ -f /etc/galdrun-install-writes-enabled ] || fail 'Write profile marker missing'
# Reuse the audited geometry, ECC, mount/UBI, factory protection and payload gates.
/usr/sbin/galdrun-install --check || fail 'Installer preflight failed'
[ "$(sha256sum "$backup/backup.sha256" | cut -d ' ' -f 1)" = "$backup_pin" ] || fail 'Factory backup pin mismatch'
(cd "$backup" && sha256sum -c backup.sha256) || fail 'Factory backup checksum mismatch'
mkdir -p "$work"
verify_protected() {
    flags=$(cat /sys/class/mtd/mtd0/flags)
    [ $((flags & 0x400)) -eq 0 ] || return 1
    sha256sum /dev/mtd1 /dev/mtd2 /dev/mtd3 >"$work/protected-current.sha256" || return 1
    sed '/  bootloader-raw$/d' "$backup/protected.sha256" >"$work/protected-saved.sha256" || return 1
    cmp "$work/protected-saved.sha256" "$work/protected-current.sha256"
}
verify_protected || fail 'Factory state changed'
flags=$(cat /sys/class/mtd/mtd4/flags)
[ $((flags & 0x400)) -ne 0 ] || fail 'MTD4 remains protected; never bypass kernel protection'
[ -f "$work/new.itb" ] || fail 'Stage the host-signature-verified FIT first'
[ "$(sha256sum "$work/new.itb" | cut -d ' ' -f 1)" = "$new_fit" ] || fail 'New FIT pin mismatch'
length=$(wc -c <"$work/new.itb")
bad=$(cat /sys/class/mtd/mtd4/bad_blocks)
bbt=$(cat /sys/class/mtd/mtd4/bbt_blocks)
capacity=$((12582912 - (bad + bbt) * 262144))
[ "$length" -gt 0 ] && [ "$length" -le "$capacity" ] || fail 'New FIT exceeds good MTD4 capacity'
if [ "$action" = --prepare ]; then
    [ ! -e "$work/current-mtd4.bin" ] || fail 'Retain the existing current FIT backup'
    nanddump --quiet --omitoob --bb=skipbad --file="$work/current-mtd4.bin" /dev/mtd4
    [ "$(wc -c <"$work/current-mtd4.bin")" -eq "$capacity" ] || fail 'Incomplete current MTD4 backup'
    head -c "$old_bytes" "$work/current-mtd4.bin" >"$work/current.itb"
    [ "$(sha256sum "$work/current.itb" | cut -d ' ' -f 1)" = "$old_fit" ] || fail 'Current FIT differs from expected installed image'
    sha256sum "$work/current-mtd4.bin"
    echo 'PREPARED: copy current-mtd4.bin to the host and independently verify its hash before --update.'
    exit 0
fi
case "$saved_pin" in ''|*[!0-9a-f]*) fail 'Supply independently verified saved MTD4 SHA256';; esac
[ "${#saved_pin}" = 64 ] || fail 'Invalid saved MTD4 SHA256'
[ "$(sha256sum "$work/current-mtd4.bin" | cut -d ' ' -f 1)" = "$saved_pin" ] || fail 'Saved MTD4 pin mismatch'
[ "$(wc -c <"$work/current-mtd4.bin")" -eq "$capacity" ] || fail 'Saved MTD4 capacity changed'
head -c "$old_bytes" "$work/current-mtd4.bin" >"$work/current.itb"
[ "$(sha256sum "$work/current.itb" | cut -d ' ' -f 1)" = "$old_fit" ] || fail 'Saved installed FIT pin mismatch'
nanddump --quiet --omitoob --bb=skipbad --file="$work/live-mtd4.bin" /dev/mtd4
cmp "$work/current-mtd4.bin" "$work/live-mtd4.bin" || fail 'MTD4 changed since backup'
rm "$work/live-mtd4.bin"
# MTD5 is read only by this helper, even though the USB profile allows writes.
sha256sum /dev/mtd5 >"$work/mtd5-before.sha256"
write_raw() {
    image=$1
    bytes=$(wc -c <"$image")
    bad=$(cat /sys/class/mtd/mtd4/bad_blocks)
    bbt=$(cat /sys/class/mtd/mtd4/bbt_blocks)
    [ "$bytes" -le $((12582912 - (bad + bbt) * 262144)) ] || return 1
    flash_erase -q /dev/mtd4 0 0 || return 1
    nandwrite -q --pad /dev/mtd4 "$image" || return 1
    nanddump --quiet --omitoob --bb=skipbad --length="$bytes" --file="$work/readback.bin" /dev/mtd4 || return 1
    cmp "$image" "$work/readback.bin" || return 1
    [ "$(cat /sys/class/mtd/mtd4/ecc_failures)" = 0 ]
}
changed=0
cleanup() {
    result=$?
    trap - EXIT INT TERM
    if [ "$result" -ne 0 ] && [ "$changed" = 1 ]; then
        echo 'FIT UPDATE FAILED: restoring the independently saved Galdrun FIT.' >&2
        if write_raw "$work/current-mtd4.bin"; then
            sync
            echo 'FIT ROLLBACK COMPLETE: exact saved MTD4 readback passed.' >&2
        else
            echo 'FIT ROLLBACK FAILED: retain USB and host backup; do not boot NAND.' >&2
        fi
    fi
    exit "$result"
}
trap cleanup EXIT
trap 'exit 1' INT TERM
changed=1
write_raw "$work/new.itb" || fail 'New FIT write/readback failed'
verify_protected || fail 'Factory hashes changed after update'
sha256sum /dev/mtd5 >"$work/mtd5-after.sha256"
cmp "$work/mtd5-before.sha256" "$work/mtd5-after.sha256" || fail 'MTD5 changed'
sync
changed=0
echo 'FIT UPDATE COMPLETE: exact MTD4 readback passed; MTD0 protected; MTD1-MTD3 and MTD5 unchanged.'
