#pragma once

#include <Arduino.h>
#include <si5351.h>

class FT8_TX {
public:

    
    struct TxRequest {
        uint32_t baseFreq;
        char message[64];
        uint8_t parity;     // 0,1 or 255 = next available
        uint32_t qso_id;
    };

    struct TxJob {
        uint32_t id;
        int qso_id;              // -1 for CQ or generic

        char message[64];

        uint8_t parity;          // 0,1 or 255 = any
        uint32_t baseFreq;
        
        int64_t targetSlot;   // 👈 NEW
        uint8_t retryCount;   // 👈 moved here

        bool cancelled;
    };

    
    QueueHandle_t txQueue;
    FT8_TX(SI5351 &clock);
   
    void begin();

    // uint8_t requestedParity = 255; // 0 = even, 1 = odd, 255 = auto
    // uint32_t scheduledSlotStart = 0;

    TxJob makeJobFromRequest(const TxRequest& req, int64_t absoluteSlot);
    void cancelJobsForQso(int qso_id);
    uint8_t getRetriesforQso(int qso_id);

    
    // Ask to send an FT8 message in next slot
    bool requestTransmission(uint32_t baseFreqHz, const char *msg, uint8_t parity, uint32_t qso_id);
    bool cancelRequestTransmission(uint32_t qso_id);


    bool startContinuousTransmission(uint32_t baseFreqHz, const char *msg);
    bool stopContinuousTransmission();

private:
    static void taskEntry(void *param);
    void taskLoop();
    bool encodeMessage(const char *msg);
    
    SI5351 &si5351;

    uint8_t symbols[79];

    volatile bool txRequested = false;
    volatile bool txContinuous = false;

    static constexpr uint16_t toneSpacing = 625;   // 6.25 Hz
    static constexpr uint16_t toneDelayMs = 159;   // FT8 symbol length
};
