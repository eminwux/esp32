# Experiment 4: CC1101 SPI VERSION register check

Board: `esp32dev`. Framework: Arduino. Serial monitor: 115200 baud.

## Wiring

CS 5, SCK 18, MISO 19, MOSI 23.
Use a common ground and power the CC1101 from 3.3 V.

## Run

From the repository root:

```sh
pio run -d experiments/4 -e esp32dev
pio run -d experiments/4 -e esp32dev -t upload
pio device monitor -d experiments/4 -e esp32dev
```

For a different serial device, pass `--upload-port PORT` when uploading and
`--port PORT` when monitoring.

Prints `CC1101 VERSION register test`, followed by five register-read attempts.

## Dependencies and limitations

Uses Arduino SPI at 100 kHz. A ready timeout returns `0xFF`. This preserves the original register-read implementation; results still need hardware validation.
