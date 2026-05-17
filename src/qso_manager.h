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

private:
    // Helpers
    void safeCopy(char* dst, const char* src, size_t size);
    bool isCallsign(const char *s);
    bool isGrid(const char *s);
    bool isReport(const char *s);

    const char* extractFt8Message(const char* line);
    const char* stateToString(QsoState s);

    
    String generateReply(Ft8MsgType type, const Ft8Fields &f, int snr_db, Ft8MsgType &output_type);
};