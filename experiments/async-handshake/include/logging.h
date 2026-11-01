#pragma once
#include <Arduino.h>
#include "config.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#ifndef LOG_REGISTERS
#define LOG_REGISTERS 0
#endif
constexpr char kLogNode = config::kNodeLabel;

// Foreground only: call with TX stopped, never from the sampling ISR.
// One bounded buffer/write keeps the identity attached to every firmware line.
inline void logMessage(const char* category, const char* format, ...)
    __attribute__((format(printf, 2, 3)));
inline void logMessage(const char* category, const char* format, ...) {
    char line[256];
    int prefix = snprintf(line, sizeof(line), "[%c %lu ms] %s: ", kLogNode,
                          (unsigned long)millis(), category);
    if (prefix < 0 || size_t(prefix) >= sizeof(line) - 2) return;
    va_list args;
    va_start(args, format);
    vsnprintf(line + prefix, sizeof(line) - prefix - 1, format, args);
    va_end(args);
    size_t n = strlen(line);
    line[n++] = '\n';
    Serial.write(reinterpret_cast<const uint8_t*>(line), n);
}
