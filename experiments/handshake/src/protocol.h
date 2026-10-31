#pragma once
#include <stdint.h>
namespace wire {
constexpr uint8_t Ping = 1, PongPing = 2;
struct Frame { uint8_t type, src, dst; uint16_t seq, ack; };
inline uint16_t next(uint16_t n) { return uint16_t(n + 1); }
inline void encode(const Frame &f, uint8_t *p) {
  p[0]=1; p[1]=f.type; p[2]=f.src; p[3]=f.dst;
  p[4]=f.seq>>8; p[5]=f.seq; p[6]=f.ack>>8; p[7]=f.ack;
}
inline bool decode(const uint8_t *p, Frame &f, uint8_t self) {
  if (p[0]!=1 || p[2]!=(self==1?2:1) || p[3]!=self ||
      (p[1]!=Ping && p[1]!=PongPing)) return false;
  f={p[1],p[2],p[3],uint16_t(p[4]*256+p[5]),uint16_t(p[6]*256+p[7])};
  return f.type!=Ping || (self==2 && f.ack==0xffff);
}
inline bool reply(const Frame &f, uint16_t sent) {
  return f.type==PongPing && f.ack==sent && f.seq==next(sent);
}
}
