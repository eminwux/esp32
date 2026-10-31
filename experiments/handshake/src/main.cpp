#include "radio.h"
#include "protocol.h"
#include "status.h"
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
char logBuffer[1024]; size_t logLength=0, logPosition=0;
// One bounded slot holds the original compact line plus its human explanation.
// Drop the pair rather than block when the UART is busy.
void event(const char *name, const wire::Frame &f={}, int crc=-1, int rssi=0,int lqi=0,int fifo=-1) {
  if(logPosition<logLength) return;
  int n=snprintf(logBuffer,sizeof(logBuffer),
    "%lu %c s=%u %s tx=%lu rx=%lu t=%u seq=%u ack=%u crc=%d rssi=%d lqi=%d dt=%lu turn_us=%lu miss=%lu retry=%lu err=%lu dup=%lu fifo=%d\n",
    millis(),self==1?'A':'B',unsigned(state),name,effective(txHz),effective(rxHz),f.type,f.seq,f.ack,
    crc,rssi,lqi,millis()-sentAt,turnUs,missed,retries,errors,duplicates,fifo);
  logLength=n<0?0:size_t(n)<sizeof(logBuffer)?size_t(n):sizeof(logBuffer)-1;
  char explanation[640];
  const char *message=f.type==wire::Ping?"PING":"PONG_PING";
  char peer=self==1?'B':'A';
  if (!strcmp(name,"ready")) {
    snprintf(explanation,sizeof(explanation),
      "Radio ready (PARTNUM=0x%02x VERSION=0x%02x). TX=%lu Hz, RX=%lu Hz. %s",
      f.src,f.dst,effective(txHz),effective(rxHz),self==1?
      "Initiator: preparing the first PING after 100 ms.":
      "Responder: listening for A; will not transmit until a valid request arrives.");
  } else if (!strcmp(name,"tx")) {
    snprintf(explanation,sizeof(explanation),
      "Sent %s to %c: sequence=%u, ack=%u%s. Now listening on %lu Hz for sequence=%u, ack=%u (timeout %u ms).",
      message,peer,f.seq,f.ack,f.type==wire::Ping?" (none)":"",
      effective(rxHz),wire::next(f.seq),f.seq,unsigned(REPLY_TIMEOUT_MS));
  } else if (!strcmp(name,"rx")) {
    uint32_t now=millis();
    uint32_t spacing=lastTx && uint32_t(now-lastTx)<2000?2000-uint32_t(now-lastTx):0;
    uint32_t delayMs=spacing>TURNAROUND_DELAY_MS?spacing:TURNAROUND_DELAY_MS;
    snprintf(explanation,sizeof(explanation),
      "Accepted %s from %c: sequence=%u, ack=%u; CRC OK, RSSI=%d dBm, LQI=%d. Preparing PONG_PING sequence=%u, ack=%u in at least %lu ms (turnaround and TX spacing).",
      message,peer,f.seq,f.ack,rssi,lqi,wire::next(f.seq),f.seq,delayMs);
  } else if (!strcmp(name,"timeout")) {
    snprintf(explanation,sizeof(explanation),
      "No valid reply within %u ms; missed replies=%lu. %s Retry interval=%u ms; retry count=%lu.",
      unsigned(REPLY_TIMEOUT_MS),missed,self==1?
      "Waiting the retry interval before a fresh PING to realign B.":
      "Returning to LISTEN on A's frequency; waiting for a new request.",unsigned(RETRY_INTERVAL_MS),retries);
  } else if (!strcmp(name,"radio-error")) {
    snprintf(explanation,sizeof(explanation),
      "Radio operation failed (SPI/state/FIFO/TX deadline); errors=%lu. Reinitializing in 1000 ms.",errors);
  } else if (!strcmp(name,"test-ended")) {
    snprintf(explanation,sizeof(explanation),
      "The %u ms bench window has ended. No more transmissions will be scheduled; reset both boards to repeat. Missed=%lu, retries=%lu, radio errors=%lu, duplicates=%lu.",
      unsigned(TEST_DURATION_MS),missed,retries,errors,duplicates);
  } else {
    const char *reason=!strcmp(name,"reject-size")?"unexpected FIFO byte count (expected 11); CRC/RSSI unavailable":
      !strcmp(name,"reject-length")?"payload length is not 8":
      !strcmp(name,"reject-crc")?"hardware CRC check failed":
      "version/type/address or sequence/ack mismatch, or duplicate packet";
    snprintf(explanation,sizeof(explanation),
      "Packet rejected: %s. FIFO bytes=%d; expected reply sequence=%u, ack=%u. RX rearmed; no reply scheduled for this packet.",
      reason,fifo,wire::next(sentSeq),sentSeq);
  }
  n=snprintf(logBuffer+logLength,sizeof(logBuffer)-logLength,
    "[%lu ms] Node %c: %s\n",millis(),self==1?'A':'B',explanation);
  if(n>0) {
    size_t room=sizeof(logBuffer)-logLength;
    logLength+=size_t(n)<room?size_t(n):room-1;
  }
  logPosition=0;
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
    if(!stableStatus(radio,0x35,marc)||!stableStatus(radio,0x3a,bytes)) { fail(); return; }
    if((bytes&0x80)||(marc&31)==22) { fail(); return; }
    if((marc&31)==1 && (bytes&0x7f)==0) {
      if(!receiveMode()) { fail(); return; }
      state=WAIT_REPLY; due=millis()+REPLY_TIMEOUT_MS; event("tx",outgoing);
    } else if(uint32_t(millis()-sentAt)>=20) { fail(); return; }
  }
  if(state==LISTEN||state==WAIT_REPLY||state==SEND_PONG_PING) {
    uint8_t bytes,marc;
    if(!rxSnapshot(radio,marc,bytes)) { fail(); return; }
    if((bytes&0x80)||(marc&31)==17) { fail(); return; }
    // RXOFF_MODE=IDLE: inspect FIFO only after complete packet, never at sync rise.
    if((marc&31)==1 && (bytes&0x7f)) {
      uint8_t data[11]={}; wire::Frame frame{};
      bool sizeOk=(bytes&0x7f)==11;
      bool valid=sizeOk;
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
      event(accepted?"rx":!sizeOk?"reject-size":data[0]!=8?"reject-length":
        !(data[10]&0x80)?"reject-crc":"reject-protocol",frame,
        sizeOk?((data[10]&0x80)?1:0):-1,rssi,data[10]&127,bytes&0x7f);
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
