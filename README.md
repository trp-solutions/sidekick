# ESP32 display and TinyProcess

- [`esp32/`](esp32/README.md): ESP32-C5 firmware, PlatformIO/CLion configuration and firmware media.
- [`tinyprocess/`](tinyprocess/README.md): Electron desktop app, configuration, video/overlay rendering and packaging.
- [`.github/workflows/build.yml`](.github/workflows/build.yml): builds both projects, then publishes artifacts on `v*` tags.

## Desktop app

```sh
cd tinyprocess
npm ci
npm start
```

Set the config endpoint in Settings. See the [desktop README](tinyprocess/README.md) for endpoint examples and button actions.

## ESP32 firmware

```sh
cd esp32
python3 -m pip install platformio==6.1.19
pio run
pio run --target upload
```

Open `esp32/` as the CLion CMake project. Existing CMake build directories refer to the old layout; configure a new build directory after the move.