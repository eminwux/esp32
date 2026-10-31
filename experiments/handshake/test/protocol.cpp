#include "../src/protocol.h"
#include <assert.h>
int main() {
  uint8_t bytes[8]; wire::Frame decoded{};
  wire::encode({wire::Ping,1,2,0,0xffff},bytes);
  assert(wire::decode(bytes,decoded,2)); assert(!wire::decode(bytes,decoded,1));
  bytes[0]=2; assert(!wire::decode(bytes,decoded,2)); bytes[0]=1;
  bytes[7]=0; assert(!wire::decode(bytes,decoded,2));
  wire::encode({wire::PongPing,2,1,0,0xffff},bytes);
  assert(wire::decode(bytes,decoded,1)); assert(wire::reply(decoded,0xffff));
  assert(!wire::reply(decoded,0));
  bytes[1]=3; assert(!wire::decode(bytes,decoded,1));
  bytes[1]=2; bytes[3]=2; assert(!wire::decode(bytes,decoded,1));
  assert(wire::next(0xffff)==0);
}
