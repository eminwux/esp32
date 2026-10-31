#pragma once
#include <Arduino.h>
#include <SPI.h>
#include "status.h"
// CC1101 SWRS061I: config reads use 0x80; status reads MUST use 0xc0.
class Radio {
  static constexpr int CS=5, SO=19;
  bool select() {
    SPI.beginTransaction(SPISettings(1000000, MSBFIRST, SPI_MODE0));
    digitalWrite(CS,LOW); uint32_t start=micros();
    while (digitalRead(SO)) if (uint32_t(micros()-start)>3000) { end(); return false; }
    return true;
  }
  void end() { digitalWrite(CS,HIGH); SPI.endTransaction(); }
public:
  bool transfer(uint8_t header, uint8_t *data, size_t n, bool read) {
    if (!select()) return false;
    SPI.transfer(header);
    for(size_t i=0;i<n;++i) { uint8_t v=SPI.transfer(read?0:data[i]); if(read) data[i]=v; }
    end(); return true;
  }
  bool strobe(uint8_t s) { return transfer(s,nullptr,0,false); }
  bool write(uint8_t a,uint8_t v) { return transfer(a,&v,1,false); }
  bool read(uint8_t a,uint8_t &v) { return transfer(a|0x80,&v,1,true); }
  bool status(uint8_t a,uint8_t &v) { return transfer(a|0xc0,&v,1,true); }
  bool burst(uint8_t a,uint8_t *p,size_t n,bool reading) {
    return transfer(a|0x40|(reading?0x80:0),p,n,reading);
  }
  bool wait(uint8_t wanted) {
    uint32_t start=micros(); uint8_t s;
    do { if(!stableStatus(*this,0x35,s)) return false; if((s&31)==wanted) return true; }
    while(uint32_t(micros()-start)<5000);
    return false;
  }
  bool idle() { return strobe(0x36)&&wait(1); }
  bool tune(uint32_t word) {
    uint8_t f[]={uint8_t(word>>16),uint8_t(word>>8),uint8_t(word)};
    return idle()&&burst(0x0d,f,3,false)&&strobe(0x33)&&wait(1);
  }
  bool rx(uint32_t word) {
    return tune(word)&&strobe(0x3a)&&strobe(0x34)&&wait(13);
  }
  bool tx(uint32_t word,uint8_t *payload) {
    uint8_t data[9]={8}; memcpy(data+1,payload,8);
    return tune(word)&&strobe(0x3b)&&burst(0x3f,data,9,false)&&strobe(0x35);
  }
  bool begin(uint8_t &part,uint8_t &version) {
    pinMode(CS,OUTPUT); digitalWrite(CS,HIGH); pinMode(SO,INPUT);
    pinMode(26,INPUT); SPI.begin(18,19,23,CS);
    digitalWrite(CS,LOW); delayMicroseconds(10); digitalWrite(CS,HIGH);
    delayMicroseconds(50);
    if(!strobe(0x30)||!wait(1)||!status(0x30,part)||!status(0x31,version)) return false;
    if(part!=0 || version==0 || version==255) return false;
    // Explicit 26 MHz, 2-FSK, 38.383 kbaud, 20.63 kHz deviation,
    // 101.56 kHz RX BW, 4-byte preamble, 30/32 sync, no whitening/FEC.
    const uint8_t cfg[]={
      0x2e,0x2e,0x06,0x47,0xd3,0x91,0x08,0x04,0x05,0x00,0x00,0x06,0x00,
      0x10,0xb0,0x71,0xca,0x83,0x03,0x22,0xf8,0x35,0x07,0x00,0x08,0x16,
      0x6c,0x43,0x40,0x91,0x87,0x6b,0xf8,0x56,0x10,0xe9,0x2a,0x00,0x1f,
      0x41,0x00,0x59,0x7f,0x3f,0x81,0x35,0x09};
    uint8_t copy[sizeof(cfg)]; memcpy(copy,cfg,sizeof(cfg));
    uint8_t pa=0x12; // approximately -30 dBm at 433 MHz; FREND0.PA_POWER=0
    return burst(0,copy,sizeof(copy),false)&&write(0x3e,pa);
  }
};
