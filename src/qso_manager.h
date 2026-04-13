#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include <vector>
#include <cstring>
#include <cctype>

#include "wifi_config.h"
#include "ui.h"
#include "ft8_tx.h"

extern FT8_TX ft8tx;

class QSOManager {
public:
    QSOManager();

    // ------------------------------------------------------------
    enum QsoState {
        QSO_CQ,
        QSO_CALLING,
        QSO_REPORT_RCVD,
        QSO_REPORT_EXCHANGED,
        QSO_DONE
    };

    enum Ft8MsgType {
        MSG_CQ,
        MSG_CQ_NO_GRID,
        MSG_CALL,
        MSG_CALL_NO_GRID,
        MSG_REPORT,
        MSG_R_REPORT,
        MSG_RR73,
        MSG_73,
        MSG_UNKNOWN
    };

    struct Ft8Fields {
        Ft8MsgType type;
        char call1[16];
        char call2[16];
        char grid[8];
        char report[8];
        bool hasCall1;
        bool hasCall2;
        bool hasGrid;
        bool hasReport;
    };

    struct QSOLogEntry {
        uint32_t timestamp;   // seconds or ms
        QsoState state;
        char msg[40];         // FT8 message (fits in 37 chars + margin)
        unsigned char rtx;    // T or R
    };

    static const int MAX_LOG = 10;   // keep it bounded!

    struct QSO {
        char call1[12];
        char call2[12];
        char grid1[8];
        char grid2[8];

        int8_t snr1;
        int8_t snr2;

        uint32_t firstSeen;
        uint32_t lastHeard;

        char report1[12];
        char report2[12];

        QsoState state;
        char cared[6] = "_____";
        bool cq;

        char reply[24];
        
        int qso_id;
        bool is_mine;

        QSOLogEntry log[MAX_LOG];
        uint8_t logCount = 0;
    };

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
    QSO * addOrUpdate(Ft8MsgType type, Ft8Fields &f, uint32_t timestamp, int8_t snr_db, const String &reply);
    QSO * getQSOByFields(Ft8Fields &f);

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

    
    String generateReply(Ft8MsgType type, const Ft8Fields &f, int snr_db);
};