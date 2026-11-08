# Connector 3 handover (2026-11-05, updated 2026-11-08): btkeep installed

Start here for the next session. Bluetooth bring-up, button/LED behaviour and
earlier pitfalls stay in [the 2026-11-03 handover](connector3-handover-2026-11-03.md);
older install detail in [the 2026-10-31 handover](connector3-handover-2026-10-31.md)
and [the Connector 3 guide](connector3.md#bluetooth-a2dp-receive). Nothing
since 2026-11-02 is committed; `git status` shows all of it.

## Where the device is (checked 2026-11-08)

- **NAND:** btkeep USB installer
  `ab5533c862271711751e3dabcd89f4e32e9667d891e8b3a2d041ab862bcae954`
  (frozen in `hardware/buildroot/private/connector3/installer-btkeep-ab5533c8-20261108/`),
  installed 2026-11-08 11:12 UTC by `install-btkeep-20261108.py` (readback
  passed, MTD0 protected, MTD1-MTD3 unchanged, log `Kept settings: bluetooth
  headphones`). Same rootfs/FIT as btquality (FIT `94971c07...`):
  `galdrun-bluetooth` `ee5e88d8...`, client `b32e68c5...`, no bind mounts,
  bluealsa `--sbc-quality=high`. Wired Ethernet `10.0.0.74`, Wi-Fi down.
- **Cold boot after install:** pairings (LEXON `63:21:FB:EF:91:7E`, phone
  `motorola edge 60 neo`) survived; the LEXON reconnected and the headphone
  client started as the same client id `2c097945359c8912`, so its server room
  and 175 ms latency were kept too.
- **Live only (lost on reboot):** client `4a994673...` (2026-11-08 source,
  both fixes below) runs from `/run/galdrun-client.new` through a bind mount
  over `/opt/galdrun-client/galdrun-client`; only the main client was
  restarted, the headphone client still runs the image's `b32e68c5...`.
- **SSH:** `root@10.0.0.74`, key `hardware/buildroot/private/connector3/id_ed25519`,
  pin `hardware/buildroot/private/connector3/known_hosts_persistent_btkeep_20261108`
  (`DMOJIwoR...`, confirmed over UART). A reinstall still creates a new host
  key; older pins no longer match.
- **Rollback:** NAND backup taken before this install, manifest pin
  `ef5f568ba0e7d7ad9e618dcaba1c741594ac7dc078007045b8005fa6d9cd2ddb`, on USB
  `galdrun-backup/` and in
  `hardware/buildroot/private/connector3/reinstall-backup-btkeep-20261108/`.
  Earlier backups: `d1b856ef...` (btquality, on USB as
  `galdrun-backup-before-btkeep-20261108/`), `7d369fd7...`, `6732a1c2...`.
- **UART:** capture with `capture-fit-update-20261031.ps1` (archive folder; pass
  its absolute path to `powershell -File`), queue
  `private/connector3/fit-update-uart-tx-20261031.json` (`{"text": ...}`,
  written with `json.dump`). Send `echo UART_ALIVE` first; the first command
  can be lost.

## Done since 2026-11-04

- Headphone resume fix: `resume_headphones` also accepts an existing bluez-alsa
  A2DP PCM for the address, because `Device1.Connect` can keep failing with
  `br-connection-busy` after the headphones reconnect by themselves.
- Headphone SBC quality setting (`/settings/bluetooth-quality`, client
  `setBluetoothQuality`, core/REST relay, ServerGui Device section); see
  [the guide](connector3.md#bluetooth-a2dp-receive).
- The LEXON headphone room needs **175 ms** output latency to match RCA.
- ServerGui: a `bluetoothOutput` room shows a fixed output, no Device section
  and no rename button (the name follows the main room).
- Buttons: the server never runs a `bluetoothOutput` room's own input reports.
  With its room setting `inputsEnabled` ("Buttons on the device also control
  this room", default off) a press on the main room also runs for the BT room
  with the main room's bindings. Before, in one zone both clients'
  `togglePause` cancelled out.
- Control ports: main client 4720, headphone client 4730 (`127.0.0.1` only).
- 2026-11-08: client, core and .NET tests pass (`ctest --preset windows-x64`
  in `client/` and `server-core/`, `dotnet test` in `server/`: 460 passed).
- 2026-11-08: the USB installer keeps `/settings/bluetooth`,
  `/settings/headphones` and `/settings/bluetooth-quality` across the reformat
  ([install-usb.sh](../../hardware/buildroot/board/connector3/installer/install-usb.sh));
  `scripts/test-connector3-installer.py` passes; qualified on the device by the
  btkeep install above.

- 2026-11-08 Wi-Fi on NAND: Ethernet-to-Wi-Fi fallback and back at runtime,
  10 min sustained playback over WLAN (610/610 pings, max 139 ms, no client
  warnings); see the guide's Wi-Fi row. Not tested: reconnect after an AP
  loss and the setup-AP fallback (user has several APs; `iw dev mlan0
  disconnect` is refused by mwifiex).
- 2026-11-08 client fixes, tested on the device (`docs/agents/client.md`,
  thread and timing rules): the connection drops as soon as its local address
  is gone (`the local network address went away`), so a pulled cable costs
  about 12 s instead of 23 s; a repeated `startAt` while playing is answered
  with an immediate playback report instead of an ERROR. 504 client tests pass.
  Follow-up: for 15 s after such a drop, retries stay at 1 s instead of
  growing to 2 s and 5 s on "Network is unreachable" (an on-device
  `ip link set eth0 down` run lost 8 s to that backoff).

## Next steps

1. **Phone and headphones together stutter.** Measured: alone the LEXON link
   carries 42.6 kB/s, with an idle phone connected only about 35 kB/s. Try
   `galdrun-bluetooth quality medium`, then `low`, with both connected. If it
   still stutters: link roles (`hcitool con`, both are central), Wi-Fi
   coexistence, bluez-alsa/client buffer sizes.
2. **netswitch installer built 2026-11-08, not installed:** USB
   `93d70f8d151b463d6e3da675e22282e7f1404a0772479e39710690a00cbc19fd`, frozen in
   `hardware/buildroot/private/connector3/installer-netswitch-93d70f8d-20261108/`
   (same file as `private/connector3/installer-write/connect3.img`). Full chain
   (RAM with `pcie8897-working-reference.bin`, persistent, installer
   `--write`): normal FIT `6ffa76b2...`, payload `e1468e00...`, client
   `ca746824...` (both 2026-11-08 network fixes plus the 1 s switch-over
   backoff); everything else as btkeep. Procedure as for btkeep: marker
   `printf '%s_%s
' GALDRUN_USB_IDENTITY NETSWITCH_20261108; ...`, then
   `qualify-netswitch-installer-20261108.py --backup` (previous USB backup pin
   `ef5f568b...`), **ask before** `install-netswitch-20261108.py`, UART-pin the
   new host key as `known_hosts_persistent_netswitch_20261108`. Pairings are
   kept. Afterwards: pull and replug the cable once with music playing.
3. Open, not explained: when the cable returns, the first reconnect attempt
   (about 1 s after the Wi-Fi address goes) can hang for the full 5 s
   connect timeout; the next one connects at once. Addresses, routes and the
   neighbour entry looked normal; `nc` is not in the image.
4. Commit when the user asks.

Later / open: factory `bt_addr` from the const partition, TOSLINK listening
test, ESP32-S31 `IBluetoothReceiver`, AVRCP volume; the rest of the
[remaining-work table](connector3.md#remaining-work-and-retest-updated-2026-11-08).

## Pitfalls (do not repeat)

- `wsl -d Ubuntu -- bash -lc '...'` re-parses the command line: `$(...)`,
  `$?` and `$var` run in WSL, not on the device. Write the remote commands to a
  file and send it (copy to `/run`, then run it with stdin from `/dev/null`).
- `bluetoothctl` reads stdin: inside `ssh ... sh -s <script` it eats the rest
  of the script. Give it `</dev/null`.
- From Git Bash, prefix `wsl` calls with `MSYS_NO_PATHCONV=1`, or `/mnt/c/...`
  paths are rewritten to `C:/Program Files/Git/mnt/c/...`.
- `/settings` after a full install is a fresh seed: expect a new host key; do
  not "fix" the known_hosts mismatch without a UART check.
- BusyBox sed rejects `s/\\/\\\\/g`; use `s/[\]/&&/g`.
- Record each install in this handover right away: the btquality install went
  unrecorded for three days.
