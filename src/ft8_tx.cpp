#include "ft8_tx.h"
#include "ft8/message.h"
// #include "ft8/encode.h"
#include "ft8_decode/encode.h"

// #include "ft8/constants.h"
#include "ft8_decode/constants.h"
#include <time.h>
//#include "rx_att_pwm.h"
//#include "tx_bias_pwm.h"
#include "ui.h"
#include "configuration.h"
#include "ant_filters.h"
#include "ft8_types.h"
#include "qso_manager.h"
#include "ft8_freq_opt.h"

extern uint8_t audioVolume; // declared in main.cpp, used here to mute audio during TX
extern bool transmitting; // declared in main.cpp, used here to track if we are currently transmitting
extern AntennaFilters *antFilters; // declared in main.cpp, used here to control TX/RX relay and antenna filters
extern QSOManager qsoManager; // declared in wifi_config.cpp, used here to manage QSOs and generate replies
extern FT8FreqOptimizer ft8FreqOptimizer; // declared in main.cpp

std::vector<FT8_TX::TxJob> txJobs;
uint32_t nextJobId = 1;


FT8_TX::FT8_TX(SI5351 &clock) : si5351(clock) {

    // pinMode(GPIO_TX, OUTPUT);
    // digitalWrite(GPIO_TX, LOW); // Start in RX mode (relay de-energized
    
}

void FT8_TX::begin() {
    //baseFreq = baseFreqHz;
    txQueue = xQueueCreate(8, sizeof(TxRequest)); // Queue for TX requests, can hold up to 8 requests

    xTaskCreatePinnedToCore(
        taskEntry,
        "FT8_TX",
        4096,
        this,
        1,
        nullptr,
        1
    );
}

// ---------------- ENCODE MESSAGE ----------------
bool FT8_TX::encodeMessage(const char *msg) {
    ftx_message_t m;
    if (ftx_message_encode(&m, nullptr, msg) != FTX_MESSAGE_RC_OK)
        return false;

    ft8lib_encode(m.payload, symbols);
    return true;
}

// ----------------- SLOT PARITY CALCULATION ----------------
uint8_t getSlotParity(struct tm &t) {
    return ((t.tm_sec / 15) % 2); // 0 even, 1 odd
}

// ---------------- REQUEST TX ----------------
bool FT8_TX::requestTransmission(uint32_t baseFreqHz, const char *msg, Ft8MsgType msgType, uint8_t parity, uint32_t qso_id) {

    TxRequest req;

    if (!encodeMessage(msg)) return false;

    req.baseFreq = baseFreqHz;
    req.qso_id = qso_id;
    strlcpy(req.message, msg, sizeof(req.message));

    req.parity = parity;   // ⭐ store desired slot
    req.msgType = msgType; // ⭐ store message type
    //txRequested = true;
    if(xQueueSend(txQueue, &req, 0) != pdTRUE) {
        Serial.println("Failed to enqueue TX request");
        return false;
    }

    return true;
}

// ---------------- CONTINUOUS TX (FOR TESTING) ----------------
bool FT8_TX::startContinuousTransmission(uint32_t baseFreqHz, const char *msg) {
    if (txContinuous) return false; // Already in continuous mode
    if (!encodeMessage(msg)) return false;  


    txContinuous = true;
    transmitting = true; // Set transmitting flag to true to indicate we are now in a TX session
    Serial.printf("Started continuous transmission at %u Hz\r\n", baseFreqHz);

    si5351.setupPllForTx(baseFreqHz); // Pre-configure PLL for the base frequency to speed up the first tone generation
    //setRxAttFromUi(0);  // Set RX attenuation to maximum (0V to the Mosfet Gate)
    //setTxBiasFromUi(ui_get_tx_bias());  // Set TX bias at 1V to the Mosfet Gate) during tuning and RX to protect the PA   
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
    transmitting = false; // Clear transmitting flag
    Serial.printf("Stopped continuous transmission \r\n");    
    si5351.oe(0b00000011);  // Enable CLK0, CLK1. Disable CLK2
    antFilters->setRx(); // Set relay for RX/TX switching to RX
    //setRxAttFromUi(ui_get_att_rf());  // Restore RX attenuation from UI
    //setTxBiasFromUi(TX_BIAS_FOR_RX);  // Restore TX bias from UI at a good level for RX (1V to the Mosfet Gate)
    audioVolume = ui_get_volume(); // Restore audio volume from UI    
    //digitalWrite(GPIO_TX, LOW); // Set relay to RX mode (de-energize)
    return true;
}

uint8_t FT8_TX::getRetriesforQso(int qso_id) {
    for (auto &j : txJobs) {
        if (j.qso_id == qso_id) {
            return j.retryCount;
        }
    }
    return 0;
}

// get the next pending job for a given QSO (that is not cancelled and has the earliest target slot)
FT8_TX::TxJob * FT8_TX::getNextPendingJob(int qso_id) {
    // QSO * q = & qsoManager.qso_list[qso_id];
    QSO * q = qsoManager.getQsoById(qso_id);
    for (auto &j : txJobs) {
        //Serial.printf("Examining job id=%u QSO id=%d\n", j.id, qso_id);
        if (j.qso_id == qso_id  && j.cancelled == false) { 
            return &j;
            // Serial.printf("Cancelled job id=%u QSO=%d\n", j.id, qso_id);
        }
    }
    return nullptr;
}

void FT8_TX::cancelJobsForQso(int qso_id) {
    // Serial.printf("Cancelling jobs for QSO %d\n", qso_id);
    QSO * q = & qsoManager.qso_list[qso_id];
    for (auto &j : txJobs) {
        //Serial.printf("Examining job id=%u QSO id=%d\n", j.id, qso_id);
        if (j.qso_id == qso_id  ) { // Only cancel jobs that are for the same QSO and have a message type less than or equal to the current message type (to avoid cancelling newer jobs that may have been created after this message was sent)
            j.cancelled = true;
            // Serial.printf("Cancelled job id=%u QSO=%d\n", j.id, qso_id);
        }
    }
}

FT8_TX::TxJob FT8_TX::makeJobFromRequest(const TxRequest& req, int64_t absoluteSlot) {

    TxJob job;

    job.id = nextJobId++;
    job.qso_id = req.qso_id;
    job.baseFreq = req.baseFreq;
    job.retryCount = 0;
    job.transmitting = false; // Initialize transmitting flag to false
    strlcpy(job.message, req.message, sizeof(job.message));
    job.msgType = req.msgType; // ⭐ store message type

    job.cancelled = false;

    switch (req.parity)
    {
    case 0: // even slot
        // job.targetSlot = (absoluteSlot & ~1LL) + 2; // next even slot    
        job.targetSlot =  (absoluteSlot & 1LL) ? absoluteSlot + 1 : absoluteSlot + 2; // next even slot
        job.parity = 0; // mark as even parity
        break;
    
    case 1: // odd slot   
        // job.targetSlot = (absoluteSlot | 1LL) + 2; // next odd slot
        job.targetSlot =  (absoluteSlot & 1LL) ? absoluteSlot + 2 : absoluteSlot + 1; // next odd slot
        job.parity = 1; // mark as odd parity
        break;

    default:
        job.targetSlot = absoluteSlot + 1; // default to next slot without parity constraint
        job.parity = job.targetSlot & 1; // mark as no parity constraint
        break;
    }

    return job;
}

inline int64_t getNowMs() {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (int64_t)tv.tv_sec * 1000LL + tv.tv_usec / 1000;
}

// ---------------- TASK ENTRY ----------------
void FT8_TX::taskEntry(void *param) {
    static_cast<FT8_TX *>(param)->taskLoop();
}

const uint32_t symbolPeriodUs = 160000;

void FT8_TX::taskLoop() {

    while (true) {

        // --------------------------------------------------------
        // 1) Compute timing
        // --------------------------------------------------------
        int64_t nowMs = getNowMs();
        int64_t slot = nowMs / 15000LL;
        int64_t nextSlotMs = (slot + 1) * 15000LL;

        // wake ~500 ms before slot boundary
        int64_t wakeTime = nextSlotMs - 500;

        while (getNowMs() < wakeTime) {
            vTaskDelay(1);
        }

        // --------------------------------------------------------
        // 2) Drain queue → create jobs
        // --------------------------------------------------------
        TxRequest req;
        while (xQueueReceive(txQueue, &req, 0) == pdTRUE) {

            int64_t nowSlot = getNowMs() / 15000LL;
            TxJob job = makeJobFromRequest(req, nowSlot);

            txJobs.push_back(job);

            Serial.printf("[FT8] queued job id=%u QSO=%u parity=%d: %s\n",
                        job.id, job.qso_id, job.parity, job.message);
        }

        if(ui_get_ft8_offset_enabled()) {
            // get the calculated ft8 offset
            uint16_t bestFt8Offset = 
                ft8FreqOptimizer.best_freq(ui_get_vfo_freq(), false, false);
            // Serial.printf("[FT8] applying best frequency offset of %d Hz\n", bestFt8Offset);
            setFt8Offset(bestFt8Offset); // save it to the UI state so it can be displayed and used for the next transmissions
        }   

        // --------------------------------------------------------
        // 3) Select best job (eligible NOW)
        // --------------------------------------------------------
        TxJob* best = nullptr;
        int64_t currentSlot = getNowMs() / 15000LL;

        for (auto &j : txJobs) {
            // Serial.printf("[FT8] examining job id=%u QSO=%u parity=%d targetSlot=%lld cancelled=%d: %s\n",
            //               j.id, j.qso_id, j.parity, j.targetSlot, j.cancelled, j.message);
            if (j.cancelled) continue;

            // not yet time → skip
            if (j.targetSlot > currentSlot + 1){
                Serial.printf("[FT8] job id=%u not eligible yet (targetSlot=%lld currentSlot=%lld), skipping\n", j.id, j.targetSlot, currentSlot);
                continue;

            } 

            if (!best || j.targetSlot < best->targetSlot) {
                best = &j;
            }
        }
        if(best)
            Serial.printf("[FT8] currentSlot=%lld selected job id=%u QSO=%u parity=%d targetSlot=%lld: %s\n",
                       currentSlot, best ? best->id : 0, best ? best->qso_id : 0, best ? best->parity : 255, best ? best->targetSlot : 0, best ? best->message : "N/A");
        // --------------------------------------------------------
        // 4) Enforce parity
        // --------------------------------------------------------
        int64_t txSlot = currentSlot + 1;

        if (best && best->parity != 255 && (txSlot & 1) != best->parity) {
            Serial.printf("[FT8] job %u parity mismatch, skipping\n", best->id);
            best = nullptr;
        }
        // --------------------------------------------------------
        // 5) Wait exact slot boundary (tight)
        // --------------------------------------------------------
        int64_t targetTimeMs = txSlot * 15000LL;

        while (true) {
            int64_t now = getNowMs();
            int64_t diff = targetTimeMs - now;

            if (diff <= 0) break;

            if (diff > 2) {
                taskYIELD();
            }
        }

        // --------------------------------------------------------
        // 6) TRANSMIT (if job exists)
        // --------------------------------------------------------
        if (best) {

            best->baseFreq = ui_get_vfo_freq() + ui_get_ft8_offset(); 
            
            // Serial.printf("[FT8] TX job %u: %s\n", best->id, best->message);
            best ->transmitting = true; // Mark job as transmitting for UI purposes (not used in scheduling logic)

            uint32_t startTime = esp_timer_get_time();

            transmitting = true;

            si5351.setupPllForTx(best->baseFreq);
            //setRxAttFromUi(0);
            //setTxBiasFromUi(ui_get_tx_bias());
            audioVolume = 0;

            antFilters->setTx();
            si5351.oe(0b00000111);

            for (int i = 0; i < 79; i++) {

                uint64_t f = (uint64_t)best->baseFreq * 100ULL +
                             (uint64_t)symbols[i] * toneSpacing;

                si5351.freqb_fast(f);

                while (true) {
                    uint32_t now = esp_timer_get_time();
                    uint32_t target = startTime + (i + 1) * symbolPeriodUs;

                    if (now >= target) break;

                    if (target - now > 2000) {
                        vTaskDelay(1);
                    } else {
                        taskYIELD();
                    }
                }
            }

            // ----------------------------------------------------
            // End TX
            // ----------------------------------------------------
            best->transmitting = false; // Clear transmitting flag for UI purposes
            si5351.oe(0b00000011);
            //setRxAttFromUi(ui_get_att_rf());
            //setTxBiasFromUi(TX_BIAS_FOR_RX);

            transmitting = false;
            antFilters->setRx();
            audioVolume = ui_get_volume();

            // Serial.printf("[FT8] TX done job %u\n", best->id);

            // ----------------------------------------------------
            // 7) Post-TX: QSO + retry logic
            // ----------------------------------------------------
            time_t timestamp;
            time(&timestamp);

            Ft8Fields f;
            Ft8MsgType type = qsoManager.parseMessage(best->message, f);
            QSO *q = qsoManager.addOrUpdate(type, f, timestamp, 127); // Update QSO state based on the message we just sent (SNR is not relevant for sent messages, so we pass 0)
            if(q->qso_id != best->qso_id) {
                Serial.printf("[FT8] Warning: QSO ID mismatch after addOrUpdate. Expected %d, got %d\n", best->qso_id, q->qso_id);
            }
            qsoManager.addLog(q, 'T', timestamp, q->state, best->message);

            // Retry logic
            if (q->state == QSO_DONE) {

                best->cancelled = true;

            } else if (best->retryCount >= qsoManager.MAX_RETRIES) {

                best->cancelled = true;
                Serial.printf("[FT8] retry job %u MAX_RETRIES reached\n", best->id);
            } else {

                best->retryCount++;

                int64_t nowSlot = getNowMs() / 15000LL;
                int64_t retrySlot = nowSlot + 3;

                if (best->parity != 255 && (retrySlot & 1) != best->parity) {
                    retrySlot++;
                }

                best->targetSlot = retrySlot;
                best->cancelled = false;

                // Serial.printf("[FT8] retry job %u nextSlot=%lld\n",
                //               best->id, retrySlot);
            }
        }
        else {
            // No TX this slot
            // Serial.printf("[FT8] idle slot\n");
        }
    }
}

