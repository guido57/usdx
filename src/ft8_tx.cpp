#include "ft8_tx.h"
#include "ft8/message.h"
#include "ft8/encode.h"
#include "ft8/constants.h"
#include <time.h>

FT8_TX::FT8_TX(SI5351 &clock) : si5351(clock) {}

void FT8_TX::begin(uint32_t baseFreqHz) {
    baseFreq = baseFreqHz;

    xTaskCreatePinnedToCore(
        taskEntry,
        "FT8_TX",
        4096,
        this,
        2,
        nullptr,
        1
    );
}

// ---------------- ENCODE MESSAGE ----------------
bool FT8_TX::encodeMessage(const char *msg) {
    ftx_message_t m;
    if (ftx_message_encode(&m, nullptr, msg) != FTX_MESSAGE_RC_OK)
        return false;

    ft8_encode(m.payload, symbols);
    return true;
}

// ---------------- REQUEST TX ----------------
bool FT8_TX::requestTransmission(const char *msg) {

    if (txRequested) return false;
    if (!encodeMessage(msg)) return false;

    txRequested = true;
    return true;
}

// ---------------- TASK ENTRY ----------------
void FT8_TX::taskEntry(void *param) {
    static_cast<FT8_TX *>(param)->taskLoop();
}

// ---------------- TASK LOOP ----------------
void FT8_TX::taskLoop() {
    while (true) {
        struct tm t;

        if (getLocalTime(&t)) {

            // Wait until next 15s boundary
            int msToSlot = (15 - (t.tm_sec % 15)) * 1000 - (millis() % 1000);
            if (msToSlot < 0) msToSlot += 15000;

            vTaskDelay(pdMS_TO_TICKS(msToSlot));

            if (txRequested) {
                txRequested = false;

                si5351.oe(0b00000111);  // Enable CLK0, CLK1, CLK2

                for (int i = 0; i < 79; i++) {

                    uint64_t f = (uint64_t)baseFreq * 100ULL +
                                 (uint64_t)symbols[i] * toneSpacing;

                    // si5351.set_freq(f, SI5351_CLK2);
                    si5351.freqb(f);

                    vTaskDelay(pdMS_TO_TICKS(toneDelayMs));
                }

                si5351.oe(0b00000011);  // Enable CLK0, CLK1. Disable CLK2
            }
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

