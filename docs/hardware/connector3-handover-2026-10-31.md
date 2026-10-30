# Connector 3 handover for 2026-10-31

Work stopped at the user's request on 2026-10-30. No further hardware tests or
installation should run today. Resume when the user asks.

## Start with the user's final firmware decision

**Use the working Marvell/NXP hardware firmware blob `15.87.19.p14`.**
The user explicitly instructed:

> add in the handover that we will usee this working blob. The version missmatch for the licence is we dont care

Do **not** keep searching for a different firmware or make an exact licence-
version match a prerequisite. The mismatch is known and explicitly accepted by
the user. Keep the available manufacturer licence notice with the hardware
firmware, as with Connector 2. Do not claim the short check verified an official
download or an exact-version licence; it did not. This decision authorizes
using this specific working hardware blob, not copying the vendor rootfs/apps.

```text
Version: 15.87.19.p14
Size: 673220 bytes
SHA256: b2c5e7f4f88b061b92be8113ddd5ec688da94729c3a5d07bec9e334335499538
Local source: hardware/buildroot/private/connector3/pcie8897-working-reference.bin
Runtime destination: /lib/firmware/mrvl/pcie8897_uapsta.bin
Embedded identity: $Id: w8897o-B0, RF87XX, FP87, 15.87.19.p14 $
Additional embedded branding: Marvell Micro AP
```

The earlier assumption that extraction from the device makes it Raumfeld code
was too strict. Manufacturer hardware microcode is compatible with the
Connector 2 approach. Continue excluding Raumfeld/OEM OS files, applications,
daemons, proprietary libraries and the vendor-dependent D-Bus bridge.

**First code correction tomorrow:** `hardware/buildroot/scripts/connector3_content.py`
currently blocks this exact digest pending provenance review. Update that guard
to allow the user-approved hardware firmware while retaining the OEM-runtime
checks. `verify-connector3-ram.py` also marks explicit local-radio builds
ineligible for the clean installer; adjust that classification for this approved
blob. Do not relabel its source as an independently downloaded NXP release.
Those implementation changes were intentionally left for tomorrow.

## Verified end-of-day state

- Board is back on the **unchanged original beta `2.12-snapshot-7`, kernel
  `4.1.45`**, verified by UART and the existing trusted SSH connection at
  **`10.0.0.58` over WLAN**. Ethernet `.74` was not responding in beta.
- No custom firmware was installed. No NAND erase/program, bootloader update,
  environment repair or `saveenv` was performed.
- No RAM experiment remains active. UART capture PID 35136 was stopped and
  COM5 released. The final log is saved at
  `hardware/buildroot/reference/connector3-ssh-archive/clean-wifi-end-of-day-20261030.log`.
- Local Galdrun server/core/client are stopped. No owned Connector 3 compiler
  processes remain. Do not restart a stack just because an older checkpoint
  says music was running.
- For recovery after any later failed RAM boot: remove the USB stick and power
  cycle normally **without Reset**. Use the beta's original known-hosts file;
  never globally disable SSH host-key checking.

## Results worth keeping

1. Signed source-built USB diagnostic **cold boot, Ethernet DHCP and key-only
   SSH passed**. `output/connector3-diagnostic/connect3.img` SHA256:
   `1e117314f90c1692df5c9f274ba34f3e608aac2ca66c54d9c57592bdf188bdb2`.
   It is RAM-only and cannot install anything.
2. New-kernel **NAND geometry/BCH8 reads, UBI read-only attach, UBIFS read-only
   mount/unmount, and USB/FAT32 stick reads passed**. All six MTD flags were
   `0x0`; original environment/constants/FIT hashes matched; zero uncorrectable
   ECC errors. These were protected kexec tests, not storage cold-boot or
   erase/program qualification.
3. Geometry is 512 MiB SLC, 4096-byte pages, 256-byte OOB, 256 KiB eraseblocks,
   BCH8/512. UBI VID offset 4096, data offset 8192, LEB 253952. Preserve bootloader,
   env, constants and settings partitions. The FIT is MTD4, rootfs MTD5; do not
   reuse Connector 2's partition indices or image format.
4. The old UBI allocation produced `reserved 25, need 34` under the modern
   kernel. Leave enough bad-block reserve when creating a new UBI layout.
5. Standalone RAM packaging now omits `galdrun-connector3-buttons`, which
   depends on the vendor D-Bus service. Its explicit historical vendor-test
   helper builds/uploads that bridge separately. Galdrun's own direct MCU
   service remains in the standalone image.
6. The rebuilt generic RAM archive passed content/source checks: 22 packages,
   81 ELF files, no vendor runtime bridge or saved home Wi-Fi credentials.
   Keep those content checks, allowing the specifically approved radio blob.

The known audio kernel remains:
`f9ef8e2f3fdb35372e74869b839fa1da9da1baafa2de9ceaba3bb909de70f044`.
The older working private radio build remains under
`hardware/buildroot/private/connector3/ram-local/`; it has older packaging, so
rebuild the cleaned standalone userspace with the approved radio instead of
blindly declaring that entire old bundle release-ready.

## Experiments to avoid repeating

- Independently published NXP p154 loads and reports firmware active, then
  times out at first command `0xfa`, printed action `0x3000`; no Wi-Fi interface.
- A second p154 boot with a low PCIe DMA bus aperture failed identically.
  Do not promote that experimental range into the base DT.
- p119 was packed and audited, **not booted**. p53 was downloaded, **not tested**.
  There is no need to pursue these replacements under the user's final decision.
- A standard `pci=nomsi` comparison kernel was compiled, **not booted**:
  `output/connector3-clean-radio-legacy-irq/zImage`, SHA256
  `51d655d68dcacf9c66e74fc984c19b19ddf890c7ba38ef22695d5e42f4b2ec6e`.
  The private boot helper still selects the ordinary kernel, not this component.

## Short licence/provenance check

The working blob carries the Marvell identifiers above. The public vendor GPL
archive's manifest lists `linux-firmware` commit
`a179db97914da5e650c21ba8f9b0bae04a0f8a41` and a Marvell licence permitting
unmodified binary redistribution subject to its conditions. That package's
radio is **15.68.7.p77**, not the working **15.87.19.p14**. Therefore the exact
blob-to-licence association was not proven by this check. **The user explicitly
accepted this version mismatch and chose the working blob; do not reopen that
decision as an installation blocker.**

Records: `hardware/buildroot/reference/connector3/license-check-20261030/`
and `reference/connector3-ssh-archive/end-of-day-provenance-and-state-20261030.json`.
Keep reference/vendor files private and out of the image except the specifically
approved hardware firmware. Never package a vendor rootfs or private signing key.

## Next implementation work

Apply the approved-radio exception, rebuild the cleaned standalone image and
retain its manufacturer notice/hash/source attribution. Restore existing
Galdrun identity/settings from private backups for hardware tests; generic
release defaults must not embed this board's identity, home credentials or keys.

Then finish the **separate persistent rootfs/settings and USB installer**:
source/license bundle, signed FIT within 12 MiB, USB payload, protected boot
regions, check/preflight mode, storage cold boot, write/readback and rollback.
There is currently **no writable Connector 3 installer**. Source-built OP-TEE
handoff works but printed initialization/entropy warnings; production monitor
qualification remains open. Do not invent an i.MX7 RNGB address or enable the
i.MX6 RNGB option blindly.

Keep Deep Sleep disabled pending Ethernet wake qualification. Hardware PLL trim
and native rates passed previously. Audible MCU volume/mute remains **pending,
explicitly deferred by the user**; both RCA channels playing were confirmed on
the earlier working image. Do not claim new listening verification.

Read [the detailed checkpoint](connector3.md) for exact earlier artifact hashes,
read-only storage source/build commands, private evidence helpers and known-hosts
files. Inspect `git status` first: substantial pre-existing and new dirty work
is intentionally retained. No commit, reset or cleanup was requested.
