# Galdrun client for @MULTIROOM_BOARD@ (@IDF_TARGET@)

Flash images for the `@MULTIROOM_BOARD@` board, written by the last build of
`client/esp32` for it (`client/esp32/boards/@MULTIROOM_BOARD@.defaults` has its
wiring). `flash_args` lists each image with its offset; the settings in
`flasher_args.json` are the same for other flashing tools. These images only
boot on an @IDF_TARGET@, and only play through the wiring that board file names.

From this folder, with esptool (`pip install esptool`, or an ESP-IDF shell):

```
python -m esptool --chip @IDF_TARGET@ -p COM5 -b 460800 --before default_reset --after hard_reset write_flash "@flash_args"
```

Replace `COM5` with the board's port (`/dev/ttyUSB0` on Linux). The `storage`
partition is not flashed; the client formats it on first start and writes the
board's buttons to `client.json` there. Wi-Fi setup and settings are described
in `client/esp32/README.md`.
