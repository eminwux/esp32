# Experiment <ID>: <purpose>

Replace these placeholders when copying this template. Add the new experiment
to the repository catalog.

- Board/framework: ESP32 `esp32dev`, Arduino.
- Wiring: list connections, power, and any extra hardware.
- Dependencies: list libraries declared in this project's `platformio.ini`.
- Expected behavior: describe serial output and observable hardware behavior.
- Calibration/roles: document each environment and its flags, if applicable.
- Validation: record builds and hardware checks actually performed.
- Known limitations: record unfinished work and hardware assumptions.

From the repository root, replacing `<ID>`:

```sh
pio run -d experiments/<ID> -e esp32dev
pio run -d experiments/<ID> -e esp32dev -t upload
pio device monitor -d experiments/<ID> -e esp32dev
```

The starter prints `Experiment ready` at 115200 baud. Override serial ports with
`--upload-port PORT` for upload and `--port PORT` for the monitor.
