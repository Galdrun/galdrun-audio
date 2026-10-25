# Raspberry Pi client images

Choose the Galdrun Buildroot SD-card image by device model:

| Raspberry Pi models | Image family | Image filename |
| --- | --- | --- |
| Pi 1 Model A, B, A+, B+; Zero, Zero W, Zero WH | `rpi1` | `galdrun-rpi1-sdcard.img.gz` |
| Pi 2 Model B (revisions 1.0, 1.1, 1.2); Pi 3 Model B, B+, A+; Zero 2 W | `rpi3` | `galdrun-rpi3-sdcard.img.gz` |
| Pi 4 Model B, all RAM variants | `rpi4` | `galdrun-rpi4-sdcard.img.gz` |
| Pi 5 Model B, all RAM variants | `rpi5` | `galdrun-rpi5-sdcard.img.gz` |

**Pi 2 B and Zero 2 W use the `rpi3` image.** The original Zero and Zero W/WH
use `rpi1`. A fitted or unfitted GPIO header does not change the image choice;
Zero W and Zero WH share the same image. Each Pi 4/5 family uses one image
across RAM variants.

The `rpi1` image bundles the 32-bit ARMv6 client (`linux-armv6`); `rpi3`
bundles the 32-bit ARMv7 client (`linux-armv7`), including on its 64-bit-capable
models. The separate `rpi4` and `rpi5` images both bundle the 64-bit ARM client
(`linux-armv8`), with board-specific kernels and boot files.

## Image location and setup

Build outputs are in `releases/client/raspberrypi/<image-family>/` relative to
the repository root. Each folder contains its image, `SHA256SUMS`,
`firmware-manifest.json`, `README.md` and `licenses/`. For example, Pi 2 B uses
`releases/client/raspberrypi/rpi3/galdrun-rpi3-sdcard.img.gz`.

These are complete Linux SD-card images. For checksum verification, flashing,
first boot, network/SSH setup, audio profiles and button/encoder wiring, follow
the [Raspberry Pi appliance guide](../../hardware/buildroot/board/rpi/README.md).
Pi 5 uses its existing EEPROM bootloader; the image does not update it.

## Coverage and validation

All four image families passed full builds, host regressions, image integrity
and filesystem checks, and QEMU client checks on 2026-10-25. Per-model physical
qualification remains incomplete: cold boot, Ethernet/Wi-Fi/AP, USB/HDMI/I2S
audio as applicable, GPIO buttons/encoders and 24-hour grouped playback.
Existing hardware test results and their limits are recorded in the appliance
guide's [validation status](../../hardware/buildroot/board/rpi/README.md#validation-status).

Compute Modules and keyboard models such as Pi 400/500 are outside the current
image coverage.
