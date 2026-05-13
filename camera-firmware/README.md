# TimerCamera-X Camera Firmware

ESP-IDF firmware for the M5Stack TimerCamera-X. This project exposes the
camera on the local network and registers it with the spoken-command server as
a camera input device.

First boot starts a setup access point:

```text
SSID: TimerCamera-X-Setup
Password: timercam123
URL: http://192.168.4.1/
```

Enter your home Wi-Fi SSID and password. The device stores them in NVS and restarts.

On restart it tries to join the saved Wi-Fi network for 10 seconds:

- Success: starts the camera web server on the home network.
- Failure: clears saved Wi-Fi credentials and restarts back into setup AP mode.

Once connected to home Wi-Fi, open the IP address printed on serial:

```text
http://<device-ip>/
http://<device-ip>/capture
http://<device-ip>/stream
```

The firmware also registers with the command server as:

```text
timercam-x-<wifi-mac-suffix>
```

It advertises:

- `type`: `camera`
- `model`: `M5Stack TimerCamera-X`
- `capabilities`: `capture`, `stream`
- endpoints for `/`, `/capture`, and `/stream`

The command server URL is configured in:

```sh
idf.py menuconfig
```

under `TimerCamera command device -> Command server base URL`.

## Build

```bash
cd /home/demo/spoken-command-device/camera-firmware
. /home/demo/esp-idf-env.sh
idf.py set-target esp32
idf.py build
```

## Flash

Connect the TimerCamera-X over USB, then run one of:

```bash
idf.py -p /dev/ttyUSB0 flash monitor
idf.py -p /dev/ttyACM0 flash monitor
```

Exit monitor with `Ctrl+]`.
