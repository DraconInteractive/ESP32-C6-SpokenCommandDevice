# Spoken Command Firmware

ESP-IDF firmware for the Waveshare ESP32-C6 Touch AMOLED 1.8 board.

This project starts from the validated hardware test firmware:

- SH8601 AMOLED display over QSPI
- FT5x06 touch over I2C
- QMI8658 IMU over I2C
- ES8311 microphone and speaker audio over I2S
- BOOT button on GPIO9 for interactions
- PWR button via AXP2101 PMU short-press IRQ for soft standby

## Build

Set network values before building. These values are stored in local
`sdkconfig`, which is intentionally ignored by git:

```sh
. /home/demo/esp-idf-env.sh
idf.py menuconfig
```

Configure:

- `Spoken command device -> Wi-Fi SSID`
- `Spoken command device -> Wi-Fi password`
- `Spoken command device -> Command server audio endpoint`

Then build:

```sh
. /home/demo/esp-idf-env.sh
idf.py build
```

## Flash

```sh
. /home/demo/esp-idf-env.sh
idf.py -p /dev/ttyACM0 flash monitor
```

## Next Firmware Work

Current interaction behavior:

- `PWR` toggles soft standby.
- Hold `BOOT` to stream mono `S16LE` PCM at 16 kHz to `/audio/command`.
- The top band shows recording/upload/result status.
- A subtle bottom meter shows ambient microphone level.
- The display renders the returned transcript text.

Next work should replace compile-time network configuration with on-device
configuration storage.
