#include "radio.h"
#include "protocol.h"
#if defined(NODE_ROLE_A) == defined(NODE_ROLE_B)
#error Select exactly one node role
#endif
#ifndef REPLY_TIMEOUT_MS
#define REPLY_TIMEOUT_MS 6000
#endif
#ifndef RETRY_INTERVAL_MS
#define RETRY_INTERVAL_MS 3000
#endif
#ifndef TURNAROUND_DELAY_MS
#define TURNAROUND_DELAY_MS 20
#endif
#ifndef TEST_DURATION_MS
#define TEST_DURATION_MS 60000
#endif
static_assert(REPLY_TIMEOUT_MS >= 5000, "Allow peer TX spacing and turnaround");
static_assert(TEST_DURATION_MS > 0 && TEST_DURATION_MS <= 60000, "Bounded bench test: 1..60000 ms");
static_assert(TURNAROUND_DELAY_MS < REPLY_TIMEOUT_MS-3000, "Turnaround must fit reply window");
#ifdef NODE_ROLE_A
constexpr uint8_t self=1;
constexpr uint32_t txHz=RF_FREQ_A_HZ, rxHz=RF_FREQ_B_HZ;
#else
constexpr uint8_t self=2;
constexpr uint32_t txHz=RF_FREQ_B_HZ, rxHz=RF_FREQ_A_HZ;
#endif
uint32_t word(uint32_t hz) { return uint32_t((uint64_t(hz)*65536+13000000)/26000000)+RF_FREQ_OFFSET; }
uint32_t effective(uint32_t hz) { return uint64_t(word(hz))*26000000/65536; }
Radio radio;
enum State { INIT, LISTEN, SEND_PING, SEND_PONG_PING, TRANSMITTING, WAIT_REPLY, RETRY, STOP };
State state=INIT;
uint32_t boot, due, sentAt, lastTx, errors=0, missed=0, retries=0, duplicates=0;
uint32_t turnUs=0;
uint16_t sentSeq=0, lastRx=0, session=0;
bool haveRx=false, awaiting=false;
wire::Frame outgoing{};
char logBuffer[256]; size_t logLength=0, logPosition=0;
// Single bounded log slot: drop rather than block when the UART is busy.
void event(const char *name, const wire::Frame &f={}, int crc=-1, int rssi=0,int lqi=0) {
  if(logPosition<logLength) return;
  int n=snprintf(logBuffer,sizeof(logBuffer),
    "%lu %c s=%u %s tx=%lu rx=%lu t=%u seq=%u ack=%u crc=%d rssi=%d lqi=%d dt=%lu turn_us=%lu miss=%lu retry=%lu err=%lu dup=%lu\n",
    millis(),self==1?'A':'B',unsigned(state),name,effective(txHz),effective(rxHz),f.type,f.seq,f.ack,
    crc,rssi,lqi,millis()-sentAt,turnUs,missed,retries,errors,duplicates);
  logLength=n<0?0:size_t(n)<sizeof(logBuffer)?size_t(n):sizeof(logBuffer)-1; logPosition=0;
}
void pumpLog() {
  size_t room=Serial.availableForWrite(), left=logLength-logPosition;
  size_t count=room<left?room:left;
  if(count) logPosition+=Serial.write(reinterpret_cast<const uint8_t *>(logBuffer)+logPosition,count);
}
bool reached(uint32_t now,uint32_t deadline) { return int32_t(now-deadline)>=0; }
void fail() { ++errors; state=INIT; due=millis()+1000; awaiting=false; haveRx=false; event("radio-error"); }
bool receiveMode() {
  uint32_t start=micros(); bool ok=radio.rx(word(rxHz)); turnUs=micros()-start; return ok;
}
void schedule(const wire::Frame &f) {
  outgoing={wire::PongPing,self,uint8_t(self==1?2:1),wire::next(f.seq),f.seq};
  state=SEND_PONG_PING; due=millis()+TURNAROUND_DELAY_MS;
}
void setup() { Serial.begin(115200); boot=millis(); due=boot; }
void loop() {
  uint32_t now=millis();
  if(state==STOP) { pumpLog(); return; }
  if(uint32_t(now-boot)>=TEST_DURATION_MS) {
    bool ok=radio.idle(); state=STOP; if(!ok) ++errors; event("test-ended"); pumpLog(); return;
  }
  if(state==INIT && reached(now,due)) {
    uint8_t part=255,version=255;
    if(!radio.begin(part,version)||!receiveMode()) { fail(); return; }
    state=self==1?SEND_PING:LISTEN; due=millis()+100;
    event("ready",{0,part,version,0,0});
  }
  if(state==RETRY && reached(now,due)) { state=SEND_PING; ++retries; }
  if((state==SEND_PING||state==SEND_PONG_PING) && reached(now,due) &&
      (lastTx==0 || uint32_t(now-lastTx)>=2000)) {
    if(state==SEND_PING) {
      // Each recovery PING chooses a fresh sequence base; B accepts it in any RX state.
      outgoing={wire::Ping,self,2,session,0xffff}; session=uint16_t(session+257);
      haveRx=false;
    }
    uint8_t payload[8]; wire::encode(outgoing,payload);
    uint32_t start=micros();
    if(!radio.tx(word(txHz),payload)) { fail(); return; }
    turnUs=micros()-start; lastTx=millis(); sentAt=lastTx; sentSeq=outgoing.seq;
    state=TRANSMITTING; awaiting=true;
  }
  if(state==TRANSMITTING) {
    uint8_t marc,bytes;
    if(!radio.status(0x35,marc)||!radio.status(0x3a,bytes)) { fail(); return; }
    if((bytes&0x80)||(marc&31)==22) { fail(); return; }
    if((marc&31)==1 && (bytes&0x7f)==0) {
      if(!receiveMode()) { fail(); return; }
      state=WAIT_REPLY; due=millis()+REPLY_TIMEOUT_MS; event("tx",outgoing);
    } else if(uint32_t(millis()-sentAt)>=20) { fail(); return; }
  }
  if(state==LISTEN||state==WAIT_REPLY||state==SEND_PONG_PING) {
    uint8_t bytes,marc;
    if(!radio.status(0x3b,bytes)||!radio.status(0x35,marc)) { fail(); return; }
    if((bytes&0x80)||(marc&31)==17) { fail(); return; }
    // RXOFF_MODE=IDLE: inspect FIFO only after complete packet, never at sync rise.
    if((marc&31)==1 && (bytes&0x7f)) {
      uint8_t data[11]={}; wire::Frame frame{};
      bool valid=(bytes&0x7f)==11;
      if(valid && !radio.burst(0x3f,data,11,true)) { fail(); return; }
      valid=valid && data[0]==8 && (data[10]&0x80) && wire::decode(data+1,frame,self);
      bool fresh=valid && self==2 && frame.type==wire::Ping;
      bool duplicate=valid && haveRx && frame.seq==lastRx && !fresh;
      bool accepted=valid && (fresh || (awaiting && wire::reply(frame,sentSeq)));
      if(duplicate) { ++duplicates; accepted=false; }
      // Flush malformed/partial packets and rearm even after a rejected frame.
      if(!receiveMode()) { fail(); return; }
      if(accepted) {
        haveRx=true; lastRx=frame.seq; awaiting=false; schedule(frame);
      }
      int rssi=int(int8_t(data[9]))/2-74;
      event(accepted?"rx":"reject",frame,(data[10]&0x80)?1:0,rssi,data[10]&127);
    } else if((marc&31)==1 && !(bytes&0x7f)) {
      if(!receiveMode()) { fail(); return; }
    }
  }
  now=millis();
  if(state==WAIT_REPLY && reached(now,due)) {
    ++missed; awaiting=false; haveRx=false;
    if(!receiveMode()) { fail(); return; }
    state=self==1?RETRY:LISTEN; due=millis()+RETRY_INTERVAL_MS; event("timeout");
  }
  pumpLog();
}
