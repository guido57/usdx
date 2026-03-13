#include "ft8_tx.h"
#include "ft8/message.h"
#include "ft8/encode.h"
#include "ft8/constants.h"
#include <time.h>
#include "rx_att_pwm.h"
#include "tx_bias_pwm.h"
#include "ui.h"
#include "configuration.h"
#include "ant_filters.h"

extern uint8_t audioVolume; // declared in main.cpp, used here to mute audio during TX
extern bool transmitting; // declared in main.cpp, used here to track if we are currently transmitting
extern AntennaFilters *antFilters; // declared in main.cpp, used here to control TX/RX relay and antenna filters

FT8_TX::FT8_TX(SI5351 &clock) : si5351(clock) {

    // pinMode(GPIO_TX, OUTPUT);
    // digitalWrite(GPIO_TX, LOW); // Start in RX mode (relay de-energized
}

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
bool FT8_TX::requestTransmission(uint32_t baseFreqHz, const char *msg) {

    if (txRequested) return false;
    if (!encodeMessage(msg)) return false;

    baseFreq = baseFreqHz;
    strlcpy(message, msg, sizeof(message));
    txRequested = true;
    return true;
}

// ---------------- CONTINUOUS TX (FOR TESTING) ----------------
bool FT8_TX::startContinuousTransmission(uint32_t baseFreqHz, const char *msg) {
    if (txContinuous) return false; // Already in continuous mode
    if (!encodeMessage(msg)) return false;  


    baseFreq = baseFreqHz;
    strlcpy(message, msg, sizeof(message));
    txContinuous = true;
    Serial.printf("Started continuous transmission at %u Hz\r\n", baseFreq);

    si5351.setupPllForTx(baseFreq); // Pre-configure PLL for the base frequency to speed up the first tone generation
    setRxAttFromUi(0);  // Set RX attenuation to maximum (0V to the Mosfet Gate)
    setTxBiasFromUi(ui_get_tx_bias());  // Set TX bias at 1V to the Mosfet Gate) during tuning and RX to protect the PA   
    audioVolume = 0; // Mute audio during TX    
    // digitalWrite(GPIO_TX, HIGH); // Set relay to TX mode (energize)
    antFilters->setTx(); // Set relay for RX/TX switching to TX
    si5351.oe(0b00000111);  // Enable CLK0, CLK1, CLK2
    return true;
}
// Stop continuous transmission mode
bool FT8_TX::stopContinuousTransmission() {
    if (!txContinuous) return false; // Not in continuous mode

    txContinuous = false;
    Serial.printf("Stopped continuous transmission at %u Hz\r\n", baseFreq);    
    si5351.oe(0b00000011);  // Enable CLK0, CLK1. Disable CLK2
    antFilters->setRx(); // Set relay for RX/TX switching to RX
    setRxAttFromUi(ui_get_att_rf());  // Restore RX attenuation from UI
    setTxBiasFromUi(TX_BIAS_FOR_RX);  // Restore TX bias from UI at a good level for RX (1V to the Mosfet Gate)
    audioVolume = ui_get_volume(); // Restore audio volume from UI    
    //digitalWrite(GPIO_TX, LOW); // Set relay to RX mode (de-energize)
    return true;
}
// ---------------- TASK ENTRY ----------------
void FT8_TX::taskEntry(void *param) {
    static_cast<FT8_TX *>(param)->taskLoop();
}

// ---------------- TASK LOOP ----------------
const uint32_t symbolPeriodUs = 160000;
void FT8_TX::taskLoop() {
    while (true) {
        struct tm t;

        if (getLocalTime(&t)) {

            // 1. Calculate time remaining
            int msToSlot = (15 - (t.tm_sec % 15)) * 1000 - (millis() % 1000);
            if (msToSlot < 0) msToSlot += 15000;

            // 2. Only go through the precision "dance" if a TX is actually wanted
            if (txRequested) {
               txRequested = false;

               // 3. PRE-PREPARE: Do the heavy lifting while we wait
                // We do this 500ms before the slot starts so the hardware is ready
                if (msToSlot > 500) {
                    vTaskDelay(pdMS_TO_TICKS(msToSlot - 500));
                }
                
                // transmitting = true;
                // Now we are 500ms before the start. Prepare the Si5351 and PA.
                // digitalWrite(GPIO_TX, HIGH); // Set relay to TX mode (energize)
                si5351.setupPllForTx(baseFreq); // Pre-configure PLL for the base frequency to speed up the first tone generation
                setRxAttFromUi(0);  // Set RX attenuation to maximum (0V to the Mosfet Gate)
                setTxBiasFromUi(ui_get_tx_bias());  // Set TX bias at 1V to the Mosfet Gate) during tuning and RX to protect the PA   
                audioVolume = 0; // Mute audio during TX    
                //si5351.oe(0b00000111);  // Enable CLK0, CLK1, CLK2

                // 4. THE PRECISION WAIT: Spin until the exact second rolls over
                while (true) {
                    getLocalTime(&t);
                    if (t.tm_sec % 15 == 0) break; // BINGO: We are exactly at 00, 15, 30, or 45
                    taskYIELD(); // Keep the watchdog happy but don't sleep
                }

                // 5. START IMMEDIATELY

                uint32_t startTime = esp_timer_get_time();
                txRequested = false; // Clear the request flag immediately to prevent any re-entrant issues if the UI tries to request another TX during this one
                transmitting = true; // Set transmitting flag to true to indicate we are now in a TX session
                antFilters->setTx(); // Set relay for RX/TX switching to TX
                si5351.oe(0b00000111);  // Enable CLK0, CLK1, CLK2
                Serial.printf("[FT8] start sending: %s\r\n", message);
                for (int i = 0; i < 79; i++) {

                    uint64_t f = (uint64_t)baseFreq * 100ULL +
                                 (uint64_t)symbols[i] * toneSpacing;

                    //si5351.freqb(f/100);
                    si5351.freqb_fast(f); // Use the fast frequency update method that relies on cached PLL parameters for microsecond-level updates

                    // 6. TIGHT TIMING: Avoid vTaskDelay(1) if we are very close
                    while (true) {
                        uint32_t now = esp_timer_get_time();
                        uint32_t target = startTime + (i + 1) * symbolPeriodUs;
                        if (now >= target) break;
                        
                        // If we have more than 2ms, we can sleep a bit
                        if (target - now > 2000) {
                            vTaskDelay(1);
                        } else {
                            taskYIELD(); // Too close to sleep, just yield
                        }
                    }
                }

                si5351.oe(0b00000011);  // Enable CLK0, CLK1. Disable CLK2
                setRxAttFromUi(ui_get_att_rf());  // Restore RX attenuation from UI
                setTxBiasFromUi(TX_BIAS_FOR_RX);  // Restore TX bias from UI at a good level for RX (1V to the Mosfet Gate)
                transmitting = false; // Clear transmitting flag
                antFilters->setRx(); // Set relay for RX/TX switching to RX
                //digitalWrite(GPIO_TX, LOW); // Set relay to RX mode (de-energize)
                audioVolume = ui_get_volume(); // Restore audio volume from UI    
                Serial.printf("[FT8] stop sending\r\n");
                
            }else {
                // No TX requested? Just sleep normally to save power
                vTaskDelay(pdMS_TO_TICKS(msToSlot > 100 ? 100 : msToSlot));
            }
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

