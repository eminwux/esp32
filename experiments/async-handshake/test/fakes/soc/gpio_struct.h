#pragma once
#include <stdint.h>

struct FakeWriteRegister {
    bool set;
    void operator=(uint32_t mask);
};
struct FakeGpio {
    FakeWriteRegister out_w1ts{true}, out_w1tc{false};
    uint32_t in = 0;
};
extern FakeGpio GPIO;
