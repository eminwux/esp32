# Experiment 5: CC1101 OOK pulse generator

Board: `esp32dev`. Framework: Arduino. Serial monitor: 115200 baud.

## Wiring

CS 5, SCK 18, MISO 19, MOSI 23, GDO0 26.
Use a common ground and power the CC1101 from 3.3 V.

## Run

From the repository root:

```sh
pio run -d experiments/5 -e esp32_a
pio run -d experiments/5 -e esp32_a -t upload
pio device monitor -d experiments/5 -e esp32_a
```

For a different serial device, pass `--upload-port PORT` when uploading and
`--port PORT` when monitoring.

Prints PARTNUM, VERSION, frequency, calibration offset, and transmitter status. Toggles GDO0 high/low every 100 ms.

## Environments

| Environment | Port | RF frequency | Frequency-word offset |
| --- | --- | --- | --- |
| `esp32_a` | `/dev/ttyUSB0` | 433,920,000 Hz | 71 |
| `esp32_b` | `/dev/ttyUSB1` | 433,500,000 Hz | 0 |

Offsets are raw frequency-word increments, not ppm.

## Dependencies and limitations

Uses Arduino SPI with direct register access. The original register settings and initialization error handling are preserved. Serial success messages do not establish RF behavior; validate on hardware.
