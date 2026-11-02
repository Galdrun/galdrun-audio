# Connector 3 handover for 2026-11-03: Bluetooth

Start here for the next session. Earlier detail (installs, backups, sleep,
TOSLINK, the full Bluetooth bring-up log) stays in
[the previous handover](connector3-handover-2026-10-31.md) and
[the Connector 3 guide](connector3.md#bluetooth-a2dp-receive). Nothing from
2026-11-02 is committed; `git status` shows all of it (plus the user's own
earlier dirty files and a deleted `docprompt.md` that was not this work).

## Where the device is

- **NAND:** USB installer `e7f66ba0...` (frozen in
  `hardware/buildroot/private/connector3/installer-btled-e7f66ba0-20261102/`),
  installed and user-tested: phone receive, BT/Setup buttons, BT LED, pairings
  kept across reboots. SSH pin
  `hardware/buildroot/private/connector3/known_hosts_persistent_btled_20261102`,
  `root@10.0.0.74` with `hardware/buildroot/private/connector3/id_ed25519`.
  Rollback backup: `hardware/buildroot/private/connector3/reinstall-backup-btled-20261102/`
  (manifest pin `994044d6...`).
- **Live only (lost on reboot):** newer client and `galdrun-bluetooth` run from
  `/run` through bind mounts over `/opt/galdrun-client/galdrun-client` and
  `/usr/sbin/galdrun-bluetooth`; `/etc/bluetooth/main.conf` (settings volume)
  has `TemporaryTimeout = 300`. The headphone room for a LEXON MINO L is
  configured in `/settings/headphones/` (survives reboot, but the installed
  image cannot use it yet).
- **Local PC:** core, server and ServerGui run the new code from `releases/`
  (started with the arguments of `scripts/start-local.ps1`, without its
  client). UART capture on COM5 is not running.

## What works (tested 2026-11-02)

- Phone → Connector: pairing (BT click or client page), playback on RCA,
  MATP `localSource: "bluetooth"`, zone pauses and resumes around it, blue
  badge in ServerGui.
- BT button (MCU button 4 → `KEY_BLUETOOTH`): click `btModeRec` (pairing for
  phones), long press `btModePlay` (headphone mode on/off). Setup (button 8 →
  `KEY_SETUP`) is listed, unassigned. Kernel `CONFIG_RFKILL_INPUT` must stay off.
- BT LED (MCU `0x21`: 0 off, 1 blink, 2 steady): blinking while pairing or
  searching, steady when connected, 3 s on / 1 s off in headphone mode.
- Headphones/speaker: client page Bluetooth card (search with countdown, pick,
  disconnect) or BT long press (nearest headset/sink). Second client "<room>
  Headphones" on port 4730 plays 48 kHz 2ch int16 through the raw bluez-alsa
  PCM; MATP `bluetoothOutput` shows a headphone icon in ServerGui. User heard
  it; it was about 20 ms behind RCA -- set the headphone room's output latency.
- Bluetooth name = room name (`IBluetoothReceiver::setName`).

## Next steps

1. **Build and install an image with everything.** Same chain as before, in WSL
   from the repository root: `build-connector3-ram.sh` with the approved radio,
   `build-connector3-persistent.sh`, `build-connector3-installer.sh --write`
   (commands in [the Connector 3 guide](connector3.md)). Copy
   `hardware/buildroot/reference/connector3-ssh-archive/qualify-btled-installer-20261102.py`
   and `install-btled-20261102.py` to new names with the new USB/FIT hashes,
   UART marker and backup pin `994044d6...`. Restart the COM5 capture
   (`capture-fit-update-20261031.ps1`) before the USB boot; the user holds
   Reset for 10 s while connecting power (the first try sometimes misses).
   Ask before the NAND write.
2. **After install:** cold boot; the LEXON reconnects and its room comes back
   by itself; BT long press toggles it; phone and headphones together; deep
   sleep and wake bring Bluetooth and the headphone room back
   (`raumfeld-suspend` restarts `galdrun-bluetooth`).
3. Set the headphone room's output latency (~20 ms for the LEXON) and recheck
   grouped sync.
4. Commit when the user asks.

Later / open: factory `bt_addr` from the const partition (Wi-Fi MAC + 1 is
used), TOSLINK listening test, an ESP32-S31 `IBluetoothReceiver`, AVRCP
volume.

## Pitfalls found 2026-11-02 (do not repeat)

- `bluetoothctl` quits at end of input when run in the background, and BlueZ
  ends a discovery when its D-Bus client leaves: searches use
  `sleep N | bluetoothctl --timeout N scan on`. `bluetoothctl --timeout` always
  waits the whole timeout; one-shot calls use `dbus-send`.
- `bluetoothd` reads the BD address at power-up: one extra `hciconfig up/down`
  after setting it, or pairings are stored under the placeholder address.
- The `bluealsa` ALSA alias is plug-wrapped: as a capture PCM disable
  resampling; as the headphone room's playback device use `type bluealsa`
  directly (the plug let the server pick 8 channels).
- A new MATP enum value the .NET server does not know fails its whole core
  `status` read silently (all rooms lose live data). Add it to
  `server/src/Multiroom.Server.Playback/Core/CoreReplies.cs` too.
- Python heredocs mangled `\n`/`\\` in shell and C++ edits several times;
  prefer the Edit tool for such text.
