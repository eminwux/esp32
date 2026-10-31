#include "../src/status.h"
#include <assert.h>
#include <stddef.h>
struct Read { uint8_t address, value; };
struct Bus {
  const Read *reads; size_t length, position=0;
  Bus(const Read *r,size_t n):reads(r),length(n) {}
  bool status(uint8_t a,uint8_t &v) {
    assert(position<length); assert(reads[position].address==a);
    v=reads[position++].value; return true;
  }
};
int main() {
  // Packet finishes during polling. First stabilize MARCSTATE, then read
  // the completed FIFO; reading RXBYTES before IDLE fails this script.
  const Read transition[]={{0x35,13},{0x35,1},{0x35,1},{0x3b,10},{0x3b,11},{0x3b,11}};
  Bus bus(transition,6); uint8_t state,bytes;
  assert(rxSnapshot(bus,state,bytes)); assert(state==1 && bytes==11);
  assert(bus.position==6);
  const Read unstable[]={{0x35,1},{0x35,2},{0x35,1},{0x35,2},{0x35,1},
    {0x35,2},{0x35,1},{0x35,2},{0x35,1}};
  Bus changing(unstable,9); assert(!stableStatus(changing,0x35,state));
  assert(changing.position==9);
}
