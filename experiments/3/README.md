# Experiment 3: LED blink and serial output

Board: `esp32dev`. Framework: Arduino. Serial monitor: 115200 baud.

## Wiring

Built-in LED on GPIO 2; no radio required.

## Run

From the repository root:

```sh
pio run -d experiments/3 -e esp32dev
pio run -d experiments/3 -e esp32dev -t upload
pio device monitor -d experiments/3 -e esp32dev
```

For a different serial device, pass `--upload-port PORT` when uploading and
`--port PORT` when monitoring.

Prints `ESP32 initialized!`, then alternates `ON` and `OFF` every second.

## Dependencies and limitations

LED availability and polarity depend on the board. Uses Arduino only.
