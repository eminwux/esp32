# Experiment base: CC1101 OOK byte-sequence transmitter

Board: `esp32dev`. Framework: Arduino. Serial monitor: 115200 baud.

## Wiring

CS 5, SCK 18, MISO 19, MOSI 23.
Use a common ground and power the CC1101 from 3.3 V.

## Run

From the repository root:

```sh
pio run -d experiments/base -e esp32dev
pio run -d experiments/base -e esp32dev -t upload
pio device monitor -d experiments/base -e esp32dev
```

For a different serial device, pass `--upload-port PORT` when uploading and
`--port PORT` when monitoring.

Prints `CC1101 OOK byte sequence transmitter ready`, then sends the fixed byte sequence every 250 ms.

## Dependencies and limitations

Uses Arduino SPI with direct register access. The original SPI-ready waits have no timeout; absent hardware can block setup. No firmware changes were made.
