# Display Firmware

ESP-IDF firmware for a display-only command device based on the ESP32-2424S012
ESP32-C3 1.28 inch 240x240 round LCD board.

This board is expected to have a 240x240 SPI LCD, likely GC9A01, and CST816S
I2C touch controller.

Current working assumptions:

- LCD SCLK: GPIO6
- LCD MOSI: GPIO7
- LCD MISO: unused
- LCD DC: GPIO2
- LCD CS: GPIO10
- LCD RST: unused
- LCD backlight: GPIO3
- Touch SDA: GPIO4
- Touch SCL: GPIO5
- Touch RST: GPIO1
- Touch INT: GPIO0

## Configure

```sh
idf.py menuconfig
```

Set:

- `Spoken command display -> Wi-Fi SSID`
- `Spoken command display -> Wi-Fi password`
- `Spoken command display -> Command server base URL`

The server URL should be the base server address, for example:

```text
http://192.168.4.20:8080
```

## Build And Flash

```sh
idf.py set-target esp32c3
idf.py -p /dev/ttyACM0 flash monitor
```

The device derives its ID from its Wi-Fi MAC suffix, for example
`waveshare-c3-display-a1b2c3`, and polls:

```text
/devices/{device_id}/events
```

Currently supported event:

```json
{"type":"alert","display_text":"Alert","tone":"alert"}
```
