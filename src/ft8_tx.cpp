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
#include "qso_manager.h"

extern uint8_t audioVolume; // declared in main.cpp, used here to mute audio during TX
extern bool transmitting; // declared in main.cpp, used here to track if we are currently transmitting
extern AntennaFilters *antFilters; // declared in main.cpp, used here to control TX/RX relay and antenna filters
extern QSOManager qsoManager; // declared in wifi_config.cpp, used here to manage QSOs and generate replies

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

// ----------------- SLOT PARITY CALCULATION ----------------
uint8_t getSlotParity(struct tm &t) {
    return ((t.tm_sec / 15) % 2); // 0 even, 1 odd
}

// ---------------- REQUEST TX ----------------
bool FT8_TX::requestTransmission(uint32_t baseFreqHz, const char *msg, uint8_t parity, uint32_t qso_id) {

    TxRequest req;

    if (!encodeMessage(msg)) return false;

    req.baseFreq = baseFreqHz;
    req.qso_id = qso_id;
    strlcpy(req.message, msg, sizeof(req.message));

    req.parity = parity;   // ⭐ store desired slot
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
    transmitting = false; // Clear transmitting flag
    Serial.printf("Stopped continuous transmission \r\n");    
    si5351.oe(0b00000011);  // Enable CLK0, CLK1. Disable CLK2
    antFilters->setRx(); // Set relay for RX/TX switching to RX
    setRxAttFromUi(ui_get_att_rf());  // Restore RX attenuation from UI
    setTxBiasFromUi(TX_BIAS_FOR_RX);  // Restore TX bias from UI at a good level for RX (1V to the Mosfet Gate)
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


void FT8_TX::cancelJobsForQso(int qso_id) {
    Serial.printf("Cancelling jobs for QSO %d\n", qso_id);
    for (auto &j : txJobs) {
        Serial.printf("Examining job id=%u QSO id=%d\n", j.id, qso_id);
        if (j.qso_id == qso_id) {
            j.cancelled = true;
            Serial.printf("Cancelled job id=%u QSO=%d\n", j.id, qso_id);
        }
    }
}

FT8_TX::TxJob FT8_TX::makeJobFromRequest(const TxRequest& req, int64_t absoluteSlot) {

    TxJob job;

    job.id = nextJobId++;
    job.qso_id = req.qso_id;

    job.baseFreq = req.baseFreq;
    job.parity   = req.parity;

    strlcpy(job.message, req.message, sizeof(job.message));

    job.cancelled = false;

    // recalc target slot based on requested parity and current absolute slot
    int64_t slot = absoluteSlot;

    // always go to future
    slot += 1;

    // parity correction
    if (job.parity != 255 && (slot & 1) != job.parity) {
        slot++;
    }

    // optional safety: ensure not in past
    if (slot <= absoluteSlot) {
        slot = absoluteSlot + 1;
    }

    job.targetSlot = slot;
    
    
    
    job.retryCount = 0;

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

        // --------------------------------------------------------
        // 3) Select best job (eligible NOW)
        // --------------------------------------------------------
        TxJob* best = nullptr;
        int64_t currentSlot = getNowMs() / 15000LL;

        for (auto &j : txJobs) {

            if (j.cancelled) continue;

            // not yet time → skip
            if (j.targetSlot > currentSlot) continue;

            if (!best || j.targetSlot < best->targetSlot) {
                best = &j;
            }
        }

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

            Serial.printf("[FT8] TX job %u: %s\n", best->id, best->message);

            uint32_t startTime = esp_timer_get_time();

            transmitting = true;

            si5351.setupPllForTx(best->baseFreq);
            setRxAttFromUi(0);
            setTxBiasFromUi(ui_get_tx_bias());
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
            si5351.oe(0b00000011);
            setRxAttFromUi(ui_get_att_rf());
            setTxBiasFromUi(TX_BIAS_FOR_RX);

            transmitting = false;
            antFilters->setRx();
            audioVolume = ui_get_volume();

            Serial.printf("[FT8] TX done job %u\n", best->id);

            // ----------------------------------------------------
            // 7) Post-TX: QSO + retry logic
            // ----------------------------------------------------
            time_t timestamp;
            time(&timestamp);

            QSOManager::Ft8Fields f;
            auto type = qsoManager.parseMessage(best->message, f);

            QSOManager::QSO *q =
                qsoManager.addOrUpdate(type, f, timestamp, INT8_MAX, "");

            qsoManager.addLog(q, 'T', timestamp, q->state, best->message);

            // Retry logic
            if (q->state == QSOManager::QSO_DONE) {

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

                Serial.printf("[FT8] retry job %u nextSlot=%lld\n",
                              best->id, retrySlot);
            }
        }
        else {
            // No TX this slot
            Serial.printf("[FT8] idle slot\n");
        }
    }
}

// void FT8_TX::taskLoop() {

//     TxRequest currentReq;
//     bool hasRequest = false;
//     uint32_t selectedJobId = 0;
//     bool hasSelectedJob = false;

//     while (true) {

//         // get seconds and milliseconds since epoch to determine current slot and parity
//         // struct timeval tv;
//         // gettimeofday(&tv, NULL);
//         // int64_t nowMs = (int64_t)tv.tv_sec * 1000LL + tv.tv_usec / 1000; // ms since epoch
//         // time_t now_sec = tv.tv_sec;                                      // seconds since epoch
//         int64_t nowMs = getNowMs(); // ms since epoch

//         // calculate current absolute slot since epoch and parity
//         int64_t absoluteSlot = nowMs / 15000LL; // 15 seconds per slot
//         uint8_t currentParity = absoluteSlot & 1; // 0 even, 1 odd

//         // calculate ms to next slot boundary (for scheduling)
//         int64_t nextSlotMs = (int64_t)(absoluteSlot + 1) * 15000LL; // ms until next slot
//         int64_t msToSlot = nextSlotMs - nowMs;
        
//         if (msToSlot < 0) msToSlot = 0; // Sanity check (should never happen)
//         // ------------------------------------------------------------
//         // Fetch next TX request (only if idle and none pending)
//         // ------------------------------------------------------------
//         if (!transmitting && !hasRequest) {

//             // 1. Drain queue → convert to jobs
//             TxRequest req;
//             while (xQueueReceive(txQueue, &req, 0) == pdTRUE) {

//                 TxJob job = makeJobFromRequest(req, absoluteSlot);
//                 txJobs.push_back(job);

//                 Serial.printf("[FT8] queued job id=%u QSO id=%u parity=%d: %s\n", job.id, job.qso_id, job.parity, job.message);
//             }

//             // 3. Select next job
//             TxJob * best = nullptr;

//             for (auto &j : txJobs) {

//                 if (j.cancelled) continue;

//                 if (j.targetSlot > absoluteSlot)
//                     continue;

//                 if (!best || j.targetSlot < best->targetSlot) {
//                     best = &j;
//                 }
//             }

//             if (best) {

//                 // store ONLY the ID (safe across time)
//                 selectedJobId = best->id;
//                 hasSelectedJob = true;

//                 currentReq.baseFreq = best->baseFreq;
//                 currentReq.parity   = best->parity;
//                 strlcpy(currentReq.message, best->message, sizeof(currentReq.message));

//                 hasRequest = true;

//                 Serial.printf("[FT8] selected job id=%u parity=%d: %s\n", best->id, best->parity , best->message);
//             }
//         }
//         // ------------------------------------------------------------
//         // TRANSMIT
//         // ------------------------------------------------------------
//         if (hasRequest) {

//             // Copy request locally (freeze it)
//             uint32_t baseFreqLocal = currentReq.baseFreq;
//             uint8_t requestedParityLocal = currentReq.parity;

//             char messageLocal[64];
//             strlcpy(messageLocal, currentReq.message, sizeof(messageLocal));

//             Serial.printf("[FT8] absoluteSlot: %lld  parity: %d  ms to next slot: %lld\r\n", absoluteSlot, requestedParityLocal, msToSlot);

//             // --------------------------------------------------------
//             // Pre-wait until ~500ms before slot
//             // --------------------------------------------------------
//             if (msToSlot > 500) {
//                 Serial.printf("[FT8] pre-waiting for %lld ms until pre-slot boundary\n", msToSlot - 500);
//                 vTaskDelay(pdMS_TO_TICKS(msToSlot - 500));
//             }
//             // --------------------------------------------------------
//             // Pre-configure hardware
//             // --------------------------------------------------------
//             si5351.setupPllForTx(baseFreqLocal);
//             setRxAttFromUi(0);
//             setTxBiasFromUi(ui_get_tx_bias());
//             audioVolume = 0;
//             // --------------------------------------------------------
//             // Precision wait for slot boundary
//             // --------------------------------------------------------
//             Serial.printf("[FT8] precision waiting for slot boundary with fine precision. \n");
//             int64_t startSlot = getNowMs() / 15000LL;

//             int64_t targetSlot = startSlot + 1;

//             // enforce parity
//             if (requestedParityLocal != 255 && (targetSlot & 1) != requestedParityLocal) {
//                 targetSlot++;
//             }

//             int64_t targetTimeMs = targetSlot * 15000LL;

//             while (getNowMs() < targetTimeMs) {
//                 taskYIELD();
//             }
//             // --------------------------------------------------------
//             // START TX
//             // --------------------------------------------------------
//             uint32_t startTime = esp_timer_get_time();

//             transmitting = true;
//             hasRequest = false;   // ✅ consume request here

//             antFilters->setTx();
//             si5351.oe(0b00000111);

//             int64_t txNowMs = getNowMs();
//             Serial.printf("[FT8] start sending: %s at slot %lld with parity: %d\r\n",
//                           messageLocal, txNowMs / 15000LL , requestedParityLocal);

//             for (int i = 0; i < 79; i++) {

//                 uint64_t f = (uint64_t)baseFreqLocal * 100ULL +
//                              (uint64_t)symbols[i] * toneSpacing;

//                 si5351.freqb_fast(f);

//                 while (true) {
//                     uint32_t now = esp_timer_get_time();
//                     uint32_t target = startTime + (i + 1) * symbolPeriodUs;

//                     if (now >= target) break;

//                     if (target - now > 2000) {
//                         vTaskDelay(1);
//                     } else {
//                         taskYIELD();
//                     }
//                 }
//             }

//             // --------------------------------------------------------
//             // END TX → back to RX
//             // --------------------------------------------------------
//             si5351.oe(0b00000011);

//             setRxAttFromUi(ui_get_att_rf());
//             setTxBiasFromUi(TX_BIAS_FOR_RX);

//             transmitting = false;

//             antFilters->setRx();
//             audioVolume = ui_get_volume();

//             Serial.printf("[FT8] stop sending\r\n");

//             // --------------------------------------------------------
//             // Log transmission in QSO manager
//             // --------------------------------------------------------
//             time_t timestamp;
//             time(&timestamp);

//             QSOManager::Ft8Fields f;
//             QSOManager::Ft8MsgType type = qsoManager.parseMessage(messageLocal, f);

//             Serial.printf("[FT8] transmission succeeded, add this spot to the QSO list: %s\r\n",
//                           messageLocal);    
            
//             // submit this transmitted message to the same processor used for received messages, to create/update QSO and trigger potential replies and retries based on the new QSO state    
//             QSOManager::QSO *q =
//                 qsoManager.addOrUpdate(type, f, timestamp, INT8_MAX, "");
            
//             Serial.printf("[FT8] QSO state after transmission: %d\r\n", q->state);

//             // log this transmission in the QSO log as well (type 'T' for transmitted)
//             Serial.printf("[FT8] adding log entry to qso_id %d for transmitted message: %s call1=%s call2=%s type=%d QSO state is %d. Generated reply: %s\r\n", q->qso_id, messageLocal, f.call1, f.call2, type, q->state, q->reply);
//             qsoManager.addLog(q, 'T', timestamp, q->state, messageLocal);    

//             // look for the job we just executed and decide if we need to schedule a retry based on the new QSO state    
//             TxJob* selectedJob = nullptr;    
//             if (hasSelectedJob) {
//                 for (auto &j : txJobs) {
//                     if (j.id == selectedJobId && j.cancelled == false) {
//                         selectedJob = &j;
//                         break;
//                     }
//                 }
//             }

//             // decide if we need to schedule a retry based on the new QSO state    
//             if(selectedJob){    
                
//                 if (q->state == QSOManager::QSO_DONE) 
//                 {
//                     Serial.printf("[FT8] QSO completed for job %u: %s, no retry needed.\n", selectedJob->id, selectedJob->message   );
//                     selectedJob->cancelled = true; // Mark job as done, no retry needed
//                 }else{

//                     if (selectedJob->retryCount >= qsoManager.MAX_RETRIES) {
//                         selectedJob->cancelled = true; // Mark job as cancelled to prevent further retries
//                         Serial.printf("[FT8] max retries reached for job %u: %s\n", selectedJob->id, selectedJob->message);

//                     } else {

//                         selectedJob->retryCount++;

//                         // time_t now_sec;
//                         // time(&now_sec);
//                         // int64_t txSlot = now_sec / 15;
//                         struct timeval tv;
//                         gettimeofday(&tv, NULL);
//                         int64_t nowMs = (int64_t)tv.tv_sec * 1000LL + tv.tv_usec / 1000;

//                         int64_t txSlot = nowMs / 15000LL;

//                         int64_t slot = txSlot + 3; // retry after 3 slots (45 seconds)

//                         // but enforce parity
//                         if (selectedJob->parity != 255 && (slot & 1) != selectedJob->parity) {
//                             slot++;
//                         }

//                         selectedJob->targetSlot = slot;

//                         selectedJob->cancelled = false;  // 👈 re-arm

//                         Serial.printf("[FT8] rescheduled job %u retry=%d nextSlot=%lld\n",
//                                     selectedJob->id, selectedJob->retryCount, selectedJob->targetSlot);
//                     }
//                 }        
//             }
//             hasSelectedJob  = false; // reset selection for next round 

//         }else {
//             // --------------------------------------------------------
//             // Idle → sleep efficiently
//             // --------------------------------------------------------
//             int sleepMs = (msToSlot > 100) ? 100 : msToSlot;

//             if (sleepMs > 0) {
//                 vTaskDelay(pdMS_TO_TICKS(sleepMs));
//             }
//         }
//     }
// }
