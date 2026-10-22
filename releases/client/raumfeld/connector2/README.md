# Galdrun for Raumfeld Connector 2

This turns your Raumfeld Connector 2 into a Galdrun speaker. After the update
it starts Galdrun by itself every time it is switched on.

> **Before you start:** this replaces the Raumfeld software on the device. The
> Raumfeld app will no longer work with it. Do not unplug the power while the
> update is running.

## What you need

- A USB stick, formatted as **FAT32** (most sticks up to 32 GB already are).
  Anything already on the stick can stay; only the file below is added.
- The file **`connect2.img`** from this folder.

## Install

1. **Copy the file.** Copy `connect2.img` onto the USB stick. Put it directly
   on the stick, not into a folder, and do not rename it.
2. **Switch the Connector 2 off.** Pull out its power plug.
3. **Plug in the USB stick.**
4. **Hold the Reset button and switch it on.** Keep holding Reset, plug the
   power back in, and keep holding for about 10 seconds, then let go.
5. **Wait.** The two lights blink in turn while the update is running. This
   takes a few minutes.
6. **Let it restart automatically.** Both lights turn on briefly after the
   update passes its checks, then the Connector 2 restarts into Galdrun.
   Remove the USB stick after it has restarted. No manual power-cycle is needed.

The Connector 2 now starts with Galdrun.

**If, during the update (USB stick still plugged in), one light stays on and
the other blinks quickly**, the update failed. Copy `connect2.img` onto the
stick again and repeat the steps above. If it still fails, go back to the
original Raumfeld software (see below).

## Going back to the Raumfeld software

The update can be undone with Teufel's official reinstallation, which works the
same way: download Teufel's `connect2.img` onto the USB stick (replacing
Galdrun's) and follow
[Teufel Raumfeld devices: reinstalling the firmware](https://support.teufel.de/hc/en-us/articles/360001499849-Teufel-Raumfeld-devices-reinstalling-the-firmware)
([Deutsch](https://support.teufel.de/hc/de/articles/360001499849-Teufel-Raumfeld-Ger%C3%A4te-Neuinstallation-der-Firmware)).
Your Galdrun settings are removed.

## Connect it to your network

- **With a network cable:** plug it in. The Connector 2 gets its address from
  your router usually automatically (DHCP) and shows up in Galdrun.
- **With Wi-Fi:** without a network cable, the Connector 2 opens its own Wi-Fi
  network:
  1. On your phone or computer, join the Wi-Fi network **`Connector2`**,
     password **`connector2`**.
  2. Open the setup page when prompted, or tap the phone's **Sign in to Wi-Fi**
     notification. If no prompt appears, open **http://192.168.77.1:4720/**.
     If that does not work, temporarily disable mobile internet and try again.
  3. Choose your home Wi-Fi, enter its password and save.

  The Connector 2 then joins your home Wi-Fi and shows up in Galdrun.

You can open the same settings page later at `http://<device address>:4720/`
to change the room name, device name, network or sound settings. Device name
sets its name on the network; leave it empty to use the default.

**Remote login (optional, for experts):** SSH is off. To use it, open the
settings page, go to **Remote login (SSH)**, choose a password and turn it on; then
log in as `root`. Keep the password: changing or turning off SSH later needs it,
and without it only installing the update again helps.

## What the lights mean

After the update, in normal use:

| Lights | Meaning |
|---|---|
| One blinks slowly, the other is off | Starting up |
| One on, the other blinks slowly (once a second) | Looking for the Galdrun server: check that the server is running and on the same network |
| One on, the other blinks twice every 2 seconds | Not set up yet |
| Both on | Ready to play |
| One on, the other blinks quickly | Something is wrong: unplug the power, wait 10 seconds, plug it back in. If it stays like this, install the update again |
| Both off | Standby or switched off; it wakes up when you play music to it |

## Check the download (optional)

To make sure `connect2.img` was copied correctly, compare its fingerprint with
`image_sha256` in `installer-manifest.json`. On Windows, open PowerShell and run
(replace `E:` with your USB stick's drive letter):

```powershell
Get-FileHash E:\connect2.img
```

The long number shown must match exactly. If it doesn't, copy the file again.
