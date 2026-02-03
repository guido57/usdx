#pragma once

#include <Arduino.h>
#include <si5351.h>

class FT8_TX {
public:
    FT8_TX(SI5351 &clock);

    void begin(uint32_t baseFreqHz);

    // Ask to send an FT8 message in next slot
    bool requestTransmission(const char *msg);

private:
    static void taskEntry(void *param);
    void taskLoop();
    bool encodeMessage(const char *msg);

    SI5351 &si5351;

    uint32_t baseFreq;
    uint8_t symbols[79];

    volatile bool txRequested = false;

    static constexpr uint16_t toneSpacing = 625;   // 6.25 Hz
    static constexpr uint16_t toneDelayMs = 159;   // FT8 symbol length
};
