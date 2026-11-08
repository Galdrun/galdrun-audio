#!/bin/sh
# Galdrun client updater, shipped beside galdrun-client. Not meant to be run by
# hand: the client exec()s it after the server uploaded a package
# (client/include/app/SelfUpdate.h), so it keeps the client's PID, and it
# always ends by exec()ing a client with the arguments it was given -- the new
# one when the update worked, the current one otherwise.
#
#   galdrun-update.sh install <sha256> [client arguments...]
#   galdrun-update.sh rollback [client arguments...]
#
# Runs under BusyBox sh. Settings live outside the replaced files; only what
# the release archive contains is replaced, the old files go to
# .update/previous.
set -u
dir=$(cd "$(dirname "$0")" && pwd)
work=$dir/.update
binary=$dir/galdrun-client
action=${1:-}
[ $# -gt 0 ] && shift
mkdir -p "$work"
# The client's --config: the trial belongs to that client.
config=
previous_arg=
for arg in "$@"; do
    [ "$previous_arg" = --config ] && config=$arg
    previous_arg=$arg
done

say() {
    printf '%s %s\n' "$(date '+%Y-%m-%d %H:%M:%S')" "$*" >> "$work/update.log"
}

# Puts the previous files back over the ones the last install placed.
restore() {
    [ -f "$work/installed" ] || return 1
    while IFS= read -r name; do
        [ -n "$name" ] && rm -rf "${dir:?}/$name"
    done < "$work/installed"
    for path in "$work/previous/"* "$work/previous/".[!.]*; do
        [ -e "$path" ] || continue
        mv "$path" "$dir/" || return 1
    done
    rm -rf "$work/previous" "$work/installed"
}

install_package() {
    sha=$1
    package=$work/package.tar.gz
    staged=$work/staged
    [ -f "$package" ] || { say "no package"; return 1; }
    actual=$(sha256sum "$package" | cut -d' ' -f1)
    [ "$actual" = "$sha" ] || { say "checksum mismatch: $actual, expected $sha"; return 1; }
    # The last update's rollback copy goes before unpacking: room for this one
    # on a small settings volume (Connector 3).
    rm -rf "$work/previous" "$work/installed"
    rm -rf "$staged"
    mkdir "$staged"
    gzip -dc "$package" | tar -xf - -C "$staged" || { say "cannot unpack the package"; return 1; }
    source=$staged
    # A release archive holds one folder, galdrun-client-<version>-<target>.
    if [ ! -f "$source/galdrun-client" ]; then
        for folder in "$staged"/*; do source=$folder; done
    fi
    [ -f "$source/galdrun-client" ] || { say "the package has no galdrun-client"; return 1; }
    chmod 755 "$source/galdrun-client"
    for name in galdrun-update.sh run.sh; do
        [ -f "$source/$name" ] && chmod 755 "$source/$name"
    done
    # Refuse a binary that does not start here (wrong architecture, missing
    # library) before touching the installed one.
    version=$(LD_LIBRARY_PATH="$source/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" "$source/galdrun-client" --version 2>&1) ||
        { say "the new client does not start: $version"; return 1; }

    mkdir "$work/previous"
    : > "$work/installed"
    for path in "$source"/* "$source"/.[!.]*; do
        [ -e "$path" ] || continue
        name=${path##*/}
        if [ -e "$dir/$name" ]; then
            mv "$dir/$name" "$work/previous/" || { restore; say "cannot move $name aside"; return 1; }
        fi
        echo "$name" >> "$work/installed"
        mv "$path" "$dir/" || { restore; say "cannot install $name"; return 1; }
    done
    echo "pending $config" > "$work/trial"
    say "installed $version; on trial"
}

case $action in
    install)
        sha=${1:-}
        [ $# -gt 0 ] && shift
        install_package "$sha" || say "update failed; the current client keeps running"
        rm -rf "$work/staged" "$work/package.tar.gz"
        ;;
    rollback)
        if restore; then say "rolled back to the previous client"
        else say "nothing to roll back"; fi
        rm -f "$work/trial"
        ;;
    *)
        echo "usage: $0 install <sha256> | rollback [client arguments...]" >&2
        exit 2
        ;;
esac
exec "$binary" "$@"
