#ifndef FT8_TYPES_H
#define FT8_TYPES_H

#include <Arduino.h>

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
        MSG_CQ_GRID,
        MSG_CQDX,
        MSG_CQDX_GRID,
        MSG_CQDX_ONLY,
        MSG_CQ_REGION,
        MSG_CQ_TEST,
        // END OF CQ TYPES

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
        int snr_db;
        uint32_t ts;
        bool is_cq;
    };

    struct QSOLogEntry {
        uint32_t timestamp;   // seconds or ms
        QsoState state;
        char msg[40];         // FT8 message (fits in 37 chars + margin)
        unsigned char rtx;    // T or R
    };

#define MAX_LOG 10   // keep it bounded!

    struct QSO {
        char call1[12];
        char call2[12];
        char grid1[8];
        char grid2[8];

        int16_t dxcc1;
        int16_t dxcc2;
        
        int8_t snr1;
        int8_t snr2;

        float score1;
        float score2;

        uint32_t firstSeen;
        uint32_t lastHeard;

        char report1[12];
        char report2[12];

        QsoState state;
        char cared[6] = "_____";
        bool cq;

        char reply[24];
        
        int qso_id;
        bool counted_in_stats = false;
        bool is_mine;

        QSOLogEntry log[MAX_LOG];
        uint8_t logCount = 0;
    };

#endif
