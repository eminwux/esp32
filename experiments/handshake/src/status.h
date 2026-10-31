#pragma once
#include <stdint.h>
// TI SWRZ020E: continuously updated status registers need matching reads.
// Bound retries so a disconnected/changing device cannot wedge the loop.
template<class Bus>
bool stableStatus(Bus &bus, uint8_t address, uint8_t &value) {
  uint8_t previous;
  if (!bus.status(address, previous)) return false;
  for (unsigned i=0; i<8; ++i) {
    if (!bus.status(address, value)) return false;
    if (value==previous) return true;
    previous=value;
  }
  return false;
}
// With RXOFF_MODE=IDLE, counts sampled AFTER IDLE cannot be partial
// counts from the preceding reception. Never reuse a count sampled in RX.
template<class Bus>
bool rxSnapshot(Bus &bus, uint8_t &state, uint8_t &bytes) {
  bytes=0;
  return stableStatus(bus,0x35,state) && stableStatus(bus,0x3b,bytes);
}
