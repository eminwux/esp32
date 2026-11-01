#include "frame.h"
#include "decoder.h"
#include "handshake.h"
#include "timing.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <vector>

using namespace ook;
static Frame ping = {kPing, 1, 2, 0, 0xffff, 0x12345678};
static void codec() {
    assert(crc16(reinterpret_cast<const uint8_t*>("123456789"), 9) == 0x29b1);
    uint8_t wire[kWireSize]; encode(ping, wire);
    const uint8_t golden[] = {
        0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xd3,0x91,
        0x0c,0x01,0x01,0x01,0x02,0x00,0x00,0xff,0xff,0x12,0x34,0x56,0x78,0xee,0xd9
    };
    static_assert(sizeof(golden) == kWireSize, "wire size");
    assert(memcmp(golden, wire, kWireSize) == 0);
    Frame f;
    assert(decode(wire + 10, kBodySize, 2, f) == Result::Good);
    assert(f.session == ping.session && f.seq == 0 && f.ack == 0xffff);
    assert(decode(wire + 10, kBodySize, 1, f) == Result::Invalid);
    for (size_t n = 0; n < kBodySize; ++n) assert(decode(wire + 10, n, 2, f) == Result::Length);
    for (unsigned bit = 0; bit < kBodySize * 8; ++bit) {
        uint8_t copy[kBodySize]; memcpy(copy, wire + 10, kBodySize);
        copy[bit / 8] ^= 0x80 >> (bit % 8);
        assert(decode(copy, kBodySize, 2, f) != Result::Good);
    }
    // Malformed fields with correct CRC must still be rejected.
    for (unsigned byte : {1U, 2U, 3U, 4U, 5U, 7U}) {
        uint8_t copy[kBodySize]; memcpy(copy, wire + 10, kBodySize);
        copy[byte] = 0x80;
        put16(copy + 13, crc16(copy, 13));
        assert(decode(copy, kBodySize, 2, f) == Result::Invalid);
    }
}
static std::vector<bool> wireBits() {
    uint8_t wire[kWireSize]; encode(ping, wire);
    std::vector<bool> bits;
    for (uint8_t byte : wire)
        for (unsigned b = 0; b < 8; ++b) bits.push_back((byte & (0x80 >> b)) != 0);
    return bits;
}
static unsigned feedBits(BitDecoder& d, const std::vector<bool>& bits) {
    unsigned good = 0; Frame f;
    for (bool b : bits) if (d.pushBit(b, 2, f) == Result::Good) ++good;
    return good;
}
static void framing() {
    BitDecoder d;
    auto bits = wireBits();
    assert(feedBits(d, bits) == 1);
    assert(feedBits(d, bits) == 1);
    auto broken = bits; broken.erase(broken.begin() + 120);
    assert(feedBits(d, broken) == 0);
    // First next-frame bit can complete the corrupted candidate; the remaining
    // preamble is still sufficient for software resynchronization.
    assert(feedBits(d, bits) == 1);
    broken = bits; broken[70] = !broken[70]; // Sync corruption.
    assert(feedBits(d, broken) == 0);
    assert(feedBits(d, bits) == 1);
    broken = bits; broken[80] = true; // Impossible length rejected immediately.
    assert(feedBits(d, broken) == 0);
    assert(feedBits(d, bits) == 1);
    uint32_t random = 7; unsigned accepted = 0; Frame f;
    for (unsigned i = 0; i < 100000; ++i) {
        random = random * 1664525U + 1013904223U;
        if (d.pushBit(random >> 31, 2, f) == Result::Good) ++accepted;
    }
    assert(accepted == 0);
}
static unsigned waveform(SampleDecoder& decoder, const std::vector<bool>& bits,
                         unsigned phase, double drift, bool jitter, bool glitches) {
    Frame f; unsigned good = 0;
    for (unsigned i = 0; i < 32 + phase; ++i) decoder.sample(false, 2, f);
    // Sample the same waveform with arbitrary start phase, bounded clock drift,
    // +/- two-sample edge jitter (1/8 bit), and isolated one-sample glitches.
    double period = 16.0 * (1.0 + drift);
    size_t bit = 0;
    for (unsigned tick = 0; tick < unsigned(bits.size() * period + 40); ++tick) {
        while (bit < bits.size() && tick >= (bit + 1) * period +
               (jitter ? int((bit + 1) % 3) * 2 - 2 : 0)) ++bit;
        bool level = bit < bits.size() ? bits[bit] : false;
        if (glitches && tick % 101 == 37) level = !level;
        Result r = decoder.sample(level, 2, f);
        if (r == Result::Good) { assert(f.session == ping.session); ++good; }
    }
    return good;
}
static void samples() {
    auto bits = wireBits();
    for (unsigned phase = 0; phase < 16; ++phase) {
        for (double drift : {-0.001, 0.0, 0.001}) {
            SampleDecoder d;
            assert(waveform(d, bits, phase, drift, false, false) == 1);
            assert(waveform(d, bits, phase, drift, true, true) == 1);
        }
    }
    SampleDecoder d; Frame f;
    auto bad = bits; bad.erase(bad.begin() + 115);
    assert(waveform(d, bad, 0, 0, false, false) == 0);
    for (unsigned i = 0; i < 5000; ++i) d.sample(false, 2, f);
    assert(waveform(d, bits, 3, 0, false, false) == 1);
    // Short candidate eventually unlocks even if the peer disconnects mid-frame.
    bits.resize(110);
    assert(waveform(d, bits, 1, 0, false, false) == 0);
    for (unsigned i = 0; i < 5000; ++i)
        d.sample(false, 2, f);
    assert(waveform(d, wireBits(), 0, 0, false, false) == 1);
    // Acquire on preamble but never deliver sync: the search must time out.
    d.reset();
    std::vector<bool> preamble(32);
    for (unsigned i = 0; i < preamble.size(); ++i) preamble[i] = i % 2;
    assert(waveform(d, preamble, 0, 0, false, false) == 0);
    bool timedOut = false;
    for (unsigned i = 0; i < 5000; ++i)
        if (d.sample(false, 2, f) == Result::Timeout) timedOut = true;
    assert(timedOut);
    assert(waveform(d, wireBits(), 0, 0, false, false) == 1);
}
static void sessions() {
    Handshake a(1), b(2);
    b.recover(55); assert(!b.pending()); // B never initiates.
    a.recover(0x12345678); Frame first = a.next(); a.sent();
    assert(b.receive(first)); assert(!b.receive(first));
    for (unsigned i = 0; i < 70000; ++i) {
        Handshake& sender = i % 2 == 0 ? b : a;
        Handshake& receiver = i % 2 == 0 ? a : b;
        assert(sender.pending()); Frame f = sender.next(); sender.sent();
        Frame stale = f; --stale.ack; assert(!receiver.receive(stale));
        stale = f; stale.session ^= 1; assert(!receiver.receive(stale));
        stale = f; ++stale.seq; assert(!receiver.receive(stale));
        assert(receiver.receive(f));
        assert(!receiver.receive(f)); // Duplicate before sending.
    }
    // Lost response: A retires its epoch; old reply cannot restart it.
    Frame old = a.next(); a.timeout(); assert(!a.receive(old));
    a.recover(123); first = a.next(); a.sent();
    assert(b.receive(first)); // New PING can replace a waiting old B session.
    Frame reply = b.next(); b.sent(); assert(a.receive(reply));
    a.sent(); assert(!a.receive(reply)); // Duplicate after sending.
    b.timeout(); assert(!b.receive(first)); assert(!b.pending());
    // B reboot: a continuation is rejected until A times out and sends PING.
    Handshake rebootB(2); assert(!rebootB.receive(a.next()));
    a.recover(456); first = a.next(); a.sent(); assert(rebootB.receive(first));
    // A reboot gets a distinct random epoch; stale PONG_PING is rejected.
    Handshake rebootA(1); rebootA.recover(789); rebootA.sent();
    assert(!rebootA.receive(rebootB.next()));
    a.recover(456); assert(a.next().session != 456); // Avoid immediate epoch reuse.
}
static void timing() {
    // The user's trace stops several seconds after A queues sequence 2.
    // A ~340 ms TX costs 6460 ms of silence at 5%; that pause is intentional.
    uint32_t silence = silenceMs(340, 50, 2000);
    assert(silence == 6460);
    const uint32_t end = 500, received = end + 488;
    assert(sendWaitMs(remainingMs(received, received, 150),
                      remainingMs(received, end, silence)) == 5972);
    assert(remainingMs(end + 4000, end, silence) == 2460);
    assert(remainingMs(end + silence - 1, end, silence) == 1);
    assert(remainingMs(end + silence, end, silence) == 0);
    assert(remainingMs(end + silence + 1, end, silence) == 0);
    assert(silenceMs(10, 50, 2000) == 2000);
    assert(sendWaitMs(150, 0) == 150);
    assert(remainingMs(0x20, 0xfffffff0, 100) == 52);
    assert(remainingMs(0x54, 0xfffffff0, 100) == 0);

    // Drive production handshake + timer gates through a minute of logical
    // time: the pending reply must eventually send, without bypassing silence.
    Handshake nodes[] = {Handshake(1), Handshake(2)};
    uint32_t ended[2] = {}, queued[2] = {}, sentAt[2] = {};
    bool sent[2] = {};
    unsigned exchanges = 0;
    nodes[0].recover(123);
    for (uint32_t now = 0; now < 60000; ++now) {
        for (unsigned i = 0; i < 2; ++i) {
            if (!nodes[i].pending()) continue;
            if (sendWaitMs(remainingMs(now, queued[i], 150),
                           sent[i] ? remainingMs(now, ended[i], silence) : 0)) continue;
            if (sent[i]) assert(now - ended[i] >= silence);
            Frame f = nodes[i].next(); nodes[i].sent();
            sentAt[i] = now; ended[i] = now + 340; sent[i] = true;
            // Advance past transmission; the peer can only queue after RX.
            now += 340;
            assert(nodes[1 - i].receive(f));
            queued[1 - i] = now;
            if (exchanges > 0) assert(now - ended[1 - i] < 10000);
            ++exchanges;
        }
    }
    assert(exchanges >= 10);
    assert(sentAt[0] > 50000 && sentAt[1] > 50000);
}
int main() {
    codec(); framing(); samples(); sessions(); timing();
    puts("PASS: protocol, sampled decoder, recovery, airtime countdown and scheduled exchange model");
}
