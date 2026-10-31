# Experiment pong: CC1101 two-node ping/pong

Board: `esp32dev`. Framework: Arduino. Serial monitor: 115200 baud.

## Wiring

CS 5, SCK 18, MISO 19, MOSI 23, GDO0 26, GDO2 27.
Use a common ground and power the CC1101 from 3.3 V.

## Run

From the repository root:

```sh
pio run -d experiments/pong -e esp32_a
pio run -d experiments/pong -e esp32_a -t upload
pio device monitor -d experiments/pong -e esp32_a
```

For a different serial device, pass `--upload-port PORT` when uploading and
`--port PORT` when monitoring.

Node A sends `PING:<sequence>`; node B replies `PONG:<sequence>`. Serial output reports tuning, packets, RSSI, and errors.

## Environments

| Environment | Port | Role | Crystal correction |
| --- | --- | --- | --- |
| `esp32_a` | `/dev/ttyUSB0` | Initiator | 65 ppm |
| `esp32_b` | `/dev/ttyUSB1` | Responder | 0 ppm |

Build and upload both roles to separate boards. A sends at 433.920 MHz and B
replies at 433.500 MHz, with per-node corrections applied to both frequencies.

## Dependencies and limitations

Uses `jgromes/RadioLib`, declared in this project only. The original dependency is unpinned. Node B delays 500 ms before replying while A has a 300 ms receive timeout; successful exchanges need separate investigation. Firmware is preserved unchanged.
