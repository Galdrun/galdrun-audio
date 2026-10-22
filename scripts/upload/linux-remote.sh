# Runs on the target, sent by linux.ps1 / linux.sh with @USER@ and @SERVER_ARGS@
# filled in; stdin carries the release archive. Not meant to be run by hand.
set -euo pipefail
mkdir -p "$HOME/galdrun-client"
cd "$HOME/galdrun-client"
if systemctl is-active --quiet galdrun-client.service 2>/dev/null; then
    if [[ $(id -u) == 0 ]]; then systemctl stop galdrun-client.service
    else sudo -n systemctl stop galdrun-client.service; fi
fi
if [[ -f client.pid ]]; then
    read -r pid < client.pid 2>/dev/null || pid=''
    if [[ "$pid" =~ ^[0-9]+$ ]] &&
       [[ "$(readlink "/proc/$pid/exe" 2>/dev/null)" == "$PWD/galdrun-client" || "$(readlink "/proc/$pid/exe" 2>/dev/null)" == "$PWD/galdrun-client (deleted)" ]] &&
       grep -Fzxq -- "$PWD/client.json" "/proc/$pid/cmdline"; then
        kill -TERM "$pid"
        for ((i=0; i<100; ++i)); do
            kill -0 "$pid" 2>/dev/null || break
            sleep 0.1
        done
        if kill -0 "$pid" 2>/dev/null; then echo 'Existing client did not stop.' >&2; exit 1; fi
    fi
fi
# Windows OpenSSH >= 9.5 prepends a UTF-8 BOM to piped stdin; strip it so tar sees a clean archive.
sed '1s/^\xef\xbb\xbf//' | tar -xzf -
chmod +x run.sh galdrun-client
if ! /sbin/ldconfig -p | grep 'libasound.so.2' >/dev/null; then
    privilege=()
    if [[ $(id -u) != 0 ]]; then
        if sudo -n true 2>/dev/null; then privilege=(sudo -n)
        else echo 'Install libasound2 or libasound2t64 with sudo, then retry.' >&2; exit 1; fi
    fi
    "${privilege[@]}" apt-get update </dev/null
    package=libasound2
    if apt-cache show libasound2t64 >/dev/null 2>&1; then package=libasound2t64; fi
    "${privilege[@]}" apt-get install -y "$package" </dev/null
fi
./run.sh --version
[[ -f client.json ]] || cp client.example.json client.json
cat > install-service.sh <<'INSTALL_SERVICE'
set -euo pipefail
deploy_user='@USER@'
client_dir="$(getent passwd "$deploy_user" | cut -d: -f6)/galdrun-client"
cd "$client_dir"
if systemctl is-active --quiet galdrun-client.service; then systemctl stop galdrun-client.service; fi
if [[ -f client.pid ]]; then
    read -r pid < client.pid
    if [[ "$pid" =~ ^[0-9]+$ ]] &&
       [[ "$(readlink "/proc/$pid/exe" 2>/dev/null)" == "$PWD/galdrun-client" || "$(readlink "/proc/$pid/exe" 2>/dev/null)" == "$PWD/galdrun-client (deleted)" ]] &&
       grep -Fzxq -- "$PWD/client.json" "/proc/$pid/cmdline"; then
        kill -TERM "$pid"
        for ((i=0; i<100; ++i)); do
            kill -0 "$pid" 2>/dev/null || break
            sleep 0.1
        done
        if kill -0 "$pid" 2>/dev/null; then echo 'Existing client did not stop.' >&2; exit 1; fi
    fi
fi
cat > /etc/systemd/system/galdrun-client.service <<UNIT
[Unit]
Description=Galdrun audio client
Wants=network-online.target
After=network-online.target sound.target

[Service]
Type=simple
User=$deploy_user
WorkingDirectory=$client_dir
RuntimeDirectory=galdrun-client
RuntimeDirectoryMode=0700
ExecStart="$client_dir/run.sh" --config "$client_dir/client.json" @SERVER_ARGS@--audio-cache "$client_dir/audio_devices.json" --log-file /run/galdrun-client/client.log
StandardOutput=null
StandardError=null
Restart=on-failure
RestartSec=5
UNIT
if getent group audio >/dev/null; then echo 'SupplementaryGroups=audio' >> /etc/systemd/system/galdrun-client.service; fi
printf '\n[Install]\nWantedBy=multi-user.target\n' >> /etc/systemd/system/galdrun-client.service
systemctl daemon-reload
systemd-analyze verify /etc/systemd/system/galdrun-client.service
systemctl enable galdrun-client.service
systemctl restart galdrun-client.service
sleep 2
if ! systemctl is-active --quiet galdrun-client.service; then journalctl -u galdrun-client.service -n 25 --no-pager; exit 1; fi
systemctl is-enabled galdrun-client.service
systemctl is-active galdrun-client.service
systemctl show galdrun-client.service -p MainPID --value > client.pid
printf 'Client service running: PID %s, directory %s\n' "$(cat client.pid)" "$PWD"
journalctl -u galdrun-client.service -n 12 --no-pager
INSTALL_SERVICE
if [[ $(id -u) == 0 ]]; then bash install-service.sh
elif sudo -n true 2>/dev/null; then sudo -n bash install-service.sh
else echo 'SERVICE_SUDO_REQUIRED'; fi

