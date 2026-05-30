#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include <vector>
#include <cstring>
#include <cctype>

#include "wifi_config.h"
#include "ui.h"
#include "ft8_types.h"
#include "ft8_tx.h"


class QSOManager {
public:
    struct TxEnqueuePlan {
        bool ok = false;
        Ft8MsgType msgType = MSG_UNKNOWN;
        uint32_t qsoId = 0;
        uint8_t parity = 255;
        char normalizedMsg[40] = {0};
        const char* error = nullptr;
    };

    struct TxPostResult {
        bool ok = false;
        uint32_t qsoId = 0;
        QsoState qsoState = QSO_CQ;
        bool qsoIdMatched = false;
        const char* error = nullptr;
    };

    QSOManager();
    void begin();

    // ------------------------------------------------------------


    std::vector<QSO> qso_list;

    // ------------------------------------------------------------
    // Retry mechanism for unanswered CQ calls
    // This allows us to automatically retry sending a reply if we don't get a response within a certain time frame.
    // ------------------------------------------------------------
    const uint8_t MAX_RETRIES = 3;
    
    // struct RetryEntry {
    //     int qso_id;              // link to QSO
    //     uint32_t nextTxSlot;     // when to retry
    //     uint8_t retryCount;
    //     char message[32];
        
    // };

    // std::vector<RetryEntry> retryList;

    // void scheduleRetry(int qso_id, const char* msg, uint32_t slot);
    // void cancelRetry(int qso_id);
    // RetryEntry* getNextRetry(uint32_t currentSlot);
    // RetryEntry* getRetryByQsoId(int qso_id);

    void addLog(QSO* q, unsigned char rtx, uint32_t ts, QsoState state, const char* msg);

    // Public API
    void processFt8Spot(const Ft8Spot &s);
    QSO * addOrUpdate(Ft8MsgType type, Ft8Fields &f, uint32_t timestamp, int8_t snr_db); //, const String &reply);
    QSO * getQSOByFields(Ft8Fields &f);
    QSO* getQsoById(int qso_id);


    String getAllQSOsJson();
    String getActiveQSOsJson();
    String getCompletedQSOsJson();
    Ft8MsgType parseMessage(const char *msg, Ft8Fields &out);
    TxEnqueuePlan prepareOutgoingTx(const char* rawMsg, uint32_t nowTsSec, uint8_t requestedParity);
    TxPostResult onTxCompleted(uint32_t expectedQsoId, const char* sentMsg, uint32_t txDoneTsSec, uint32_t txFreqHz);

private:
    // Helpers
    void safeCopy(char* dst, const char* src, size_t size);
    bool isCallsign(const char *s);
    bool isGrid(const char *s);
    bool isReport(const char *s);

    const char* extractFt8Message(const char* line);
    const char* stateToString(QsoState s);
    void finalizeCompletedQso(QSO* q, uint32_t freq_hz, uint32_t timestampSec);

    
    String generateReply(Ft8MsgType type, const Ft8Fields &f, int snr_db, Ft8MsgType &output_type);
};