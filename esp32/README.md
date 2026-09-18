# ESP32 display firmware

Firmware for the **ESP32-C5-DevKitC-1** with a 160×160 display and two buttons.
It receives video frames from [TinyProcess](../tinyprocess/README.md) over native
USB and sends button events back. The built in colors animation plays on boot
and after two seconds without video data.

Run the commands below from `esp32/`.

## Build and flash

Install PlatformIO, then build:

```sh
python3 -m pip install platformio==6.1.19
pio run
```

Connect the board and close TinyProcess or any serial monitor before flashing:

```sh
pio run --target upload
```

If needed, add `--upload-port /dev/ttyACM0` with your board's port.
Use the board's **native USB port** for TinyProcess video and button input.

## Display wiring

The firmware uses these SPI display connections:

| Display pin | ESP32-C5 pin |
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
| 1 | GPIO23 to GND |
| 2 | GPIO24 to GND |

Internal pull-ups are enabled; no external resistors are needed. Do not connect
the buttons to 5V. Single press, hold, double click, and combined-button gestures
are configured through [TinyProcess button actions](../tinyprocess/README.md#buttons-and-window-actions).
