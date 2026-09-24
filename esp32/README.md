# ESP32 display firmware

Firmware for the **ESP32-S3-DevKitC-1** with a 160×160 display and two buttons.
It receives video frames from [Sidekick](../sidekick/README.md) over native
USB and sends button events back. The built in colors animation plays on boot
and after two seconds without video data.

The default target uses 8 MB quad-SPI flash (N8, including N8R8 boards);
PSRAM is not required or enabled. The animation partition layout uses the first
4 MB of flash. Octal-flash WROOM-2 variants need a matching board configuration.

Run the commands below from `esp32/`.

## Build and flash

Install PlatformIO, then build:

```sh
python3 -m pip install platformio==6.1.19
pio run
```

Connect the board and close Sidekick or any serial monitor before flashing:

```sh
pio run --target upload
```

If needed, add `--upload-port /dev/ttyACM0` with your board's port.
Use the board's **native USB port** (labelled USB, not USB-to-UART) for
Sidekick video and button input. GPIO19/20 are reserved for native USB.
If the board is not detected for the first flash, hold **BOOT**, press and
release **RESET**, then release **BOOT** and retry the upload.

When switching from the C5, move the buttons from GPIO23/24 to GPIO4/5.
The display connections stay the same. Reconfigure CLion in a fresh CMake
build directory so it selects the S3 Xtensa toolchain.

## Display wiring

The firmware uses these SPI display connections:

| Display pin | ESP32-S3 pin |
| --- | --- |
| MOSI / SDA / DIN | GPIO7 |
| SCLK / SCL / CLK | GPIO6 |
| CS | GPIO10 |
| DC / A0 | GPIO8 |
| RST / RES | GPIO9 |
| BL / BLK (backlight control) | GPIO1 |
| GND | GND |

GPIO numbers refer to the board labels, not header positions. MISO is unused.
Connect display VCC according to the display module's supply requirements.
These assignments are defined in [`src/main.cpp`](src/main.cpp).

## Buttons

Connect normally open momentary switches:

| Button | Connection |
| --- | --- |
| 1 | GPIO4 to GND |
| 2 | GPIO5 to GND |

Internal pull-ups are enabled; no external resistors are needed. Do not connect
the buttons to 5V. Single press, hold, double click, and combined-button gestures
are configured through [Sidekick button actions](../sidekick/README.md#buttons-and-window-actions).

Board pinout: [Espressif ESP32-S3-DevKitC-1 guide](https://docs.espressif.com/projects/esp-dev-kits/en/latest/esp32s3/esp32-s3-devkitc-1/user_guide_v1.1.html).

Firmware 1.10 samples buttons every millisecond independently of video, with
10 ms debounce. Single clicks wait 300 ms to distinguish double clicks.

Run the button gesture regression tests on the host with:

```sh
g++ -std=c++17 -I include test/buttons.test.cpp -o /tmp/sidekick-buttons-test
/tmp/sidekick-buttons-test
```
