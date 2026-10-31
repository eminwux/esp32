# CC1101 register handshake

Two ESP32-WROOM-32 boards, each with one 26 MHz CC1101 module. No RF library:
Arduino SPI drives the hardware packet engine directly. Both radios are
half duplex. This is a bounded 60-second bench experiment; restart both boards
to repeat. Firmware/build tests do not establish RF performance.

## Wiring and commands

| CC1101 | ESP32 |
| --- | --- |
| GND | GND |
| VCC | 3.3 V only |
| CSn | GPIO 5 |
| SCLK | GPIO 18 |
| SI/MOSI | GPIO 23 |
| SO/MISO | GPIO 19 |
| GDO0 | GPIO 26 |
| GDO2 (optional, unused) | GPIO 27 |

Use a module and antenna suitable for 433 MHz, short SPI wires and local supply
decoupling. Connect an antenna before transmitting. From the repository root:

```sh
pio run -d experiments/handshake -e esp32_a
pio run -d experiments/handshake -e esp32_b
pio run -d experiments/handshake -e esp32_b -t upload
pio run -d experiments/handshake -e esp32_a -t upload
pio device monitor -d experiments/handshake -e esp32_a
pio device monitor -d experiments/handshake -e esp32_b
```

Start/reset B first, then A. Override upload ports with `--upload-port PORT`
and monitor ports with `--port PORT`. Each board's test window starts at boot.
No upload is performed by a normal build.

## Signal path and registers

SPI uses mode 0 at 1 MHz. CS assertion waits at most 3 ms for SO ready.
Config reads use header bit 7; burst transfers add bit 6. Read-only status
addresses 0x30..0x3d require **both** bits: PARTNUM uses 0xf0 and VERSION
0xf1. SRES uses 0x30 without read bits. PARTNUM must be zero and VERSION
must be neither zero nor 0xff; the ready event includes both as src/dst fields.
This catches absent wiring, but is not an exhaustive silicon revision whitelist.

`src/radio.h` writes the complete 0x00..0x2e configuration and PATABLE.
Important settings: IOCFG0=0x06 (sync high, packet-end low), SYNC=d391,
PKTLEN=8, PKTCTRL1=0x04 (append status), PKTCTRL0=0x05 (variable length,
CRC on), MDMCFG4/3=ca/83, MDMCFG2=03 (2-FSK, 30/32 sync qualification),
MDMCFG1=22 (four preamble bytes, no FEC), DEVIATN=35,
MCSM1=00 (RX/TX finish in IDLE, CCA disabled), MCSM0=08 (manual calibration),
FREND1/0=56/10, PATABLE[0]=12 (approximately -30 dBm at 433 MHz).
GDO0 is available for a logic analyzer; firmware polls MARCSTATE and FIFO
counts instead. MARCSTATE and FIFO counts require consecutive matching reads
(with a bounded retry count) for the TI SPI status synchronization erratum.
The RX state is sampled before RXBYTES so a packet finishing between SPI
transactions cannot pair IDLE with a stale partial FIFO count. No interrupts or software-generated RF waveform are used.

With a 26 MHz crystal the symbol rate is about 38.383 kbaud, deviation
20.63 kHz, RX filter bandwidth 101.56 kHz. These settings must match on both
boards; changes require recalculating the timeout and airtime limits.

Every TX and RX entry explicitly performs SIDLE, waits for IDLE, writes
FREQ2/1/0, strobes SCAL, waits for IDLE (5 ms bound), then flushes the relevant
FIFO while IDLE and enters STX or SRX. RX also waits for MARCSTATE=RX.
TX completion requires IDLE and an empty TX FIFO; underflow, overflow, SPI
failure and a 20 ms TX deadline trigger reset/reinitialization after one second.
A failed state transition never results in an unbounded wait.

Frequency word = round(hz * 65536 / 26000000) + RF_FREQ_OFFSET.
The offset is a signed **word increment**, about 396.73 Hz per step, not Hz.
It belongs to the physical module, regardless of which role it runs. The
supplied +71 on A shifts *both* of that board's frequencies about +28.17 kHz;
it is an example calibration, not a universal correction. Measure your modules
and adjust the per-board flags. Logs report effective calculated TX/RX Hz,
not measured crystal error. A transmits on F_A and receives on F_B; B does
the reverse. For a single channel, set both frequency flags equal in both
environments; serialization and turnaround remain identical.

## RF and application framing

```text
hardware: [4-byte preamble] [d391 d391 sync] [length=8] [payload] [CRC-16]
FIFO TX:                                   [08]       [8 bytes]
FIFO RX:                                   [08]       [8 bytes] [RSSI] [LQI|CRC_OK]
payload: [version=1:1] [type:1] [src:1] [dst:1] [sequence:2] [ack:2]
```

Multibyte values are big endian. Types: PING=01, PONG_PING=02; IDs A=01,
B=02. Only PING uses ack=ffff to mean none; ffff is a valid sequence/ack
inside a PONG_PING. The radio inserts preamble, repeated sync and CRC and
checks sync/CRC. Firmware supplies length and payload, requires exactly 11
RX FIFO bytes, length eight, CRC_OK, version/type/source/destination and the
expected sequence and ack before replying. Hardware status bytes are not
transmitted. CRC-invalid or malformed packets are flushed and RX rearmed.
Hardware sync qualification is 30 of 32 bits, not an exact software sync test.

## Scheduling and recovery

```text
A: TX F_A, RX F_B                     B: TX F_B, RX F_A
PING(seq=0, ack=ffff) ---------------> LISTEN
WAIT_REPLY <------------------------- PONG_PING(seq=1, ack=0)
PONG_PING(seq=2, ack=1) -------------> WAIT_REPLY
WAIT_REPLY <------------------------- PONG_PING(seq=3, ack=2)
                         ... until the test window ends
```

A uses INIT, SEND_PING, TX, WAIT_REPLY, SEND_PONG_PING and RETRY.
B starts INIT/LISTEN and never transmits without an accepted request.
Reception schedules a response, rather than transmitting inside the parser.
The sender switches immediately to RX after packet completion. Both sequence
and ack must progress modulo 65536. A repeated PONG_PING is counted and ignored;
A's next timeout repairs the exchange rather than retransmitting duplicates.
A valid PING is always accepted by B, even during a pending reply: it realigns
the session. Recovery PING bases advance by 257; this is a small educational
protocol, without session nonces or replay protection.

Build flags `REPLY_TIMEOUT_MS` (6000), `RETRY_INTERVAL_MS` (3000),
`TURNAROUND_DELAY_MS` (20) and `TEST_DURATION_MS` (60000, maximum 60000)
control scheduling. A timeout starts **after TX finishes and RX is armed**.
A then waits the retry interval before issuing a fresh PING. B times out into
LISTEN on F_A. Missing peers, rejected duplicates and either board restarting
therefore converge via A's next PING while both boards remain inside their
test windows. There is no `rx_delay_ms` with an overloaded meaning.

TX starts are separated by at least two seconds per board. The ~19-byte
38.383 kbaud frame takes about 4 ms; the 20 ms TX watchdog bounds faults.
Timeouts allow this spacing, retuning, calibration and RX settling. The hard
60-second window stops further transmissions even across radio resets.
This deliberately limits the otherwise indefinitely extensible ping-pong loop.
It does not certify a regulatory duty cycle: resets start a new window, and a
hardware fault cannot be made RF-safe solely by firmware.

Compact UART events include role/state, frequencies, type/seq/ack, CRC,
RSSI dBm estimate (raw signed value / 2 - 74), LQI, response elapsed ms,
latest tune/calibration latency in us, misses, retries, errors and duplicates.
One bounded log slot drops events when busy. UART writes use only reported
available buffer space, after servicing the radio; log congestion must not
hold up a reply. State values follow the enum in main.cpp; `dt` is elapsed
since the last TX, not a synchronized one-way latency measurement.

## Bench verification and RTL-SDR

Check your local frequency allocation, permitted power, occupied bandwidth,
sub-band and duty-cycle rules before enabling RF. The supplied frequencies
and low PA setting are lab examples, not authorization. Use short captures,
attenuation/shielding where appropriate, and avoid repeatedly resetting to
circumvent the bounded test window. Stop if interference appears.

1. Start with offsets measured for your two modules. Confirm ready identity
   events and no radio errors with a logic analyzer if necessary.
2. Tune an RTL-SDR midway between both carriers, using enough sample rate
   (e.g. 2 MS/s) to cover their separation. Adjust SDR gain to avoid overload.
3. Inspect alternating bursts around 433.920 and 433.500 MHz plus each module's
   offset. Zoom to measure FSK deviation and occupied width; the RX filter
   bandwidth is not the transmitted occupied bandwidth.
4. Correlate bursts with TX/RX event sequences. Measure actual turnaround and
   ensure the 20 ms minimum response delay covers retuning on your modules.
5. Set both frequencies equal and repeat; only one endpoint should transmit
   at a time. CCA is disabled; the protocol serializes normal traffic.
6. Disconnect or power-cycle a peer mid-exchange, then reconnect/reset within
   the other's window. Expect timeout/retry and a fresh PING. Inject a duplicate
   frame using a controlled packet source; expect no duplicate response.
7. Confirm no further RF bursts after the 60-second window. Test sequence
   wrap with a controlled packet source. Keep timestamped UART and SDR captures.

Host codec regression check (does not verify hardware or scheduling):

```sh
c++ -std=c++11 -Wall -Wextra -Werror experiments/handshake/test/protocol.cpp -o /tmp/handshake-protocol
/tmp/handshake-protocol
```

Status-read ordering regression (scripted SPI values, not an RF test):

```sh
c++ -std=c++11 -Wall -Wextra -Werror experiments/handshake/test/status.cpp -o /tmp/handshake-status
/tmp/handshake-status
```

Reject events distinguish `reject-size`, `reject-length`, `reject-crc` and
`reject-protocol`, and include `fifo=`. CRC=-1 means no status bytes were read;
RSSI/LQI are unavailable in that case. An earlier firmware version read RXBYTES
before MARCSTATE and could discard a completed packet using a partial count;
its size rejects misleadingly displayed CRC=0 and RSSI=-74 placeholders.
See [TI CC1101 silicon errata SWRZ020E](https://www.ti.com/lit/er/swrz020e/swrz020e.pdf)
for SPI read synchronization constraints.

Hardware SPI reliability, retuning, same-frequency operation, recovery and RF
compliance remain unverified until the above checks run on real boards.
Register definitions/formulas: [TI CC1101 datasheet, SWRS061I](https://www.ti.com/lit/ds/symlink/cc1101.pdf),
sections on SPI, packet handling, frequency programming and register tables.
