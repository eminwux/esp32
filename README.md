# ESP32 experiments

Independent ESP32 / Arduino projects. Each experiment owns its `platformio.ini`,
source, dependencies, and build environments. Existing suffixes are permanent
experiment IDs; `base` identifies the formerly unsuffixed example.

| ID | Purpose | Environments | Dependencies |
| --- | --- | --- | --- |
| [base](experiments/base/) | CC1101 OOK byte-sequence transmitter | `esp32dev` | Arduino SPI |
| [3](experiments/3/) | LED blink and serial output | `esp32dev` | Arduino |
| [4](experiments/4/) | CC1101 SPI VERSION register check | `esp32dev` | Arduino SPI |
| [5](experiments/5/) | CC1101 OOK pulse generator with calibration | `esp32_a`, `esp32_b` | Arduino SPI |
| [handshake](experiments/handshake/) | Direct-register CC1101 dual-frequency handshake | `esp32_a`, `esp32_b` | Arduino SPI |
| [pong](experiments/pong/) | Two-node CC1101 ping/pong | `esp32_a`, `esp32_b` | RadioLib |

## Build, upload, and monitor

Install PlatformIO Core, or use the PlatformIO extension in VS Code. Open an
individual experiment folder in VS Code to use it as the PlatformIO project.
From this repository root, select the project explicitly:

```sh
pio run -d experiments/5 -e esp32_a
pio run -d experiments/5 -e esp32_a -t upload
pio device monitor -d experiments/5 -e esp32_a
```

Experiments `5` and `pong` retain the original `/dev/ttyUSB0` (A) and
`/dev/ttyUSB1` (B) ports. For a different machine, override the upload port with
`--upload-port PORT` and the monitor port with `--port PORT`.
There is no root PlatformIO project: choose the experiment to build.
Dependencies and build products are local to each project's ignored `.pio/`.

## Add an experiment

Copy the template into an unused stable ID:

```sh
cp -R templates/experiment experiments/6
pio run -d experiments/6 -e esp32dev
```

Update its README, code, and configuration, then add it to the catalog above.
Declare libraries in that experiment's `lib_deps`; add local `include/`, `lib/`,
or `test/` folders when needed. Keep node roles and calibration flags in its
own environments. Do not reuse an existing ID for an unrelated experiment.
Keep code local until a shared library is deliberately introduced.

## Original files

The migration preserves firmware and the existing experiment configs byte for
byte. [The archive](archive/original-layout/) retains the duplicate active
source/config and the original PlatformIO placeholder READMEs.
Its [manifest](archive/original-layout/manifest.json) maps every moved file and
records its original SHA-256. The archive is historical, not a runnable project;
use `experiments/5` for the former active project.

`kuketeam.yaml.bkp` remains at the root as existing team configuration backup.

## Migration validation

- All 12 relocated files match their recorded original SHA-256 hashes.
- Builds passed for `base`, `3`, `4`, both environments of `5`, and the template.
- `pong` build validation could not complete because its RadioLib dependency
  download did not finish in this environment. Its source/config remain unchanged.
- No firmware was uploaded and no hardware behavior was verified.
