#pragma once
#include "frame.h"

// Build flags remain in platformio.ini; this is their checked, derived view.
namespace config {
#if defined(NODE_ROLE_A) == defined(NODE_ROLE_B)
#error Define exactly one node role
#endif
#ifdef NODE_ROLE_A
constexpr uint8_t kLocal = 1;
constexpr uint32_t kTxHz = RF_FREQ_A_HZ, kRxHz = RF_FREQ_B_HZ;
#else
constexpr uint8_t kLocal = 2;
constexpr uint32_t kTxHz = RF_FREQ_B_HZ, kRxHz = RF_FREQ_A_HZ;
#endif
constexpr unsigned kSamplesPerBit = ook::kSamplesPerBit;
constexpr uint32_t kBitUs = SAMPLE_PERIOD_US * kSamplesPerBit;
constexpr uint32_t kFrameMs = (ook::kWireSize * 8 * kBitUs + 999) / 1000;
static_assert(SAMPLE_PERIOD_US == 104, "This RF profile and decoder are validated only for 104 us sampling");
static_assert(DUTY_PERMILLE > 0 && DUTY_PERMILLE <= 50, "Choose 1..50 per-mille TX budget");
static_assert(TURNAROUND_DELAY_MS >= 100, "Allow peer retune/calibration/settling");
static_assert(REPLY_TIMEOUT_MS > MIN_INTERFRAME_MS + kFrameMs + TURNAROUND_DELAY_MS + 100,
              "Receive window must cover peer spacing and airtime");
static_assert(REPLY_TIMEOUT_MS > (kFrameMs + 30) * 1000 / DUTY_PERMILLE + TURNAROUND_DELAY_MS + 100,
              "Receive window must cover peer duty-cycle wait");
static_assert(RAW_OOK_TEST == 0 || RAW_OOK_TEST == 1, "RAW_OOK_TEST must be 0 or 1");
static_assert(RF_FREQ_A_HZ >= 433050000UL && RF_FREQ_A_HZ <= 434790000UL &&
              RF_FREQ_B_HZ >= 433050000UL && RF_FREQ_B_HZ <= 434790000UL,
              "This PA/filter profile is restricted to the 433 MHz experiment");

constexpr int kCsPin = 5, kClockPin = 18, kMisoPin = 19, kMosiPin = 23, kDataPin = 26;
constexpr char kNodeLabel = kLocal == 1 ? 'A' : 'B';
constexpr char kPeerLabel = kLocal == 1 ? 'B' : 'A';
}  // namespace config
