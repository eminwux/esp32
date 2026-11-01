# Experiment am-roundtrip: CC1101 AM/OOK echo

Two ESP32 + CC1101 nodes exchange the same payload using on/off keying (OOK),
a binary form of amplitude modulation. A broadcasts a frame, B validates and
retransmits its 16 payload bytes unchanged, and A compares the echo byte for
byte. Both directions use the same nominal 433.920 MHz carrier.

This is a regenerated digital echo. It does not record or reproduce arbitrary
analog AM audio, RF phase, received amplitude, or noise. Use exactly one A and
one B; multiple responders are not coordinated. Only A initiates, so an echo
does not cause A to transmit another echo.

## Hardware

Board: `esp32dev`. Framework: Arduino. Power each CC1101 from 3.3 V and connect
it to its ESP32's ground. Use modules and antennas suitable for the carrier.

| CC1101 | ESP32 GPIO |
| --- | --- |
| CS | 5 |
| SCK | 18 |
| MISO | 19 |
| MOSI | 23 |
| GDO0 | 26 |
| GDO2 | 27 |

Both GDO connections are required: GDO0 signals received packet completion;
RadioLib's blocking transmission uses GDO2 to detect transmission completion.
These are the same connections as `experiments/pong`.

## Build, upload, and monitor

From the repository root, build both roles:

```sh
pio run -d experiments/am-roundtrip -e esp32_a -e esp32_b
```

Upload each role to its own board:

```sh
pio run -d experiments/am-roundtrip -e esp32_b -t upload
pio run -d experiments/am-roundtrip -e esp32_a -t upload
```

Open a serial monitor for each board in separate terminals:

```sh
pio device monitor -d experiments/am-roundtrip -e esp32_a
pio device monitor -d experiments/am-roundtrip -e esp32_b
```

Serial speed is 115200 baud. Override device paths with `--upload-port PORT`
when uploading and `--port PORT` when monitoring. Start B first for the first
A transmission to have a listener.

| Environment | Default port | Role | Crystal correction |
| --- | --- | --- | --- |
| `esp32_a` | `/dev/ttyUSB0` | Initiator | +65 ppm |
| `esp32_b` | `/dev/ttyUSB1` | Echo responder | 0 ppm |

Correction values are inherited from PONG and have not been measured for this
experiment. Each node applies its correction to both TX and RX. They are ppm,
not experiment 5's raw frequency-word offsets. Recalibrate for your modules.

## Radio and payload

The local dependency is pinned to `jgromes/RadioLib@7.1.2`. Initialization
explicitly enables OOK, disables address filtering for broadcast reception,
and configures a 4.8 kbps rate, 135 kHz receive bandwidth, -30 dBm TX power,
32-bit preamble, exact `A5 5A` sync word, fixed 16-byte payload, and hardware
CRC. The frequency-deviation argument passed to `begin()` is only used during
initial FSK setup; it does not control OOK modulation.

| Payload offsets | Contents |
| --- | --- |
| 0–3 | ASCII `AME1`, experiment identifier and format version |
| 4–7 | Random per-boot session identifier, big endian |
| 8–11 | Incrementing 32-bit sequence, big endian |
| 12–15 | Test pattern `AA 55 00 FF` |

Preamble, sync, and CRC are radio framing outside the payload. The zero byte
in the test pattern is preserved because transmission uses binary buffers,
not strings. B checks CRC, identifier, and pattern before replying. A also
requires an exact match of all 16 bytes, including session and sequence.

## Timing and recovery

1. A sends one frame and immediately enters RX, before further serial output.
2. B finishes receiving, then waits at least `REPLY_GUARD_MS` (default 50 ms).
3. B transmits the same bytes once and returns to listening immediately.
4. A accepts a matching echo within `RX_TIMEOUT_MS` (default 500 ms after TX
   completion), then waits 1000 ms before its next exchange. A timeout also
   starts that interval. Round-trip time is measured from the start of A's TX.

The reply takes approximately 40 ms on air. A compile-time check requires
the receive window to exceed the guard, reply airtime, and a 20 ms settling
margin. `RF_FREQ_HZ`, `RF_CORRECTION_PPM`, `REPLY_GUARD_MS`, and
`RX_TIMEOUT_MS` can be overridden with build flags. Change timing and carrier
consistently on both nodes.

CRC failures and unrelated or stale frames restart RX without extending A's
original deadline. B refreshes RX every 2000 ms to clear incomplete receptions
or FIFO problems; this can interrupt a frame that straddles the refresh.
Runtime TX/RX-start errors enter standby and retry initialization after
1000 ms. Initialization failures retry at the same interval. Sequences advance
for every attempt; there are no automatic retransmissions of a failed frame.

## Expected serial output

A prints `TX`, `ECHO RX`, and `ROUNDTRIP SUCCESS` with RSSI and round-trip time.
`STATS` reports attempts, successes, timeouts, TX errors, RX errors, rejected
frames, and successes as a percentage of all attempts. A silent B produces
`ECHO timeout` and later attempts continue. Read errors include the RadioLib
error code. B prints `RX` and `ECHO TX`; their hex bytes must match exactly.

## Validation

Both `esp32_a` and `esp32_b` firmware builds passed with RadioLib 7.1.2,
Espressif32 platform 8.0.0, and Arduino-ESP32 3.3.12. Both host test roles
passed with `-Wall -Wextra -Werror`.

Host tests execute the production `setup()` and `loop()` with a fake radio and
clock. They cover binary payloads, CRC and invalid-frame rejection, stale
sessions/sequences, the fixed deadline, guard timing, exact echo, absent B,
millisecond rollover, initialization retries, and recovery after TX/RX errors.
They do not simulate the RF hardware or prove reception quality.

```sh
g++ -std=c++11 -Wall -Wextra -Werror -I experiments/am-roundtrip/test/host -D NODE_ROLE_A=1 experiments/am-roundtrip/test/roundtrip_test.cpp -o /tmp/am-roundtrip-test-a
/tmp/am-roundtrip-test-a
g++ -std=c++11 -Wall -Wextra -Werror -I experiments/am-roundtrip/test/host -D NODE_ROLE_A=0 experiments/am-roundtrip/test/roundtrip_test.cpp -o /tmp/am-roundtrip-test-b
/tmp/am-roundtrip-test-b
```

Hardware acceptance procedure:

1. Confirm both boards print `OOK ready` and verify one-way A reception at B.
2. Confirm B's `ECHO TX` matches its `RX`, and A reports matching echoes.
3. Run 100 attempts and record successes, timeouts, errors, RSSI, and RTT.
4. Disconnect B: A must time out and continue. Reconnect B: exchanges must
   recover without resetting A.
5. Use an SDR or suitable RF instrument to verify OOK carrier switching and
   matching bit patterns in both directions. Serial logs alone do not establish
   the modulation or actual radiated carrier frequency.
6. If reception is poor, measure each board's frequency error and evaluate
   ASK/OOK AGC settings using TI's guidance. This initial implementation uses
   RadioLib's receiver defaults; it has no measured ASK/OOK tuning profile.

No firmware has been uploaded and no RF/hardware validation has been performed.

References: [TI CC1101 datasheet, especially sections 16.3 and 17](https://www.ti.com/lit/ds/symlink/cc1101.pdf),
[RadioLib 7.1.2 CC1101 driver](https://github.com/jgromes/RadioLib/blob/7.1.2/src/modules/CC1101/CC1101.cpp).
