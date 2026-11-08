# Connector 3 handover (2026-11-05, updated 2026-11-08): platform image installed

Start here for the next session. Bluetooth bring-up, button/LED behaviour and
earlier pitfalls stay in [the 2026-11-03 handover](connector3-handover-2026-11-03.md);
older install detail in [the 2026-10-31 handover](connector3-handover-2026-10-31.md)
and [the Connector 3 guide](connector3.md#bluetooth-a2dp-receive). Nothing
since 2026-11-02 is committed; `git status` shows all of it. Since 2026-11-08
all Raumfeld devices behave the same (read-write root, self-installing USB
image, client updates from the server, LAN-only time): do not reintroduce the
read-only root, SSH-driven installs or host-pinned backups described in older
sections.

## Where the device is (checked 2026-11-08 evening)

- **NAND:** platform image (USB `9f6061b3...`, payload `61466613...`, see
  [below](#platform-parity-installer-2026-11-08-installed)), installed by the
  self-installing USB image: read-write root, client `169ace58...` in
  `/opt/galdrun-client` with `galdrun-update.sh` (updates from ServerGui),
  chrony with LAN-only time (synced to the core on `10.0.0.6:4721`), the
  Connector 2 tools, `galdrun-restart`, the 2026-11-08 network fixes
  (netswitch) and everything from btkeep. Hostname set to `connector3` by
  hand (the next image does it itself). Wired Ethernet `10.0.0.74`.
- **Rooms:** main room `74bfd6573359566f` (fresh since the 2026-11-08 evening
  install; the stick no longer has `galdrun-private/`) and the headphone client
  `2c097945359c8912`. The next install is clean: both rooms and the pairings
  start over.
- **SSH:** `root@10.0.0.74`, key `hardware/buildroot/private/connector3/id_ed25519`,
  pin `hardware/buildroot/private/connector3/known_hosts_persistent_platform_20261108`
  (`SHA256:WgRG2Ji5...`, pinned on first use, not over UART). Every reinstall
  creates a new host key; older pins no longer match.
- **Backups on the stick:** the self-installing images up to `9f6061b3...`
  still took one each (`galdrun-backup/`, `galdrun-backup-1/`, `-2/`); host
  copy of the one before the first of them in
  `hardware/buildroot/private/connector3/reinstall-backup-selfupdate-20261108/`
  (pin `9fb09987...`). Newer images take none.
- **UART:** capture with `capture-fit-update-20261031.ps1` (archive folder; pass
  its absolute path to `powershell -File`), queue
  `private/connector3/fit-update-uart-tx-20261031.json` (`{"text": ...}`,
  written with `json.dump`). Send `echo UART_ALIVE` first; the first command
  can be lost.

## Self-update installer (2026-11-08, installed)

The root file system is now **read-write** (kernel `rw`, `init` checks it),
like Connector 2: the client updates itself in place in `/opt/galdrun-client`
([client guide](../agents/client.md#self-update)); `galdrun-update.sh` is in
the image. Do not bring the read-only root back: USB recovery is the factory
bootloader in MTD0, which the system never writes. A first build that copied
the client to `/settings` instead (USB `10b117c2...`) is superseded; do not
install it.

**The write installer installs itself** since this build, like Connector 2:
after the preflight it installs **without a backup** (since USB `29898c6d...`;
the first self-installing builds still took one each time), writes
`galdrun-install.log` to the stick and reboots into the NAND. A failed write
stays in the installer: boot the stick again. `galdrun-install --backup` and
`--install PIN` (with rollback) remain for a manual install over SSH. No SSH command or host-recorded backup pin
is needed any more; SSH stays up for inspection. The check image still stops
after the preflight. Do not reintroduce the SSH-driven install step
(`qualify-*`/`install-*` scripts in `reference/connector3-ssh-archive/` are
history).

Current USB `b1434d428ddcbbc5af12990a84fbe658deb0ada92260fcdb9ad52258e2002967`,
frozen in `hardware/buildroot/private/connector3/installer-selfupdate-b1434d42-20261108/`
(same file as `private/connector3/installer-write/connect3.img`); normal FIT
`490b8e72...`, payload `96a869ce...`, client `169ace58...`; everything else as
netswitch. The earlier `d984df73...` (same payload, SSH-driven) was booted
once: preflight passed and a fresh backup (pin `9fb09987...`, host copy in
`private/connector3/reinstall-backup-selfupdate-20261108/`) was taken, but it
never wrote the NAND.
**Installed 2026-11-08 by the self-installing USB image** (the user's USB
boot, no SSH). Checked afterwards: root `ubi0:RootFS ... rw`, kernel `rw`,
client `169ace58...` in `/opt/galdrun-client` with `galdrun-update.sh`,
`getUpdate` `supported`, room `c170ac6d4af8eddd` and the headphone client
from `/settings/headphones` running. New host key pinned on first use (no
UART check) in `private/connector3/known_hosts_persistent_selfupdate_20261108`
(`SHA256:whdlHIRH...`). Live self-update through ServerGui with a v0.1.1
test package (same binary): same PID 243 across the hand-over, swap, trial
confirmed 4 s after restart on the established session, headphone client
(PID 327) kept running, `.update/previous` 9.5 MB on the root volume.
The device clock still reads 1970 at boot (update.log timestamps).

## Platform parity installer (2026-11-08, installed)

USB `9f6061b3ed6ca6f2fbfccdaa4bfa36fff402d51d32ecbd72fa5ee07e9ecc4ba0`, frozen in
`hardware/buildroot/private/connector3/installer-platform-9f6061b3-20261108/`,
payload `61466613...` (still backs up first) -- **installed 2026-11-08** by
the user's USB boot (automatic backup to the stick, write, reboot). Checked:
NAND root `rw`, clock synced to the core (`chronyc`: `^* 10.0.0.6`, stratum 3,
the router `10.0.0.254` unreachable), all Connector 2 tools present except the
`timeout`/`nc` links (applets compiled in), `getPower` `restart: true`,
`getUpdate` `supported`, both rooms connected. Restart itself not yet pressed.
Host key pinned on first use in `known_hosts_persistent_platform_20261108`
(`SHA256:WgRG2Ji5...`). The installed hostname was `connector3-ram` (the RAM
image's `/etc/hostname`, seeded into `/settings/etc`); fixed on the device by
hand, in the persistent image (`connector3`) and by `init` for settings seeded
earlier.

Next image (not installed): USB
`971cee4ff86590c04d05c8c2c9fa637380057236e98ba51ee9e448a8a5a27178` in
`private/connector3/installer-platform-971cee4f-20261108/`, payload
`add89afe...`: installs without a backup, adds the `timeout`/`nc` links and
the `connector3` hostname. Everything in the self-update image plus what Connector
2 has: chrony with LAN-only time (`galdrun-time-sources`: router via DHCP hook
or static gateway, else the core's SNTP on UDP 4721; `init` starts
`chronyd -u root` -- this image has no init.d scripts and `/etc` comes from
`/settings`), `galdrun-restart` (restart button, audio profiles; falls back to
`reboot -f` for the shell PID 1), procps-ng, strace, tcpdump, rsync,
alsamixer, curl with CA certificates, MTD/UBI tools, BusyBox `timeout`/`nc`.
Host-checked: the image's ARM `chronyd` (qemu, `-Q`) got the Windows core's
time on 10.0.0.6:4721 within 0.3 ms. The router `10.0.0.254` answers no NTP.
After installing: `date` is current, `chronyc sources` lists the core,
the room page offers Restart.

## Connector 2 parity, last round (2026-11-08 night, not installed)

Release image `releases/client/raumfeld/connector3/connect3.img` <!-- path-check: ignore -->
`0139105e19be3462a6f22978bafaf1ecf84b582ccf1c2a2aeee455390a919ce7`: clean
reinstall (`KEEP_SETTINGS` empty), `resetHardwareVolume` in the board file,
client runs `galdrun-time-sources` after `setServer`, installer progress
lights (UART3 added to the installer DT, MCU tool one-shot `--indication`,
dynamically linked: the static build pushed the installer kernel over the
8 MiB FIT limit), console prints only the install log's tail.
`licenses/SOURCE.txt` names `connector3-sources-030ee2fe0d30` (668 MB, in
`hardware/buildroot/output/connector3-sources/`), **not yet published**:
upload it as a pre-release with the firmware release. Release packaging
(`scripts/release/package.py`, `notes.md`, git-ignored) now includes
`galdrun-connector3-<tag>.zip`. Untested on the device: the progress lights.

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
2. **"Analog + Digital" image `f0773e73...` installed 2026-11-08** (user's USB
   boot). After the install the device did not come back by itself; a power
   cycle (no stick, no Reset) booted it cleanly in ~20 s (UART capture). Cause
   unknown: the first UART read got nothing, RAM console lost by the power
   cycle, `galdrun-install.log` on the stick not yet read. Checked after the
   power cycle: one PCM `00-00 Analog + Digital`, client opens it at 96 kHz
   int32 with hardware volume, DMA position advances at the sample rate, no
   underruns, time synced, hostname `connector3`. Listening test on RCA and
   TOSLINK still open (the zone was paused). Boot shows two harmless sysfs
   warnings (`.../308a0000.sai/dma:tx` duplicate) from `dma_request_chan()`.
   **Next image** (not installed): USB
   `f8c90cdc3a0864f97d7bb45a086b90d7f257a02d60e2235a85f4dcbb72c952a2` in
   `releases/client/raumfeld/connector3/`: DMA via `of_dma_request_slave_channel()`
   (no warnings) and an installer fallback that forces the reset through
   `/proc/sysrq-trigger` if `reboot -f` returns. On its install, capture COM5
   (CP210x, 115200) from the USB boot on and read `galdrun-install.log`.
   **Installed 2026-11-08** with a COM5 capture: preflight, install,
   `INSTALL COMPLETE`, automatic reset at 97 s (`reboot: Restarting system`;
   the console cut the log's last lines because the reset came while the UART
   was still draining), NAND boot in ~20 s, no `dma:tx` warnings. The stick no
   longer had `galdrun-private/`, so the main room started fresh (new id
   `74bfd6573359566f`, attached again, SSH off); the headphone room came back
   by itself from `/settings/headphones`. Since then (user decision): a reinstall
   is **clean**, nothing is kept, switchable through `KEEP_SETTINGS` at the
   top of `board/connector3/installer/install-usb.sh`; and the client runs `galdrun-time-sources` after `setServer` (this router's DHCP
   lease never renews, so a later attach waited for a reboot before the core
   became a time source). The earlier "did not come back" stays unexplained;
   the reset path (watchdog, `reboot -f`, now with a SysRq fallback) worked here. One PCM drives RCA and TOSLINK together (see the
   guide's TOSLINK row); also no backup, `timeout`/`nc`, hostname. After it:
   `cat /proc/asound/pcm` shows only `00-00 ... Analog + Digital`; press
   **Rescan** on the room page so the client's device cache drops the old
   names; play and listen on RCA **and** TOSLINK, at 44.1 and 48 kHz; check
   `dmesg` for SAI/SDMA errors. If TOSLINK stays dark while RCA plays, the MCU
   likely has to enable the transmitter. Then press Restart once and
   pull/replug the cable with music.
3. Open, not explained: when the cable returns, the first reconnect attempt
   (about 1 s after the Wi-Fi address goes) can hang for the full 5 s
   connect timeout; the next one connects at once. Addresses, routes and the
   neighbour entry looked normal; `nc` comes with the next image.
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
