


# Testbench


## Client
| Device               | OS          | Soundcard                      | Chip    | API     | HPP | Tested  | Notes                           
| -------------------- | ----------- | ------------------------------ | ------- | ------- | --- | ------- | -------
| RPI 1 Model B 1.2    | DietPi      | [USB] DAC                      | KT0210  | ALSA    | N   | Yes     | Common
| RPI 1 Model B 1.2    | DietPi      | [I2S] InnoMaker HiFi DAC       | PCM5122 | ALSA    | N   | Yes     | Custom Firmware
| RPI 3 Model A        | DietPi      | [PWM] Headphone Jack           | BMC2835 | ALSA    | N   | Yes     | Custom Firmware
| ESP32-S31 (Protoype) | freeRTOS    | [I2S] InnoMaker HiFi DAC       | PCM5122 | IDF-I2S |     |         |
| ESP32-S31 Core Board | freeRTOS    | [I2S] InnoMaker HiFi DAC       | PCM5122 | IDF-I2S |     |         |
| ESP32-A1S-Audio-Kit  | freeRTOS    | [I2S] DAC                      | ES8388  | IDF-I2S |     | Yes     | WLAN Bottleneck / Max. 96khz with 24bit on good conditions
| Raumfeld Connector 2 | linux       | [I2S] DAC                      | CS4271  | ALSA    | N   | Yes     | Custom firmware 
| Raumfeld Streamer    | linux       | [I2S] DAC                      |         | ALSA    | N   | Yes     | Custom firmware 
| Native PC x64        | Windows 11  | [Buetooth] Jabra Evolve 75     |         | WASAPI  | N   | Yes     |
| Native PC x64        | Windows 11  | [USB] DAC Behringer UMC202HD   | CS4272  | WASAPI  | N   | Yes     |
| Native PC X64        | WSL-Ubuntu  | [Buetooth] Jabra Evolve 75     |         | ALSA    | N   | Yes     |

HPP = Headphone pairable

## Server / ServerCore



## Featured Devices
| Device               | PWR Control |   | PWR DEEP | PWR STAND | PWR PLAY  | DAC Control | Client Web | Bitperfect | WLAN       | LAN | Line In | HW Drift | BUTTONS                   | Type 
| -------------------- | ----------- | - | -------- | --------- | --------- | ---------   | ---------- | ---------- | ---------- | --- | ------- | -------- | ------------------------- | ------------------
| Raumfeld Connector 2 | FULL        |   | ~0.6 W   | ~2.0 W    | ~2.4 W    | ALSA/I2S    | Yes        | Yes        | Yes        | Yes | No      | Yes      | Power / Setup / Reset     | Custom Firmware
| Raumfeld streamer    |             |   |          |           |           | ALSA/I2S    | Yes        | Yes        | Yes        | Yes | No      | Yes      |                           | Custom Firmware
| RPI 1 Model B 1.2    | NO          |   |          |           |           | ALSA/I2S    | Yes        | Yes        | Yes (USB)  | Yes | No      | Depends  | Depends                   | Custom Firmware
| RPI 3 Model A        | NO          |   |          |           |           | ALSA/I2S    | Yes        | Yes        | Yes        | Yes | No      | Depends  | Depends                   | Custom Firmware


## In Research List
| Raumfeld Streamer
| Raumfeld One S
| Raumfeld Connector 1
| Sonos Connect ZP90 
| Sonos Connect ZP120



# Tested Setups [Client]


