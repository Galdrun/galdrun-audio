# Connector 3 / Teufel Streamer: SSH access and experimental RAM build

Verified on the test Streamer on **2026-10-28**, without working UART or Ethernet.
USB configuration connected it to home Wi-Fi, and a direct UPnP update request
installed the archived beta without completing phone-app setup.

| Item | Verified result |
| --- | --- |
| Device | Teufel Streamer, hardware ID **19**, `connect3.img` |
| Address during the test | **10.0.0.58**, Wi-Fi MAC **50:1e:2d:3b:77:f6** |
| Before update | **2.13.4**, `WAIT_FOR_SETUP` |
| Installed beta | **2.12-snapshot-7**, kernel **4.1.45** |
| SSH | Successful login as `root` without a password |
| Activation files on device | `/etc/raumfeld/sshd_enabled`, `/etc/raumfeld/console_enabled` |

These vendor installer/update files write persistent firmware. They are separate
from the unsuccessful source-built RAM diagnostic described in
[the investigation notes](raumfeld/connector3.md). Compatibility with a different
Stream810 module revision has not been established; see those notes before
applying this old firmware to another unit. Ethernet repair, serial-console
output and Galdrun playback are not established by the SSH result.

## Files used

The user supplied the public [SSH Streamer archive](https://drive.google.com/drive/folders/1-htQzqporcluONsyXVopgBq1JnFk1148).
Downloads and prepared marker are in ignored
[`hardware/buildroot/downloads/connector3-ssh-archive/`](../../hardware/buildroot/downloads/connector3-ssh-archive/).
Do not commit or bundle the vendor images with a Galdrun release.

| File | Bytes | SHA256 |
| --- | ---: | --- |
| `connect3.img` | 86,412,288 | `a26cf9471f8cdb1cf32fa9a886f444bf7bcad860c4b41427ffd3ac40b547ae86` |
| `raumfeld-updates.img` | 164,304,384 | `44bb4c437c6319220b6e65c5bfcc06f440a19e9d4a8d4908d237f984ae36b07a` |

- `connect3.img` installs **2.13.4**. It does **not** enable SSH by itself.
- `raumfeld-updates.img` contains the old **2.12-snapshot-7** beta. Its
  `19.updates` manifest advertises `version=2.99` and `storageVersion=2.99` so
  it can be offered to the newer installed firmware. This is still a downgrade
  to old beta code, not firmware 2.99.
- An empty file named **`48fab7623bce0c903d5fe53dd681bb163eba85ae`** activates
  SSH and the console through the beta's USB mount hook. There is no extension.

The beta's `etc/usbmount/mount.d/20sshd-enable` checks only marker-file existence,
creates the two enable files, and starts Dropbear. It does not require room
setup or an existing Raumfeld host. The inspected 2.13.4 installer lacks that
hook, so the marker cannot enable SSH until the beta is actually running.

## Keep online updates blocked during the procedure

The tested network uses a MikroTik router/DNS server at `10.0.0.254`. The user
added these entries in the router terminal:

```routeros
/ip dns static add name=updates.raumfeld.com address=127.0.0.1 ttl=1m comment="Temporary Raumfeld update block"
/ip dns static add name=updates.raumfeld.com address=::1 ttl=1m comment="Temporary Raumfeld update block"
/ip dns cache flush
```

This affects devices using that router for DNS; external DNS bypasses it.
Verify from the PC with:

```powershell
nslookup updates.raumfeld.com 10.0.0.254
```

Both `127.0.0.1` and `::1` were returned. Reboot the Streamer before starting the
procedure to discard old DNS state. Blocking the server may prevent normal
app setup from completing its update check; the procedure below bypasses the
app. Do not unblock it merely to get past the setup screen.

## 1. Install the archived 2.13.4 base

The user installed `connect3.img` using the normal USB reinstall procedure:
copy the file into the root of an empty FAT32 stick, power off, insert the
stick, hold Reset while reconnecting power, and keep holding Reset for
10 seconds. Teufel specifies a stick of at most 4 GB. A reinstall deletes
personal settings; see [Teufel's instructions](https://support.teufel.de/hc/en-us/articles/360001499849-Teufel-Raumfeld-devices-reinstalling-the-firmware).

Allow the device to finish and boot normally. In this test its Ethernet link
remained down, and the phone app repeatedly found the Streamer but failed
after Next/Connecting. Full room setup was not needed to proceed.

## 2. Join home Wi-Fi using USB configuration

Use an otherwise empty FAT32 stick containing **only** `raumfeld-setup.json`
in its root. Example with placeholders:

```json
{
  "isHost": false,
  "ssid": "YOUR_HOME_WIFI_SSID",
  "password": "YOUR_HOME_WIFI_PASSWORD",
  "name": "Streamer"
}
```

`isHost: false` selects the client role because this installation already has a
Raumfeld host. Use `true` only when intentionally configuring a standalone host.
Keep actual credentials local; do not put the completed file in Git or chat.
The local template and instructions are under ignored
`hardware/buildroot/private/connector3/`.

With the Streamer booted normally, insert the stick, wait a few seconds for it
to mount, and briefly press **Setup**, not Reset. If the first press cancels an
already-active setup, press Setup again. The firmware's USB mount hook links
the file to `/tmp/raumfeld-setup.json`; its Setup-button handler then loads it
through `PreconfiguredSetupController`.

In this test the open `Raumfeld Setup` WLAN disappeared and the same device MAC
appeared on the home LAN at **10.0.0.58**. Direct status queries confirmed the
home SSID, signal strength 90, firmware 2.13.4 and mode `WAIT_FOR_SETUP`.
This established connectivity, not completed membership in the existing system.
Remove the stick afterward because it contains the Wi-Fi password.

## 3. Insert the beta update and marker

Replace the stick's contents with exactly:

```text
raumfeld-updates.img
48fab7623bce0c903d5fe53dd681bb163eba85ae
```

Insert it while the Streamer is running. No Reset press and no `connect3.img`
are needed at this stage. The current firmware's USB mount hook starts
`raumfeld-updates-server`, making the update available locally. Insertion alone
does not install the beta. Leave the stick inserted through the update and reboot
so the beta can process the marker.

## 4. Discover the target and request its update directly

The working path was the Streamer's UPnP **SetupService**, not the phone app or
the HTTPS setup API (which rejected unauthenticated setup requests). The
existing host at **10.0.0.207** already discovered the unconfigured Streamer.
Its read-only `/listDevices` endpoint supplied the device-description URL.

The following PowerShell uses the test addresses. Confirm the target's IP/MAC
in the router and replace the addresses when repeating this on another network.
Description ports and UUID paths are dynamic; discover them again after reboot.

```powershell
$deviceIp = '10.0.0.58'
$existingHostIp = '10.0.0.207'
[xml]$devices = (Invoke-WebRequest "http://${existingHostIp}:47365/listDevices" -UseBasicParsing).Content
$entry = $devices.devices.device | Where-Object {
    $_.type -eq 'urn:schemas-raumfeld-com:device:RaumfeldDevice:1' -and
    ([uri]$_.location).Host -eq $deviceIp
} | Select-Object -First 1
if (-not $entry) { throw 'Streamer not found in the host device list' }
$descriptionUri = [uri]$entry.location
[xml]$description = (Invoke-WebRequest $descriptionUri -UseBasicParsing).Content
if ($description.SelectSingleNode("//*[local-name()='hardwareType']").InnerText -ne '19') {
    throw 'Target is not Streamer hardware ID 19'
}
$service = $description.root.device.serviceList.service | Where-Object {
    $_.serviceType -eq 'urn:schemas-raumfeld-com:service:SetupService:1'
}
$controlUri = [uri]::new($descriptionUri, [string]$service.controlURL)

function Invoke-StreamerSetup([string]$action, [string]$argumentXml = '') {
    $serviceType = 'urn:schemas-raumfeld-com:service:SetupService:1'
    $body = '<s:Envelope xmlns:s="http://schemas.xmlsoap.org/soap/envelope/"><s:Body>' +
        "<u:$action xmlns:u=`"$serviceType`">$argumentXml</u:$action>" +
        '</s:Body></s:Envelope>'
    [xml](Invoke-WebRequest $controlUri -Method Post -UseBasicParsing -TimeoutSec 10 `
        -ContentType 'text/xml; charset=utf-8' `
        -Headers @{ SOAPACTION = ('"' + $serviceType + '#' + $action + '"') } -Body $body).Content
}

Invoke-StreamerSetup 'GetInfo'
Invoke-StreamerSetup 'CheckForUpdate' '<Throttle>0</Throttle>'
```

The check is asynchronous. After a few seconds, read the result:

```powershell
$update = Invoke-StreamerSetup 'GetUpdateInfo'
$version = $update.SelectSingleNode("//*[local-name()='Version']").InnerText
if ($version.Trim() -ne '2.99') { throw 'Expected USB beta is not available' }
```

Before installing, the device-served manifest was checked as hardware ID 19,
and its actual payload was downloaded from the USB update server and verified:

```text
Payload bytes: 33156728
SHA256: 227ec7fdb49bf85008014f178642c7da67b1c6443e4605fafd3b6ba38b99779e
```

The update server advertised SSDP type `raumfeld:updates`; its test URL was
`http://10.0.0.58:55553`. That port can change. `GET /19.updates` returned the
manifest and `GET /<payload-sha256>` returned the matching payload. A matching
hash is an integrity check, not independent verification of the vendor signature.

Once the target and package are verified, this is the command that starts the
persistent update on **this Streamer only**:

```powershell
$escapedVersion = [System.Security.SecurityElement]::Escape($version)
Invoke-StreamerSetup 'DoUpdate' "<Version>$escapedVersion</Version>"
```

Keep the **original** returned version string when sending `DoUpdate`. This
archive's string includes a trailing tab and space (`"2.99\t "`); trim only for
the comparison. The accepted request returned HTTP 200 and `DoUpdateResponse`.
If a request times out, inspect device state before retrying: a timeout does not
establish that installation failed to start.

Leave power and USB connected. The Streamer went offline, returned on the same
Wi-Fi address, and opened SSH roughly 90 seconds after the request. Allow several
minutes for another attempt rather than interrupting installation.

## 5. Verify SSH and set a password

The successful login and checks were:

```sh
ssh root@10.0.0.58
cat /etc/raumfeld-version
cat /proc/sys/kernel/osrelease
ls -l /etc/raumfeld/sshd_enabled /etc/raumfeld/console_enabled
```

The remote output confirmed `2.12-snapshot-7`, `4.1.45`, and both activation files.
The first-seen ECDSA host key was saved locally in ignored
`hardware/buildroot/private/connector3/known_hosts`. Verify the device identity
before accepting a host key; do not globally disable host-key checking.

The tested beta permits an empty root password. Set one after login:

```sh
passwd
```

Do not remove the update-server block until the intended next firmware step is
decided. Successful SSH does not mean that room setup or playback is configured.

## Ethernet inspection after enabling SSH

`connmanctl enable ethernet` changed the beta's Ethernet technology from
`Powered=False` to `Powered=True` and brought `eth0` administratively up. The
interface still reported `NO-CARRIER`, `/sys/class/net/eth0/carrier` remained `0`,
and there was no physical link light. The cable/port had worked with Connector 2;
this does not establish the cause of the Connector 3 failure. Wi-Fi remained
usable, so Ethernet is not a prerequisite for the custom RAM experiment.

This unit's MDIO PHY ID is `0x0007c0f1` (SMSC LAN8710/LAN8720 family), at
`/sys/bus/mdio_bus/devices/30bf0000.etherne:00`. The beta kernel uses Generic PHY
and lacks `CONFIG_SMSC_PHY`. Trying the SMSC Auto-MDIX setting and restarting
autonegotiation did not produce a link; the original register setting was
restored. The new RAM kernel includes `CONFIG_SMSC_PHY=y`; that is not evidence
that it fixes this unit's link.

## Custom RAM build

The experimental profile is
[`hardware/buildroot/board/connector3/ram/`](../../hardware/buildroot/board/connector3/ram/).
It builds Linux **6.12.112**, an external initramfs with public-key-only OpenSSH,
Marvell **88W8897 PCIe** Wi-Fi support, ALSA utilities, and the current
**linux-armv7** Galdrun client. It builds the kernel from source and bundles
redistributable upstream Wi-Fi/SDMA firmware; it does not copy Teufel's rootfs or
kernel into the custom payload. This unit has board revision **0** and an
**AXP152** PMIC; compatibility with later module revisions is unqualified.

Stop local Galdrun instances before building as described in
[the development guide](../agents/development.md). From the repository root in
Linux/WSL, with an existing **public** SSH key:

```sh
bash hardware/buildroot/scripts/build-connector3-ram.sh \
  hardware/buildroot/private/connector3/id_ed25519.pub
```

On this Windows checkout the equivalent command was:

```powershell
wsl -d Ubuntu -- bash /mnt/c/Projects/Codeing/GaldrunAudio/galdrun-audio/hardware/buildroot/scripts/build-connector3-ram.sh /mnt/c/Projects/Codeing/GaldrunAudio/galdrun-audio/hardware/buildroot/private/connector3/id_ed25519.pub
```

The build passed on **2026-10-28**. Outputs are in ignored
[`hardware/buildroot/output/connector3-ram/`](../../hardware/buildroot/output/connector3-ram/):
`zImage`, `connector3.dtb`, `rootfs.cpio.gz`, `rootfs.tar.gz`, static ARM `kexec`, `SHA256SUMS`, and
`ram-verification.json`. These are RAM boot components, **not** a USB
`connect3.img` installer. The build script first rebuilds the ARMv7 client and
automatically checks the actual initramfs and kernel configuration. The kernel
has no MTD, MMC, ATA, SCSI, USB-storage or loadable module support.
A persistent image will need a separate root filesystem: this full RAM payload
exceeds the vendor's **12 MiB** FIT partition (`mtd4`).

The payload check passed, as did QEMU execution of the packaged ARM client's
`--version`, static `kexec --help`, SSH host-key generation and `sshd -t`/`-T`.
The SSH check can be repeated as root in WSL:

```sh
sudo python3 hardware/buildroot/scripts/test-connector3-ssh.py \
  "$HOME/.cache/raumfeld-buildroot/connector3-ram"
```

The reusable initramfs contains no home Wi-Fi credentials. During the first tests,
`etc/wpa_supplicant.conf` with mode `0600` was supplied in a separate compressed
`newc` archive under ignored `hardware/buildroot/private/connector3/`. Concatenating
that archive after `rootfs.cpio.gz` creates the private boot initrd. Keep both the
overlay and the combined initrd private; never export them with the reusable
image. The current profile uses the shared network supervisor and its private
`etc/galdrun-network.conf` data file instead of that earlier supplicant-only
bootstrap. Supply credentials there for future boots. The init starts the network
supervisor, SSH and the client with `active:false`;
its settings listener is port **4720**. SSH host keys are generated anew in RAM.
If neither interface obtains an IPv4 address within two minutes, this diagnostic
init requests a reboot to recover the installed firmware.

The DTS preserves observed vendor GPIO levels for the first warm boot and leaves
CPU frequency changes disabled. It is not a qualified cold-boot board definition.
SAI1 is exposed as ALSA card `Connector3`, with the client defaulting to
`hw:CARD=Connector3,DEV=0` and 48 kHz. The MCU controls the physical audio hardware;
audible playback, mute handling and input support are not established.

The stock beta has `CONFIG_KEXEC=y`. Files were uploaded only under
`/run/galdrun-ram-test/`, then read back over SSH and SHA256-checked on the PC.
The static loader successfully accepted:

```sh
/run/galdrun-ram-test/kexec -l /run/galdrun-ram-test/zImage \
  --dtb=/run/galdrun-ram-test/connector3.dtb \
  --initrd=/run/galdrun-ram-test/rootfs-private.cpio.gz \
  --append='console=ttymxc0,115200n8 rdinit=/init clk_ignore_unused maxcpus=1 panic=10'
cat /sys/kernel/kexec_loaded
```

`kexec_loaded` returned **1**. Loading does not execute the kernel. For the first
execution, both installed UBIFS mounts (`/update` and `/`) were successfully
remounted read-only after `sync`, then `kexec -e` was requested. No custom payload
was installed in NAND and no bootloader/environment changes were made. A normal
power cycle returns to installed firmware; do not hold Reset for that recovery.
Hardware boot, Wi-Fi/SSH return and audio must be verified separately from the
successful build and load.

The first execution did not return a Galdrun listener. The device automatically
rebooted into the installed beta: SSH again reported kernel `4.1.45`, firmware
`2.12-snapshot-7`, and a fresh uptime. The current prototype also reserves 1 MiB
at `0x9d000000` for `ramoops` console/crash records to diagnose another warm boot
without UART. This is inside the vendor's CMA region; recovery is best effort
because a subsequent vendor boot may reuse that RAM. A cold power cycle loses
these logs. No persistent boot-log writes are used.

The second attempt recovered **15,292 bytes** of console output from that RAM
after the beta restarted. It confirmed Linux **6.12.112** entered successfully,
unpacked the initramfs, then faulted while probing CAAM at physical
`0x30900004` (`caam_probe`, followed by `Attempted to kill init!` and a ten-second
panic reboot). The live vendor DTS already disables `caam@30900000`; the RAM DTS
now also disables its mainline `&crypto` node. This establishes the early kernel
failure, not a Wi-Fi authentication or app-setup failure. Raw recovered memory
and the decoded console are kept only under ignored `private/connector3/`.

With CAAM disabled, the third attempt reached `Run /init as init process`,
registered ALSA card **Connector3**, and loaded SDMA firmware **4.6**. It did not
obtain a network address: Ethernet reported a missing MDIO PHY and the PCIe
controller reported `Phy link never came up`, so no wireless endpoint appeared.
The two-minute recovery reboot ran successfully and the beta returned over
Wi-Fi. The fourth attempt supplied a PCIe reset pulse on GPIO4_23, returning to
the observed low idle level. It still reported `Phy link never came up`, reached
`/init`, then automatically recovered to the beta after the network timeout.
The custom kernel's PCIe endpoint bring-up and Ethernet PHY access remain
unresolved; there is no qualified persistent installer. The final init also
copies recovery logs into the RAM console one line at a time, respecting the
kernel log record size limit.

## Temporary Galdrun client on the vendor kernel

The current ARMv7 client also ran natively on the beta's **4.1.45** kernel, using
the new build's ELF loader and libraries from **`/run/galdrun-stock-runtime/`**.
No client service or custom firmware was installed in NAND. The settings page
returned HTTP **200** at **`http://10.0.0.58:4720/`**, and `POST /api` with
`{"command":"status"}` confirmed version **0.1.0**, ARMv7, live Wi-Fi signal,
and an idle session. This is a separate runtime test, not proof that the new
kernel's wireless driver works.

Both stock playback PCMs were initially `RUNNING`, owned by the vendor renderer;
the first Galdrun scan consequently found no usable formats. Stopping
`/etc/init.d/S99master-process stop` released both outputs while leaving ConnMan,
Wi-Fi, hardwared and SSH running. A subsequent `rescanAudio` succeeded, and status
reported `formatSelected:true` with exclusive access to **`hw:CARD=Audio,DEV=1`**.
The other stock output is **`hw:CARD=Audio,DEV=3`**. The rescan found **96 exclusive
formats** on each output. Audible playback is untested.

The temporary config keeps `active:false`, no server configured, and standby/
deep-sleep timers disabled. It sets `volumeControl:"software"` because the beta
exposes a `Drift compensator` mixer that auto-detection otherwise treats as
hardware volume. Bit-perfect policy can disable software volume; no hardware
volume/clock-control qualification is implied.

This runtime and its config/cache/logs live entirely under `/run` and disappear
on reboot. To stop that test and resume the vendor application without rebooting:

```sh
kill -TERM "$(cat /run/galdrun-stock-runtime/client.pid)"
/etc/init.d/S99master-process start
```

Only use that PID file for the current running test, not after a reboot or for a
different process. A normal reboot also restores the stock startup sequence.

## Touch buttons, audio names and network settings

The top touch controls are delivered through the vendor MCU daemon, not the
single `gpio-keys` evdev device. The MCU UART is `/dev/ttymxc2` (115200 baud).
The running daemon owns it; do not concurrently read it with `mcu-talk`.
Read-only monitoring with `/raumfeld/hardwared/hw-cli monitor` captured all nine
physical controls in the user's sequence:

| Control | Vendor name | D-Bus button ID | Linux key |
| --- | --- | ---: | --- |
| 1 | `num-1` | 0 | `KEY_1` (2) |
| 2 | `num-2` | 1 | `KEY_2` (3) |
| 3 | `num-3` | 2 | `KEY_3` (4) |
| Previous | `left` | 6 | `KEY_PREVIOUSSONG` (165) |
| Play/pause | `play-pause` | 5 | `KEY_PLAYPAUSE` (164) |
| Next | `right` | 7 | `KEY_NEXTSONG` (163) |
| ? | `multipurpose` | 13 | `KEY_PROG1` (148) |
| Volume - | `minus` | 11 | `KEY_VOLUMEDOWN` (114) |
| Volume + | `plus` | 10 | `KEY_VOLUMEUP` (115) |

D-Bus introspection verified service/interface **`com.raumfeld.hardwared`**,
object **`/com/raumfeld/hardwared`**, and signal
**`ButtonStateChanged(uint32 buttonId, uint32 buttonState)`**. Synthetic
`hw-cli simulate-button` calls confirmed the numeric button IDs and states 0
(pressed) and 1 (short release). The bridge also maps the listed release variants
and ignores held/repeat notifications; those variants still need live validation.
The source-built [`buttons.cpp`](../../hardware/buildroot/board/connector3/ram/buttons.cpp)
exposes nine keys through `/dev/uinput`, forwarding edges and leaving gesture
recognition to Galdrun. It creates no device when the vendor service is absent.
Its `--self-test` passed natively on the beta. All nine generated MCU events were
then observed as press/release events through Galdrun's `/api/input-events` stream;
the live `inputs` API and browser list all nine named controls plus the separate
GPIO key. After the successful network handover, the user repeated the physical
sequence. Galdrun's event stream recorded **all nine controls in the requested
order**, each with one matching press and release (**18 edges**, no keys left
pressed). This verifies the physical MCU → D-Bus → uinput → Galdrun path for
short presses. This bridge depends on the installed vendor daemon; a standalone
mainline MCU driver is not implemented.

Both stock playback PCMs return an **empty** ALSA PCM name. Their IDs differ:
`I2S dit-hifi-1` and `I2S dit-hifi-3`. The ALSA backend now falls back to the PCM
ID, then `PCM <device number>`, instead of displaying `Teufel Audio: ` twice.
The focused `alsa_rescan_tests` regression passes for both empty names and a
missing fallback ID. A new scan replaced the old cached names on the test unit:
both the live API and browser now show `Teufel Audio: I2S dit-hifi-1` and
`Teufel Audio: I2S dit-hifi-3`, each retaining 96 exclusive formats.

Network controls were hidden because the earlier native client ran against the
vendor `/etc`, without Galdrun's board capability file or network supervisor.
The RAM profile now packages `networkSettings:true` and the existing shared
network supervisor, Wi-Fi scanner, setup-AP dependencies and captive portal.
Its `/init` starts that supervisor. No separate network settings implementation
or ConnMan configuration adapter was added to the client.

[`prepare-connector3-vendor-ram.py`](../../hardware/buildroot/scripts/prepare-connector3-vendor-ram.py)
prepares `rootfs.tar.gz` in a private `/run/galdrun-connector3` directory on the
inspected beta. It binds `/dev`, `/proc`, `/sys` and the vendor D-Bus socket into
that RAM root, preserves the existing client settings/identity, supplies named
bindings for the nine touch controls, and writes private network configuration
with mode `0600`. The preparatory client `--version` and bridge self-test passed.
No vendor executable is copied into the reusable image.

In Linux/WSL, after building, preparation is:

```sh
python3 hardware/buildroot/scripts/prepare-connector3-vendor-ram.py \
  10.0.0.58 hardware/buildroot/private/connector3/known_hosts \
  hardware/buildroot/private/connector3/raumfeld-setup.json.example
```

The setup JSON path is private and already contains the user-supplied Wi-Fi
credentials; never print or commit it. Preparation does not stop ConnMan.
Starting the printed `nohup .../start.sh` command is a separate disruptive step:
it stops the earlier test client and ConnMan, then starts the shared network
service, MCU bridge and client from the RAM root.

The first network handover did not return on the LAN or expose a setup AP; its
host-side reboot guard also did not restore access. A normal physical power cycle
returned the beta. Read-only preflight then established two concrete problems:
the vendor SSH PATH is only `/usr/bin:/bin`, and the incremental Buildroot cache
had not installed `wpa_passphrase` despite its newly enabled option. The helper
now sets an explicit PATH and checks all required commands before handover. The
image verifier requires the passphrase binary. Reconfiguring the cached WPA
package installed it; its native key derivation was checked against PBKDF2-HMAC-SHA1
without printing the password or key. When changing options on an existing WPA
package, use `wpa_supplicant-reconfigure` in the prepared Buildroot environment
before rebuilding the image.

The corrected handover **succeeded on 2026-10-28**. The RAM network log recorded a
DHCP lease for **10.0.0.58** from **10.0.0.254**. ConnMan was absent and the RAM
network supervisor was alive. `getNetwork` returned `supported:true`, the correct
SSID and current IP. All nine inputs and both distinct output names remained
available. Only after those checks was `/run/galdrun-connector3/run/verified`
created to cancel recovery. A real `scanNetworks`/`getNetworks` cycle returned
nearby access points with BSSIDs, channels and signal levels.

The real page at **http://10.0.0.58:4720/** was checked in headless Edge with no
mocked API responses: all nine names, distinct outputs, and both WLAN/Ethernet
sections were visible. The existing network UI test also passed at **1200 px**
and **320 px**, including DHCP/static forms, password reveal, AP selection and
save payloads; its network writes were intercepted. Refresh an already-open page
after the handover so it reloads the board capabilities.

**Verified vendor-kernel runtime before mainline attempt 7:** the client, MCU bridge and network supervisor ran from
`/run/galdrun-connector3` on the vendor **4.1.45** kernel. Wi-Fi settings/config
and logs live inside that RAM root. The earlier raw-loader runtime is no longer
the active one. Playback remains inactive and audible output is untested.
The earlier Ethernet no-carrier observation was superseded by the live link
verification below. A normal power cycle clears this runtime and
restores the installed beta's services and network configuration.

The revised guard attempts to stop the RAM network service and restore ConnMan
before resorting to reboot, while retaining supervisor stderr and startup stages.
That failure-recovery path and setup-AP fallback have not been hardware-verified;
they were not needed during the successful handover. The separate mainline
kernel's PCIe Wi-Fi issue remains unresolved; this userland result does not
qualify a persistent custom firmware installation.

## Ethernet link and mainline clock bring-up (2026-10-28)

The same unit is now reachable at **10.0.0.74 over Ethernet**: MAC
`50:1e:2d:3b:77:f4`, `carrier=1`, **100 Mbit/s, full duplex**, default route via
`10.0.0.254 dev eth0`. WLAN is down. RX/TX counters increased during SSH traffic
with both error counters zero. The user reports no visible lights at the RJ45
port; its LED wiring has not been established, and darkness is not evidence
against this measured link. This rules out a completely nonfunctional Ethernet
port, but does not establish why the earlier link attempts failed.

The shared network supervisor prefers Ethernet and disables WLAN after Ethernet
has an address. Consequently, the earlier WLAN address `10.0.0.58` disappearing
was an expected handover; the client page is now **http://10.0.0.74:4720/**.
The vendor SSH identity at the new address was checked against the previously
pinned key using `HostKeyAlias=10.0.0.58`.

The source kernel's board port now matches two observed reference-clock details:

- The working vendor `IOMUXC_GPR1` is `0x4f461005`, with ENET2 clock-direction
  bit 18 set. Mainline i.MX7 initialization clears it. A patch scoped to
  `fsl,imx7d-teufel-streamer` preserves that board setting.
- Mainline `IMX7D_ENET2_IPG_ROOT_CLK` belongs to the AXI/IPG branch, not the PHY's
  50 MHz output. The FEC `enet_out` consumer and 50 MHz rate assignment now use
  `IMX7D_ENET2_REF_ROOT_DIV`, parented to `IMX7D_PLL_ENET_MAIN_50M_CLK`.

These changes require a new hardware RAM-boot check; matching vendor registers
and completing a build alone do not qualify networking or a persistent image.
Changing a kernel patch causes a clean Linux source rebuild; ordinary DTS and
kernel-configuration changes retain the incremental reconfiguration path.

Attempt 5 booted the corrected kernel through `/init` and registered the ALSA
card, but still reported `MDIO device at address 0 is missing`,
`Unable to connect to phy`, `PCIe PLL lock timeout` and
`Phy link never came up`. The 120-second guard rebooted successfully. After
the beta returned over WLAN, bringing `eth0` up restored its Ethernet address
`10.0.0.74`, again with carrier.

Attempt 6, with the same kernel and additional userspace diagnostics, instead
hit a pending SAI interrupt during probe: `fsl_sai_readable_reg` dereferenced
NULL through `regmap_read`/`fsl_sai_isr` immediately after IRQ registration.
The panic reboot recovered the beta. The SAI patch now publishes driver data
before regmap creation and IRQ registration. Its hardware verification is
pending. RAM init also records selected clocks, GPIO states and reference
registers in the warm-reboot console, without network credentials.

The next bring-up candidate also restores the vendor's GPR12 PCIe reference
selection (`0x300040a0`, bit 5 set); mainline cleared that bit in both PHY
initialization and clock enabling. The MDIO bus now pulses GPIO4_22 low for
10 ms, then returns it high and waits 30 ms before PHY probing. The earlier
GPIO hog held that output low continuously.

**GPIO readback correction:** debugfs reported `out lo` even when the vendor's
GPIO output-data register drove a bit high. The pad-status register was zero
for those outputs, so the debug display was not a reliable source for their
commanded levels. Direct register reads with vendor WLAN working showed GPIO1
DR bit 8 high, GPIO4 DR bits 20 and 23 high, and GPIO5 DR bit 2 high. With
Ethernet up, GPIO4 DR bit 22 was high as well (`0x00d0000c` versus
`0x0090000c` with Ethernet down). The earlier DTS mistakenly held all these
outputs low. It now preserves the commanded high levels and treats both reset
lines as active low; GPIO5_11 remains low. This supersedes earlier conclusions
drawn from the GPIO debug display.

The CRC-verified manufacturer source archive `linux-d0c3731730.tar.gz` also
confirms ENET clock-direction bits set, PCIe GPR12 bit 5 selecting the internal
PLL, and the old FEC driver deasserting PHY reset by driving it high. Analysis
copies belong only under ignored `reference/connector3/`. The corrected board
candidate was hardware-verified in attempt 7 below.

### Successful own-kernel RAM boot (attempt 7, 2026-10-28)

At 20:07 CET the device accepted key-only SSH at **10.0.0.74** running our
source-built **Linux 6.12.112**, model `Galdrun Connector 3 RAM test`. The FEC
driver attached **SMSC LAN8710/LAN8720** at MDIO address 0 and established
**100 Mbit/s, full duplex**. Packet counters increased from RX/TX `1093/707`
to `1185/791` during SSH/API checks, with zero RX/TX errors. The earlier
conclusion that the board's Ethernet hardware might be dead is superseded by
working Ethernet under both vendor and custom kernels.

The Galdrun client answered `status` and `rescanAudio` on
**http://10.0.0.74:4720/**. ALSA exposed card `Connector3`, playback device 0;
the rescan selected `hw:CARD=Connector3,DEV=0` and reported
`formatSelected:true`. This tests format negotiation, not audible output.
Only SAI1 is currently described; the vendor's second playback path and MCU
mute/volume behavior remain unqualified.

PCIe also established **Gen1 x1** and detected **11ab:2b38**. The upstream
Marvell firmware downloaded, but command `0xfa` timed out during initialization
and the driver removed its WLAN interface. Disabling MSI and rebinding did not
resolve it. Later isolated hardware-reset/rescan diagnostics produced a firmware
download/CRC failure; these are not a qualified WLAN fix. A suspected missing
32 kHz clock was not confirmed: direct CCM register readback was `0x17000000`
at `0x3038be00`, with the gate set. No clock-register change was applied.

**Current runtime is the own-kernel RAM test**, accessed over Ethernet. Its
fresh client config is inactive, without a server, and has no working MCU button
or LED integration. The prior vendor-runtime client and network settings were
saved privately before boot. The installed beta is unchanged; a normal reboot
returns to it and clears this RAM runtime. Vendor SSH has its original pinned
key; RAM OpenSSH uses the test public key and a separate host-key file.

The build outputs remain in `hardware/buildroot/output/connector3-ram/`.
`hardware-verification.json` records the live checks and hashes of the tested
kernel, generic DTB and generic initramfs; `ram-verification.json` remains the
separate build-only check. The device-specific boot DTB additionally injects
this unit's existing Ethernet MAC, and private network settings are loaded in
a separate ignored overlay, never in the generic artifacts. This successful
kexec boot is not a persistent installer or a cold-boot qualification.

## Output routing, LEDs and the custom-kernel image

The live vendor DT labels playback **link@1** as `i2s out` on SAI1
(`308a0000.sai`) and **link@3** as `spdif out` on SAI2 (`308b0000.sai`). These
are two separately addressable PCM paths, consistent with the analogue/DAC and
digital S/PDIF routes. Their labels do **not** establish that the physical
connectors are mutually exclusive, or prove whether hardware/MCU routing mirrors
one path to both connectors. The original renderer was observed holding both
playback PCMs open simultaneously, consistent with software feeding both paths.
Parallel output needs an actual output test. Galdrun currently opens the selected
PCM only; automatic mirroring to both connectors has not been established. In
contrast, the Connector 2 board driver describes both codecs on one shared McASP
audio link. The
[manufacturer manual](https://assets.teufelaudio.com/products/Teufel_Streamer/Teufel_Streamer_UM_DE_V3.1_HC_WEB.pdf)
identifies the analogue RCA and optical TOSLINK outputs on page 5. Physical
audible-output confirmation is still pending; the successful format scan alone
does not verify the DAC's mute/volume state or sound at either connector.

The Streamer has a front LED strip and a separate LED in the rear Bluetooth
pairing button (manual page 9). Its exact strip segment count has not been
established. `/sys/class/leds` is empty on the inspected beta. The vendor MCU API
offers status indication patterns (`set indication`, command `0x0020`),
Bluetooth indication (`0x0021`) and global LED brightness (`0x0022`). Therefore
the Connector 2's two GPIO LED model does not describe this unit. Galdrun's MCU
LED integration has not yet been implemented; do not enable the existing
two-LED capability merely because the physical indicators work under hardwared.
See also the manufacturer's
[LED-strip recovery description](https://support.teufel.de/hc/en-us/articles/360011716460).

The PCIe issue belongs specifically to the experimental **6.12.112 kernel**:
its internal link to the Marvell Wi-Fi chip never comes up, before Wi-Fi
association can begin. The currently running **4.1.45 vendor kernel** has a
working PCIe/Wi-Fi driver and working WLAN. Keeping that kernel avoids this
specific blocker; it does not require solving the mainline port first.

The requested final image uses our own kernel, like Connector 2. The vendor
kernel remains a working hardware reference and recovery environment. Bring up
Ethernet in the source-built RAM kernel first, then resolve PCIe Wi-Fi, MCU
support and actual audio/mute/volume behavior before preparing and qualifying
the persistent USB installer. The current button bridge still depends on the
vendor MCU daemon and is not a standalone MCU implementation. The complete RAM
bundle also does not fit the existing 12 MiB diagnostic FIT slot unchanged.
No persistent custom installation has been performed. A successful kexec RAM
boot does not qualify cold boot, NAND installation, or rollback.

## Active defaults, attach and native touch controls (2026-10-28)

The generic client defaults to `active:true`; the RAM profile had overridden
that with `false`. The Connector 3 RAM defaults and vendor-RAM preparation now
also default to active. Existing explicit settings are retained. The initramfs
verifier checks the actual archived default and the nine physical input bindings.

The reported attach error was reproduced at the core as
`could not connect to 10.0.0.74:4720: no answer`, even though direct HTTP and
TCP-line `status` calls answered immediately. The same compiled connection
helper also succeeded in a separate process. After rebuilding and restarting
the Windows core, `client.attach` succeeded and set this client to
`10.0.0.6:4711`. The room ID `c170ac6d4af8eddd` and server assignment are
preserved in the private overlay between RAM tests. No speculative socket-code
workaround was retained; long-running recurrence has not been excluded.

Attachment also exposed a separate capability error: ALSA advertised eight
channels and then `fsl_sai_hw_params` rejected them. A Streamer-only startup
constraint now advertises the physical stereo channel count. The kexec handover
also inherited SAI TX/RX registers `0x90130c01` and accumulated millions of
interrupts during startup. Another Streamer-only patch stops the inherited
streams, with the bus clock enabled, before requesting the IRQ. Both changes
require verification of the newly built kernel, not just an old client cache.

The mainline DTS now enables UART3, `/dev/ttymxc2`, on the published RX/TX pins
at 115200 baud. [`mcu-buttons.cpp`](../../hardware/buildroot/board/connector3/ram/mcu-buttons.cpp)
implements a native serial-to-uinput path without `hardwared` or D-Bus. The
vendor-kernel `buttons.cpp` bridge remains available separately.

The MCU uses a 12-byte little-endian header: start `0x55`, command type byte,
16-bit command ID, 32-bit value, transaction byte, checksum byte and 16-bit
payload length. The checksum negates the sum of the remaining header bytes
and payload, excluding the start byte. Notify type 2 / command `0x0040`
contains the button in the high 16 bits and state in the low 16 bits. A transport
acknowledgement is Notify command 0, status 0 and the received transaction.
The driver validates framing and checksum, bounds payload length, acknowledges
retries and suppresses duplicate edges. Its self-test covers partial frames,
bad checksums, resynchronization, oversized payloads and acknowledgements.

The corrected physical capture recorded all nine controls in order. Raw MCU
IDs are `0,1,2,6,5,7,3,10,9` for presets 1–3, Previous, Play/Pause, Next,
Multipurpose, Volume − and Volume +. State 0 is pressed, states 1–4 release;
observed `0x10` hold notifications do not create new edges. These raw IDs differ
from the vendor D-Bus IDs for Multipurpose and volume. The native daemon creates
`Connector 3 touch buttons`, and the live client `inputs` API exposes all nine
named bindings. Physical verification through the final evdev/client path is
separate from the earlier raw UART capture.

### Scanning with Ethernet connected

Ethernet preference disables association and routing, not the ability to scan.
The scan helper can use a temporary managed interface. For drivers supporting
only one managed interface, it now temporarily raises a down interface, then
restores it unless the station/AP manager claimed it in the meantime. Tests
cover success, failure, timeout, cancellation, excessive output, already-up and
down single-interface radios, concurrent station ownership and a missing radio.

`Wi-Fi interface is not ready` on this current mainline port is additionally a
real driver-initialization problem: no Wi-Fi interface exists after the firmware
timeout. An explicit PCIe `dma-ranges` now programs the inbound window
`0x80000000..0x9fffffff`; live register readback confirmed it, but it alone did
not fix firmware initialization. A live comparison using the manufacturer's
firmware and an isolated radio reset also failed and is not a qualified fix.
That binary was used only in RAM and is not included in the generic image.
The next diagnostic kernel disables PCIe link power transitions and enables
targeted firmware-start logging. A scan-worker fix cannot replace a working
radio driver.

The RAM kernel deliberately disables suspend/PM. ARM SMP consequently does not
offer `KEXEC`; the previously requested Kconfig symbol was ineffective and was
removed. Each subsequent test is loaded by the unchanged vendor beta instead.
The static `kexec` binary in the artifact is for that vendor-side loader, not
evidence that the running RAM kernel can load another kernel. A normal reboot
returns to the installed beta. Live settings must be saved privately first.

## Local evidence and references

Ignored analysis files under `hardware/buildroot/reference/connector3-ssh-archive/`:

- `inspection.json`: archive versions, hashes and activation-hook inspection.
- `live-device-description.xml`: target hardware ID and SetupService description.
- `device-beta-payload.bin`: payload read back from the actual USB update server.
- `live-update-request.json`: exact version string, target and accepted response.
- `live-ssh-verification.json`: post-update SSH and activation-marker results.
- `live-stock-2.12.dtb` / `.dts`: live vendor hardware description.
- `phy-restart-result.txt`: unsuccessful Ethernet negotiation retry.
- `ram-load-verification.json`: RAM uploads read back and hashed; kernel loaded.
- `ram-execution-request.json`: temporary boot request and SSH disconnect/timeout.
- `ram-*-attempt{1,2,3,4,5,6,7}.json`: preserved load/execution evidence.
- `ram-mainline-hardware-verification.json`, `ram-mainline-live-console.log`,
  `ram-mainline-live-status.txt`: successful own-kernel boot, Ethernet and PCIe.
- `ram-mainline-live-client.json`, `ram-mainline-audio-rescan.json`: the real
  client API and format scan under our kernel.
- `stock-runtime-status.json`, `stock-runtime-audio-devices.json` and
  `stock-runtime-audio-rescan.json`: native client, control API and audio scan.
- `ram-live-input-events.jsonl`: all nine generated MCU events reaching Galdrun.
- `ram-physical-input-events.jsonl`, `ram-physical-input-verification.json`:
  all nine physical controls delivering matching press/release pairs in order.
- `ram-live-network.json`, `ram-live-inputs.json`, `ram-live-audio-devices.json`:
  verified network ownership, named controls and distinct output labels.
- `ram-live-wifi-scan.json`, `ram-live-browser.json`: real wireless scan and
  browser visibility of all three requested features.

The original lead was [Kargol's archived installer offer](https://www.hifi-forum.de/viewthread-212-2470-2.html#61)
and [Kojak_LE's beta update method](https://www.hifi-forum.de/viewthread-212-2470-2.html#62).
The successful direct update and SSH checks above were performed on this device;
they do not depend on assuming the phone-app procedure works.
