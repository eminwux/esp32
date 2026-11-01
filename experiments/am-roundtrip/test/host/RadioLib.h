#pragma once

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <vector>

#define RADIOLIB_NC 0xFFFFFFFF
#define RADIOLIB_ERR_NONE 0
#define RADIOLIB_ERR_CRC_MISMATCH -7

struct Module {
    Module(int, int, uint32_t, int) {}
};

// Model the radio boundary only; tests execute the production setup()/loop().
struct CC1101 {
    CC1101(Module*) {}
    bool initialized = false;
    bool listening = false;
    bool ook = false;
    bool crc = false;
    bool addressFiltering = true;
    size_t packetLength = 0;
    int16_t beginResult = 0;
    int16_t txResult = 0;
    int16_t rxResult = 0;
    int16_t readResult = 0;
    unsigned int txCount = 0;
    unsigned int rxCount = 0;
    unsigned int initCount = 0;
    void (*callback)() = nullptr;
    std::vector<uint8_t> input;
    std::vector<uint8_t> output;

    int16_t begin(float, float, float, float, int8_t, uint8_t) {
        initialized = true;
        ++initCount;
        return beginResult;
    }
    int16_t setOOK(bool value) { ook = value; return 0; }
    int16_t setSyncWord(uint8_t, uint8_t, uint8_t, bool) { return 0; }
    int16_t disableAddressFiltering() { addressFiltering = false; return 0; }
    int16_t fixedPacketLengthMode(size_t value) { packetLength = value; return 0; }
    int16_t setCrcFiltering(bool value) { crc = value; return 0; }
    void clearPacketReceivedAction() { callback = nullptr; }
    void setPacketReceivedAction(void (*action)()) { callback = action; }
    int16_t standby() {
        assert(initialized);
        listening = false;
        return 0;
    }
    int16_t startReceive() {
        assert(initialized);
        ++rxCount;
        listening = rxResult == 0;
        return rxResult;
    }
    int16_t transmit(const uint8_t* bytes, size_t length) {
        assert(initialized && ook && crc && !addressFiltering);
        assert(length == packetLength);
        listening = false;
        ++txCount;
        output.assign(bytes, bytes + length);
        fakeMillis += 40;
        return txResult;
    }
    int16_t readData(uint8_t* bytes, size_t length) {
        assert(input.size() == length);
        memcpy(bytes, input.data(), length);
        listening = false;
        return readResult;
    }
    float getRSSI() { return -42.5f; }
    void inject(const uint8_t* bytes, size_t length, int16_t result = 0) {
        assert(listening && callback);
        input.assign(bytes, bytes + length);
        readResult = result;
        callback();
    }
};
