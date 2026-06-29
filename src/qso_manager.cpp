#include "qso_manager.h"
#include "ft8_freq_opt.h"
#include "qsostats.h"
#include "adif.h"
#include "pskreporter.h"
#include <time.h>
#include "rgb.h"
extern FT8FreqOptimizer ft8FreqOptimizer; // declared in main.cpp
extern std::vector<FT8_TX::TxJob> txJobs; // declared in ft8_tx.cpp
extern QSOStats qsoStats; // declared in wifi_config.cpp 
extern FT8_TX ft8tx;      // declared in main.cpp
extern Adif adif;   // declared in main.cpp, used here to submit completed QSOs to QRZ.com
QSOManager::QSOManager() {
    qsoMutex = xSemaphoreCreateMutex();
}

void QSOManager::begin() {

    if (qsoMutex == nullptr) {
        qsoMutex = xSemaphoreCreateMutex();
    }

    Serial.printf("\r\n=== Memory Status before qso_list.reserve ===\r\n");
    Serial.printf("Total heap: %u bytes\r\n", ESP.getHeapSize());
    Serial.printf("Free heap: %u bytes\r\n", ESP.getFreeHeap());
    Serial.printf("PSRAM size: %u bytes\r\n", ESP.getPsramSize());
    Serial.printf("Free PSRAM: %u bytes\r\n", ESP.getFreePsram());
  
    qso_list.reserve(100); // pre-allocate space for 100 QSOs to avoid fragmentation and improve performance
    
    Serial.printf("\r\n=== Memory Status after qso_list.reserve ===\r\n");
    Serial.printf("Total heap: %u bytes\r\n", ESP.getHeapSize());
    Serial.printf("Free heap: %u bytes\r\n", ESP.getFreeHeap());
    Serial.printf("PSRAM size: %u bytes\r\n", ESP.getPsramSize());
    Serial.printf("Free PSRAM: %u bytes\r\n", ESP.getFreePsram());

}
// ------------------------------------------------------------
// Helpers
// ------------------------------------------------------------
void QSOManager::lockQsoList() {
    if (qsoMutex) {
        xSemaphoreTake(qsoMutex, portMAX_DELAY);
    }
}

void QSOManager::unlockQsoList() {
    if (qsoMutex) {
        xSemaphoreGive(qsoMutex);
    }
}

void QSOManager::safeCopy(char* dst, const char* src, size_t size) {
    if (!dst || !src || size == 0) return;
    strncpy(dst, src, size - 1);
    dst[size - 1] = '\0';
}

bool QSOManager::isCallsign(const char *s) {
    int len = 0;

    for (int i = 0; s[i]; i++) {
        char c = s[i];

        if (!((c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') ||
              c == '/')) return false;

        len++;
    }

    return (len >= 3 && len <= 12);
}

bool QSOManager::isGrid(const char *s) {
 return strlen(s) == 4 &&
        s[0] >= 'A' && s[0] <= 'R' &&
        s[1] >= 'A' && s[1] <= 'R' &&
        s[2] >= '0' && s[2] <= '9' &&
        s[3] >= '0' && s[3] <= '9';
}

bool QSOManager::isReport(const char *s) {
    if (!s) return false;

    if ((s[0] == '+' || s[0] == '-') && isdigit(s[1]))
        return true;

    if (s[0] == 'R' && (s[1] == '+' || s[1] == '-') && isdigit(s[2]))
        return true;

    return false;
}

// ------------------------------------------------------------

const char* QSOManager::extractFt8Message(const char* line) {
    if (!line) return "";

    const char* p = strchr(line, '~');
    if (!p) return line;

    p++;
    while (*p == ' ') p++;

    return p;
}

const char* QSOManager::stateToString(QsoState s) {
    switch(s){
        case QSO_CQ: return "CQ";
        case QSO_CALLING: return "CALLING";
        case QSO_REPORT_RCVD: return "REPORT_RCVD";
        case QSO_REPORT_EXCHANGED: return "REPORT_EXCHANGED";
        case QSO_DONE: return "DONE";
        default: return "?";
    }
}

// ------------------------------------------------------------
// JSON
// ------------------------------------------------------------

String QSOManager::getActiveQSOsJson() {
    std::vector<QSO> snapshot;
    lockQsoList();
    snapshot = qso_list;
    unlockQsoList();

    JsonDocument doc;
    JsonArray arr = doc.to<JsonArray>();

    for (const auto &q : snapshot) {
        if (q.state == QSO_DONE) continue;

        JsonObject o = arr.add<JsonObject>();
        o["call1"] = q.call1;
        o["call2"] = q.call2;
        o["state"] = stateToString(q.state);
        o["cq"] = q.cq;
    }

    String out;
    serializeJson(doc, out);
    return out;
}

String QSOManager::getCompletedQSOsJson() {
    std::vector<QSO> snapshot;
    lockQsoList();
    snapshot = qso_list;
    unlockQsoList();

    JsonDocument doc;
    JsonArray arr = doc.to<JsonArray>();

    for (const auto &q : snapshot) {
        if (q.state != QSO_DONE) continue;

        JsonObject o = arr.add<JsonObject>();
        o["call1"] = q.call1;
        o["call2"] = q.call2;
        o["state"] = stateToString(q.state);
    }

    String out;
    serializeJson(doc, out);
    return out;
}

String QSOManager::getAllQSOsJson() {
    // Keep the payload below 64 KiB to avoid transport/library edge cases on embedded stacks.
    static constexpr size_t JSON_SOFT_LIMIT = 60U * 1024U;

    std::vector<QSO> snapshot;
    lockQsoList();
    snapshot = qso_list;
    unlockQsoList();

    unsigned long scoreCQtotalMicros = 0UL;
    unsigned long scoreTotalLogUs = 0UL;
    uint32_t vfo_freq = ui_get_vfo_freq();
    uint32_t now_epoch = (uint32_t)time(nullptr);

    String out;
    out.reserve(8192);
    out += "[";

    bool first = true;
    size_t serializedCount = 0;

    // Serialize latest QSOs first and stop before payload becomes too large.
    for (auto it = snapshot.rbegin(); it != snapshot.rend(); ++it) {
        QSO &q = *it;

        JsonDocument qdoc;
        JsonObject o = qdoc.to<JsonObject>();

        o["call1"] = q.call1;
        o["call2"] = q.call2;
        o["grid1"] = q.grid1;
        o["grid2"] = q.grid2;
        o["snr1"]  = q.snr1;
        o["snr2"]  = q.snr2;

        unsigned long scoreStart = micros();
        q.score1 = qsoStats.scoreCQ(q.call1, q.grid1, q.dxcc1, q.snr1, vfo_freq, now_epoch);
        o["score1"] = q.score1;
        q.score2 = strcmp(q.call2,"") == 0 ? 0 : qsoStats.scoreCQ(q.call2, q.grid2, q.dxcc2, q.snr2, vfo_freq, now_epoch);
        o["score2"] = q.score2;
        scoreCQtotalMicros += micros() - scoreStart;

        o["report1"] = q.report1;
        o["report2"] = q.report2;
        o["cq"] = q.cq;
        o["firstSeen"] = q.firstSeen;
        o["lastHeard"] = q.lastHeard;
        o["duration"] = (q.lastHeard > q.firstSeen) ? (q.lastHeard - q.firstSeen) : 0;
        o["state"] = stateToString(q.state);
        o["completed"] = (q.state == QSO_DONE);
        o["cared"] = q.cared;
        o["isMine"] = q.is_mine;
        o["qso_id"] = q.qso_id;

        unsigned long logStart = micros();
        JsonArray logArr = o["log"].to<JsonArray>();
        for (uint8_t i = 0; i < q.logCount; i++) {
            JsonObject le = logArr.add<JsonObject>();
            le["ts"] = q.log[i].timestamp;
            le["state"] = stateToString(q.log[i].state);
            le["msg"] = q.log[i].msg;
            le["rtx"] = q.log[i].rtx;
        }
        scoreTotalLogUs += micros() - logStart;

        // send jobs only for the QSO having pending jobs
        if(ft8tx.getNextPendingJob(q.qso_id)) {
            JsonArray queueArr = o["tx_queue"].to<JsonArray>();
            for (FT8_TX::TxJob t : txJobs) {
                if (t.qso_id != q.qso_id || t.cancelled) continue;
                JsonObject te = queueArr.add<JsonObject>();
                te["ts"] = t.targetSlot;
                te["msg"] = t.message;
                te["retries"] = t.retryCount;
                te["transmitting"] = t.transmitting;
            }
       }    

        uint8_t retries = ft8tx.getRetriesforQso(q.qso_id);
        if (retries > 0) {
            o["retryCount"] = retries;
        } else {
            o["nextRetry"] = nullptr;
            o["retryCount"] = 0;
            o["retryMessage"] = "";
        }

        const size_t expectedLen = measureJson(qdoc);
        String item;
        size_t serializedLen = serializeJson(qdoc, item);
        const bool itemLooksComplete = (item.length() > 0 && item[item.length() - 1] == '}');
        if (qdoc.overflowed() || serializedLen == 0 || item.length() == 0 || serializedLen != expectedLen || !itemLooksComplete) {
            Serial.printf(
            "getAllQSOsJson: stopping at qso_id=%d because item serialization failed (overflow=%d, expected=%u, bytes=%u, free_heap=%u, free_psram=%u)\r\n",
                q.qso_id,
                qdoc.overflowed() ? 1 : 0,
            (unsigned)expectedLen,
                (unsigned)serializedLen,
                (unsigned)ESP.getFreeHeap(),
                (unsigned)ESP.getFreePsram());
            break;
        }

        size_t extra = item.length() + (first ? 0U : 1U) + 1U; // comma + closing ']'
        if (out.length() + extra > JSON_SOFT_LIMIT) {
            break;
        }

        if (!first) out += ",";
        out += item;
        first = false;
        serializedCount++;
    }

    out += "]";

    if (serializedCount < snapshot.size()) {
        Serial.printf("getAllQSOsJson: truncated output to %u/%u QSOs to keep JSON under %u bytes\r\n",
                      (unsigned)serializedCount,
                      (unsigned)snapshot.size(),
                      (unsigned)JSON_SOFT_LIMIT);
    }

    // Serial.printf("Total scoreCQ computation time: %lu microseconds\r\n", scoreCQtotalMicros);
    // Serial.printf("Total log processing time: %lu microseconds\r\n", scoreTotalLogUs);
    return out;
}

void QSOManager::streamAllQSOsJson(WebServer &server) {
    std::vector<QSO> snapshot;
    lockQsoList();
    snapshot = qso_list;
    unlockQsoList();

    unsigned long scoreCQtotalMicros = 0UL;
    unsigned long scoreTotalLogUs = 0UL;
    uint32_t vfo_freq = ui_get_vfo_freq();
    uint32_t now_epoch = (uint32_t)time(nullptr);

    server.sendHeader("Connection", "close");
    server.setContentLength(CONTENT_LENGTH_UNKNOWN);
    server.send(200, "application/json", "[");

    bool first = true;
    size_t sentCount = 0;
    size_t skippedCount = 0;

    for (auto it = snapshot.rbegin(); it != snapshot.rend(); ++it) {
        if (!server.client().connected()) {
            break;
        }

        QSO &q = *it;

        JsonDocument qdoc;
        JsonObject o = qdoc.to<JsonObject>();

        o["call1"] = q.call1;
        o["call2"] = q.call2;
        o["grid1"] = q.grid1;
        o["grid2"] = q.grid2;
        o["snr1"]  = q.snr1;
        o["snr2"]  = q.snr2;

        unsigned long scoreStart = micros();
        q.score1 = qsoStats.scoreCQ(q.call1, q.grid1, q.dxcc1, q.snr1, vfo_freq, now_epoch);
        o["score1"] = q.score1;
        q.score2 = strcmp(q.call2, "") == 0 ? 0 : qsoStats.scoreCQ(q.call2, q.grid2, q.dxcc2, q.snr2, vfo_freq, now_epoch);
        o["score2"] = q.score2;
        scoreCQtotalMicros += micros() - scoreStart;

        o["report1"] = q.report1;
        o["report2"] = q.report2;
        o["cq"] = q.cq;
        o["firstSeen"] = q.firstSeen;
        o["lastHeard"] = q.lastHeard;
        o["duration"] = (q.lastHeard > q.firstSeen) ? (q.lastHeard - q.firstSeen) : 0;
        o["state"] = stateToString(q.state);
        o["completed"] = (q.state == QSO_DONE);
        o["cared"] = q.cared;
        o["isMine"] = q.is_mine;
        o["qso_id"] = q.qso_id;

        unsigned long logStart = micros();
        JsonArray logArr = o["log"].to<JsonArray>();
        for (uint8_t i = 0; i < q.logCount; i++) {
            JsonObject le = logArr.add<JsonObject>();
            le["ts"] = q.log[i].timestamp;
            le["state"] = stateToString(q.log[i].state);
            le["msg"] = q.log[i].msg;
            le["rtx"] = q.log[i].rtx;
        }
        scoreTotalLogUs += micros() - logStart;

        // send jobs only for the QSO having pending jobs
        if(ft8tx.getNextPendingJob(q.qso_id)) {
            JsonArray queueArr = o["tx_queue"].to<JsonArray>();
            for (FT8_TX::TxJob t : txJobs) {
                if (t.qso_id != q.qso_id || t.cancelled) continue;
                JsonObject te = queueArr.add<JsonObject>();
                te["ts"] = t.targetSlot;
                te["msg"] = t.message;
                te["retries"] = t.retryCount;
                te["transmitting"] = t.transmitting;
            }
       }    

        uint8_t retries = ft8tx.getRetriesforQso(q.qso_id);
        if (retries > 0) {
            o["retryCount"] = retries;
        } else {
            o["nextRetry"] = nullptr;
            o["retryCount"] = 0;
            o["retryMessage"] = "";
        }

        const size_t needed = measureJson(qdoc) + 1U;
        char *item = (char *)malloc(needed);
        if (!item) {
            skippedCount++;
            Serial.printf(
                "streamAllQSOsJson: skip qso_id=%d due to OOM allocating %u bytes (free_heap=%u, free_psram=%u)\r\n",
                q.qso_id,
                (unsigned)needed,
                (unsigned)ESP.getFreeHeap(),
                (unsigned)ESP.getFreePsram());
            continue;
        }

        size_t serializedLen = serializeJson(qdoc, item, needed);
        if (qdoc.overflowed() || serializedLen == 0) {
            skippedCount++;
            Serial.printf(
                "streamAllQSOsJson: skip qso_id=%d due to serialization failure (overflow=%d, bytes=%u, free_heap=%u, free_psram=%u)\r\n",
                q.qso_id,
                qdoc.overflowed() ? 1 : 0,
                (unsigned)serializedLen,
                (unsigned)ESP.getFreeHeap(),
                (unsigned)ESP.getFreePsram());
            free(item);
            continue;
        }

        if (!first) {
            server.sendContent(",");
        }

        // Write the object in small pieces so the NET task does not block long enough
        // to starve IDLE0 when the TCP stack applies backpressure.
        static constexpr size_t STREAM_CHUNK = 128U;
        bool clientGone = !server.client().connected();
        for (size_t offset = 0; !clientGone && offset < serializedLen; offset += STREAM_CHUNK) {
            if (!server.client().connected()) {
                clientGone = true;
                break;
            }
            size_t chunkLen = serializedLen - offset;
            if (chunkLen > STREAM_CHUNK) {
                chunkLen = STREAM_CHUNK;
            }
            server.sendContent(item + offset, chunkLen);
            if (((offset / STREAM_CHUNK) & 0x03U) == 0U) {
                vTaskDelay(1);
            }
        }
        free(item);
        if (clientGone) {
            break;
        }
        first = false;
        sentCount++;

        // Let lower-priority tasks (including IDLE0) run to avoid TWDT on long streams.
        if ((sentCount & 0x07U) == 0U) {
           // vTaskDelay(1);
        }
    }

    server.sendContent("]");
    server.sendContent("");

    if (skippedCount > 0) {
        Serial.printf("streamAllQSOsJson: sent %u/%u objects, skipped=%u\r\n",
                      (unsigned)sentCount,
                      (unsigned)snapshot.size(),
                      (unsigned)skippedCount);
    }

    // Serial.printf("Total scoreCQ computation time: %lu microseconds\r\n", scoreCQtotalMicros);
    // Serial.printf("Total log processing time: %lu microseconds\r\n", scoreTotalLogUs);
}

bool isRegion(const char *t) {
    return strcmp(t, "EU") == 0 ||
           strcmp(t, "NA") == 0 ||
           strcmp(t, "SA") == 0 ||
           strcmp(t, "AS") == 0 ||
           strcmp(t, "AF") == 0 ||
           strcmp(t, "OC") == 0 ||
           strcmp(t, "RU") == 0 ||
           strcmp(t, "DL") == 0 ||
           strcmp(t, "JA") == 0;
}

// ------------------------------------------------------------
// Robust message parser (NO fragile substring matching)
// ------------------------------------------------------------
// --- FAST FT8 CHARACTER FILTER ---
static inline bool isFt8Char(char c) {
    return (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') ||
           c == ' ' || c == '/' ||
           c == '+' || c == '-';
}

Ft8MsgType QSOManager::parseMessage(const char *msg, Ft8Fields &out) {
    if (!msg) return MSG_UNKNOWN;

    // --------------------------------------------------------
    // HARD FILTER (reject garbage immediately)
    // --------------------------------------------------------
    for (const char *c = msg; *c; c++) {
        if (*c >= 'a' && *c <= 'z') return MSG_UNKNOWN; // kill "a1 a2"
        if (!isFt8Char(*c)) return MSG_UNKNOWN;
    }

    // --------------------------------------------------------
    // Reset output
    // --------------------------------------------------------
    memset(&out, 0, sizeof(out));
    out.type = MSG_UNKNOWN;

    char tmp[64];
    strncpy(tmp, msg, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';

    char *tokens[6] = {0};
    int ntok = 0;

    // --------------------------------------------------------
    // Tokenize
    // --------------------------------------------------------
   // char *p = strtok(tmp, " ");
    // while (p && ntok < 6) {
    //     tokens[ntok++] = p;
    //     p = strtok(nullptr, " ");
    // }

    // if (ntok == 0) return MSG_UNKNOWN;

    // --------------------------------------------------------
    // Tokenize (no strtok)
    // --------------------------------------------------------
    char *p = tmp;
    while (*p && ntok < 6) {
        while (*p == ' ') p++;
        if (!*p) break;

        tokens[ntok++] = p;

        while (*p && *p != ' ') p++;

        if (*p) {
            *p = '\0';
            p++;
        }
    }

    // Empty or whitespace-only payloads cannot be parsed further.
    if (ntok == 0) {
        return MSG_UNKNOWN;
    }

    // --------------------------------------------------------
    // Early reject obvious garbage like "A1 B2"
    // --------------------------------------------------------
    if (ntok == 2) {
        if (!isCallsign(tokens[0]) &&
            !isCallsign(tokens[1])) {
            return MSG_UNKNOWN;
        }
    }
    // ========================================================
    // 1. CQ FAMILY (CQ, CQ DX, CQ REGION, CQ TEST)
    // ========================================================
    if (strcmp(tokens[0], "CQ") == 0) {

        int idx = 1;

        bool isCQDX = false;
        bool isCQTEST = false;
        bool isCQREGION = false;

        // ----------------------------------------------------
        // CQ modifiers
        // ----------------------------------------------------
        if (ntok > 1) {
            if (strcmp(tokens[1], "DX") == 0) {
                isCQDX = true;
                idx = 2;
            }
            else if (strcmp(tokens[1], "TEST") == 0 ||
                     strcmp(tokens[1], "CONTEST") == 0) {
                isCQTEST = true;
                idx = 2;
            }
            else if (isRegion(tokens[1])) {
                isCQREGION = true;
                idx = 2;
            }
        }

        // ----------------------------------------------------
        // Optional callsign
        // ----------------------------------------------------
        if (ntok > idx && isCallsign(tokens[idx])) {
            safeCopy(out.call1, tokens[idx], sizeof(out.call1));
            out.hasCall1 = true;
            idx++;
        }

        // ----------------------------------------------------
        // Optional grid
        // ----------------------------------------------------
        if (ntok > idx && isGrid(tokens[idx])) {
            safeCopy(out.grid, tokens[idx], sizeof(out.grid));
            out.hasGrid = true;
        }

        // ----------------------------------------------------
        // Type classification
        // ----------------------------------------------------
        if (isCQDX) {
            if (out.hasGrid) out.type = MSG_CQDX_GRID;
            else if (out.hasCall1) out.type = MSG_CQDX;
            else out.type = MSG_CQDX_ONLY;
        }
        else if (isCQTEST) {
            out.type = MSG_CQ_TEST;
        }
        else if (isCQREGION) {
            out.type = MSG_CQ_REGION;
        }
        else {
            if (out.hasGrid) out.type = MSG_CQ_GRID;
            else if (out.hasCall1) out.type = MSG_CQ;
            else out.type = MSG_CQ_NO_GRID;
        }

        return out.type;
    }

    // ========================================================
    // 2. TWO-CALLSIGN MESSAGES (QSO exchanges)
    // ========================================================
    if (ntok >= 2) {

        if (isCallsign(tokens[0])) {
            safeCopy(out.call1, tokens[0], sizeof(out.call1));
            out.hasCall1 = true;
        }

        if (isCallsign(tokens[1])) {
            safeCopy(out.call2, tokens[1], sizeof(out.call2));
            out.hasCall2 = true;
        }
    }

    // ========================================================
    // 3. THIRD TOKEN DISCRIMINATOR
    // ========================================================
    if (ntok >= 3) {

        // ----------------------------------------------------
        // End of QSO
        // ----------------------------------------------------
        if (strcmp(tokens[2], "RR73") == 0 ||
            strcmp(tokens[2], "RRR")  == 0 ||
            strcmp(tokens[2], "73")   == 0) {

            if (strcmp(tokens[2], "RR73") == 0) out.type = MSG_RR73;
            else if (strcmp(tokens[2], "RRR") == 0) out.type = MSG_RR73;
            else out.type = MSG_73;

            return out.type;
        }
        // ----------------------------------------------------
        // Reports
        // ----------------------------------------------------
        if (isReport(tokens[2])) {
            safeCopy(out.report, tokens[2], sizeof(out.report));
            out.hasReport = true;

            out.type = (tokens[2][0] == 'R') ? MSG_R_REPORT : MSG_REPORT;
            return out.type;
        }

        // ----------------------------------------------------
        // Grid in QSO
        // ----------------------------------------------------
        if (isGrid(tokens[2])) {
            safeCopy(out.grid, tokens[2], sizeof(out.grid));
            out.hasGrid = true;
        }
    }

    // ========================================================
    // 4. FINAL CLASSIFICATION
    // ========================================================
    if (out.hasCall1 && out.hasCall2) {
        out.type = out.hasGrid ? MSG_CALL : MSG_CALL_NO_GRID;
    } else {
        out.type = MSG_UNKNOWN;
    }

    return out.type;
}

// used for new outgoing messages (not seen before, so no qso_id to match against)
QSOManager::TxEnqueuePlan QSOManager::prepareOutgoingTx(const char* rawMsg, uint32_t nowTsSec, uint8_t requestedParity) {
    TxEnqueuePlan plan{};

    if (!rawMsg || rawMsg[0] == '\0') {
        plan.error = "empty message";
        return plan;
    }

    Ft8Fields fields{};
    Ft8MsgType type = parseMessage(rawMsg, fields);
    if (type == MSG_UNKNOWN) {
        plan.error = "unrecognized message format";
        return plan;
    }

    fields.ts = nowTsSec;
    QSO* q = addOrUpdate(type, fields, nowTsSec, INT8_MIN);
    if (!q) {
        plan.error = "failed to create/update qso";
        return plan;
    }

    plan.ok = true;
    plan.msgType = type;
    plan.qsoId = q->qso_id;
    plan.parity = requestedParity;
    strlcpy(plan.normalizedMsg, rawMsg, sizeof(plan.normalizedMsg));
    return plan;
}

void QSOManager::finalizeCompletedQso(QSO* q, uint32_t freq_hz, uint32_t timestampSec) {
    if (!q || q->state != QSO_DONE || q->counted_in_stats) {
        return;
    }

    int band = freqToBand(freq_hz);
    char bandname[5];
    strcpy(bandname, BandNames[band]);
    char mode[4] = "FT8";

    if (strcmp(q->call1, ui_get_mycall()) == 0) {
        qsoStats.onQSOCompleted(q->call2, freq_hz, timestampSec);
        adif.enqueue(*q, freq_hz, bandname, mode);
    } else if (strcmp(q->call2, ui_get_mycall()) == 0) {
        qsoStats.onQSOCompleted(q->call1, freq_hz, timestampSec);
        adif.enqueue(*q, freq_hz, bandname, mode);
    }

    q->counted_in_stats = true;
}

QSOManager::TxPostResult QSOManager::onTxCompleted(uint32_t expectedQsoId, const char* sentMsg, uint32_t txDoneTsSec, uint32_t txFreqHz) {
    TxPostResult result{};

    if (!sentMsg || sentMsg[0] == '\0') {
        result.error = "empty tx message";
        return result;
    }

    Ft8Fields fields{};
    Ft8MsgType type = parseMessage(sentMsg, fields);
    if (type == MSG_UNKNOWN) {
        result.error = "unrecognized tx message format";
        return result;
    }

    fields.ts = txDoneTsSec;
    QSO* q = addOrUpdate(type, fields, txDoneTsSec, INT8_MIN, &expectedQsoId);
    if (!q) {
        result.error = "failed to update qso after tx";
        return result;
    }

    addLog(q, 'T', txDoneTsSec, q->state, sentMsg);
    finalizeCompletedQso(q, txFreqHz, txDoneTsSec);

    result.ok = true;
    result.qsoId = q->qso_id;
    result.qsoState = q->state;
    result.qsoIdMatched = (expectedQsoId == (uint32_t)q->qso_id);
    return result;
}

String QSOManager::generateReply(Ft8MsgType input_type, const Ft8Fields &f, int snr_db, Ft8MsgType & output_type) {

    String myCall = String(ui_get_mycall());
    String myGrid = String(ui_get_mygrid());

    // Safety: no callsign configured
    if (myCall.isEmpty()) return "";

    String call1 = String(f.call1);
    String call2 = String(f.call2);
    
    // --------------------------------------------------------
    // Determine if message is relevant to me
    // --------------------------------------------------------
    bool isCQ = (input_type == MSG_CQ || input_type == MSG_CQ_NO_GRID || 
                 input_type == MSG_CQ_GRID || input_type == MSG_CQDX  || 
                 input_type == MSG_CQDX_GRID || input_type == MSG_CQDX_ONLY || 
                 input_type == MSG_CQ_REGION || input_type == MSG_CQ_TEST);

    bool directedToMe = (call1 == myCall);

    if (!isCQ && !directedToMe) {
        output_type = MSG_UNKNOWN;
        return ""; // ❌ ignore messages not addressed to me
    }

    // --------------------------------------------------------
    // If it is not a CQ and the message is not directed to me, ignore it
    // If you get here means that call1=MYCALL and the other station is call2, so you should reply to call2
    // --------------------------------------------------------
    
    // --------------------------------------------------------
    // Format report
    // --------------------------------------------------------
    char rpt[5];
    snprintf(rpt, sizeof(rpt), "%+03d", snr_db);

    // --------------------------------------------------------
    // Generate reply
    // --------------------------------------------------------
    String reply = "";
    switch (input_type) {

        case MSG_CQ:
        case MSG_CQ_NO_GRID:
        case MSG_CQ_GRID:
        case MSG_CQDX:
        case MSG_CQDX_GRID:
        case MSG_CQDX_ONLY:
        case MSG_CQ_REGION:
        case MSG_CQ_TEST:
            // CQ CALL GRID → CALL MYCALL GRID
            reply = call1 + " " + myCall + " " + myGrid;
            output_type = MSG_CALL;
            break;

        case MSG_CALL:
        case MSG_CALL_NO_GRID:
            // MYCALL CALL → CALL MYCALL -RPT
            reply = call2 + " " + myCall + " " + String(rpt);
            output_type = MSG_REPORT;
            break;

        case MSG_REPORT:
            // MYCALL CALL -RPT → CALL MYCALL R-RPT
            reply = call2 + " " + myCall + " R" + String(rpt);
            output_type = MSG_R_REPORT;
            break;

        case MSG_R_REPORT:
            // MYCALL CALL R-RPT → CALL MYCALL RR73
            reply = call2 + " " + myCall + " RR73";
            output_type = MSG_RR73;
            break;


        case MSG_RR73:
            // MYCALL CALL RR73 → CALL MYCALL 73
            reply = call2 + " " + myCall + " 73";
            output_type = MSG_73;
            break;

        default:
            reply = "";
            output_type = MSG_UNKNOWN;
            
    }
    if(reply.length()){
        //Serial.println("From call1=" + String(call1) + " call2=" + String(call2) + " type=" + String(f.type) + " Generated reply: " + reply);
        return reply;
    } else{
        output_type = MSG_UNKNOWN;
        return "";
    }
}
// ------------------------------------------------------------
// Main received messages processor
// ------------------------------------------------------------
void QSOManager::processFt8Spot(const Ft8Spot &s) {
    const char* msg = extractFt8Message(s.decoded_line); // get the message part after '~' if present, otherwise the whole line

    Ft8Fields f;
    Ft8MsgType type = parseMessage(msg, f);              // parse the message and extract fields

    time_t timestamp;
    time(&timestamp);
    f.ts = timestamp;

    // Discard my transmissions 
    // Serial.println("Check if it's my own transmission: " + String(msg) + " call1=" + String(f.call1) + " call2=" + String(f.call2) + " grid=" + String(f.grid) + " report=" + String(f.report) + " type=" + String(type) + " snr_db=" + String(s.snr_db));
    if( (f.type <= MSG_CQ_TEST && strcmp(f.call1, ui_get_mycall()) == 0 ) ||
                                strcmp(f.call2, ui_get_mycall()) == 0    )
    {
        Serial.println("Decoded my own transmission. Discard it.");
        return; 
    }

    // Discard unknown messages
    if (type == MSG_UNKNOWN){ 
        // Serial.println("Decoded message " + String(msg) + " with unknown type: " + String(type) + ". Ignoring it.");
        return;
    }

    RGB::ft8Decoded();

    // Serial.println("Processing message: " + String(msg) + " type=" + String(type) + " f.call1=" + String(f.call1) + " f.call2=" + String(f.call2) + " f.grid=" + String(f.grid) + " f.report=" + String(f.report) + " s.snr_db=" + String(s.snr_db));

    // store the decoded fields in the ft8_frequency_optimizer for frequency optimization (CQ density tracking)
    // Serial.println("Storing spot in ft8FreqOptimizer: freq=" + String(s.freq_hz) + " snr_db=" + String(s.snr_db) + " ts=" + String(f.ts) + " is_cq=" + String(f.is_cq) + " vfo_freq=" + String(ui_get_vfo_freq()));
    ft8FreqOptimizer.store(s.freq_hz, s.snr_db, f.ts, f.is_cq, ui_get_vfo_freq());

    // Prepare and send PSKReporter spot for this message
    Ft8Spot psk_spot = s;
    if(f.type <= MSG_CQ_TEST){
        // Serial.println("Message is a CQ type, preparing PSKReporter spot with callsign=" + String(f.call1) + " grid=" + String(f.grid));
        strlcpy(psk_spot.callsign, f.call1, sizeof(psk_spot.callsign));
        if (f.hasGrid) {
            strlcpy(psk_spot.grid, f.grid, sizeof(psk_spot.grid));
        }
    } else {
        // Serial.println("Message is a non-CQ type, preparing PSKReporter spot with callsign=" + String(f.call2) + " grid=" + String(f.grid));
        strlcpy(psk_spot.callsign, f.call2, sizeof(psk_spot.callsign));
        if (f.hasGrid) {
            strlcpy(psk_spot.grid, f.grid, sizeof(psk_spot.grid));
        }
    }   
    
    if (f.hasReport) {
        strlcpy(psk_spot.report, f.report, sizeof(psk_spot.report));
    }

    // Serial.println("Sending PSKReporter spot: freq=" + String(psk_spot.freq_hz) + " snr_db=" + String(psk_spot.snr_db) + " callsign=" + String(psk_spot.callsign) + " grid=" + String(psk_spot.grid) + " report=" + String(psk_spot.report) );
    send_pskreporter_packet(psk_spot);

    // generate the appropriate reply message based on the message type and extracted fields (maybe to move ahead)
    String reply = "";  
    Ft8MsgType reply_type = MSG_UNKNOWN;
    if(type <= MSG_CQ_TEST || strcmp(f.call1, ui_get_mycall()) == 0 ){
        reply = generateReply(type, f, s.snr_db, reply_type);     
        // Serial.println("Decoded message: " + String(msg) + " type=" + String(type) + " call1=" + String(f.call1) + " call2=" + String(f.call2) + " grid=" + String(f.grid) + " report=" + String(f.report) + ". Generated reply: " + reply);
    }

    // add this received message to the QSO list
    QSO * q = addOrUpdate(type, f, s.timestamp, s.snr_db); //, reply);  // add/update the QSO list based on the message type, extracted fields, timestamp and SNR
    if(q == nullptr){
        Serial.println("Failed to add/update QSO for message: " + String(msg));
        return;
    }  
    
    time(&timestamp);

    // Serial.printf("Adding log entry to qso_id: %d for CQ message: %s call1=%s call2=%s type=%d QSO state is %d.\r\n", q->qso_id, msg, f.call1, f.call2, type, q->state);
    addLog(q, 'R', timestamp, q->state, msg);
    //}
    
    finalizeCompletedQso(q, s.freq_hz, f.ts);

    // if the message is directed to me manage the reply scheduling (only for messages directed to me or CQ messages, ignore messages not relevant to me for replying, but still log them if they are CQ or directed to me)
    if(strcmp(f.call1, ui_get_mycall()) == 0) {

        Serial.println("with msg " + String(msg) + " I decoded f.call1=" + String(f.call1) + " f.call2=" + String(f.call2) + " message type " + String(type) + " QSO state is " + String(q->state) + ". Generated reply: " + reply + " reply type: " + String(reply_type) );

        // check any pending retry for this QSO 
        FT8_TX::TxJob * pendingJob = ft8tx.getNextPendingJob(q->qso_id);
        if(pendingJob != nullptr && pendingJob->msgType >= reply_type){
            //Serial.printf("Found pending transmission for QSO %d, message: %s, type: %d\r\n", q->qso_id, pendingJob->message, pendingJob->msgType);
        }else {
            ft8tx.cancelJobsForQso(q->qso_id); // cancel any pending transmission for this QSO, we will schedule a new one with the updated message (reply)    
            
            uint8_t theirParity = ((q->lastHeard / 15) % 2);
            uint8_t myParity = (theirParity == 0) ? 1 : 0;

            Serial.printf("Scheduling FT8 reply to QSO %d at %d Hz: %s type: %d theirParity=%d myParity=%d\r\n", 
                q->qso_id, ui_get_vfo_freq() + ui_get_ft8_offset(), reply.c_str(), reply_type, theirParity, myParity);
            ft8tx.requestTransmission(ui_get_vfo_freq() + ui_get_ft8_offset(), reply.c_str(), reply_type, myParity, q->qso_id);
        }
    }
    
    // --------------------------------------------------------
    // reduce the list size if needed (remove oldest non-"mine" first)
    // --------------------------------------------------------
    // delete CQs oldest than 5 minutes, they are probably stale and will never be replied (but only if they are not "mine", otherwise we risk deleting a CQ for which we are still waiting for a reply and for which we have pending transmission jobs)
    for (auto it = qso_list.begin(); it != qso_list.end(); ) {
        FT8_TX::TxJob * pending_jobs = ft8tx.getNextPendingJob(it->qso_id);
        long ageSec = (timestamp - it->firstSeen);
        // Serial.printf("Checking if I can remove QSO %d with age %lu seconds, state %d, pending job: %s\r\n", it->qso_id, ageSec, it->state, pending_jobs ? "YES" : "NO");   
        if (it->state < QSO_DONE && pending_jobs == nullptr && ageSec > 300) {
            // Serial.printf("Removing oldest (%lu secs) QSO %d with no pending job\r\n", ageSec, it->qso_id);
            it = qso_list.erase(it);
        } else {
            ++it;
        }
    }

    if (qso_list.size() >= QSO_LIST_MAX) {

        lockQsoList();
        bool removed = false;
        
        // for (auto it = qso_list.begin(); it != qso_list.end(); ++it) {
        //     FT8_TX::TxJob * pending_jobs = ft8tx.getNextPendingJob(it->qso_id);
        //     if (it->is_mine && it->state < QSO_DONE && pending_jobs == nullptr ) {
        //         Serial.printf("Removing oldest mine QSO with no pending job %d\r\n", it->qso_id);
        //         qso_list.erase(it);
        //         removed = true;
        //         break;
        //     }
        // }

        // fallback: if it removed nothing, remove oldest not mine, excluding CQ
        if (!removed) {
            for (auto it = qso_list.begin(); it != qso_list.end(); ++it) {
                if (!it->is_mine && it->state > QSO_CQ) { 
                    Serial.printf("Removing oldest non-mine QSO %d\r\n", it->qso_id);
                    qso_list.erase(it);
                    removed = true;
                    break;
                }
            }
        }

        // fallback: if it removed nothing, remove oldest mine, excluding pending jobs
        if (!removed) {
            for (auto it = qso_list.begin(); it != qso_list.end(); ++it) {
                FT8_TX::TxJob * pending_jobs = ft8tx.getNextPendingJob(it->qso_id);
                if (it->is_mine && pending_jobs == nullptr) { 
                    Serial.printf("Removing oldest mine QSO %d\r\n", it->qso_id);
                    qso_list.erase(it);
                    removed = true;
                    break;
                }
            }
        }



        unlockQsoList();
    }
}

// QSO* QSOManager::getQSOByFields(Ft8Fields &f) {

//     if (!f.hasCall1 || !f.call1 || strcmp(f.call1, "") == 0) {
//         Serial.println("getQSOByFields: call1 is missing or empty, cannot find matching QSO");
//         return nullptr;
//     }
   
//     const char* a = f.call1;
//     const char* b = (f.hasCall2 && f.call2) ? f.call2 : ""; // normalize call2

//     for (auto q = qso_list.rbegin(); q != qso_list.rend(); ++q) { // reverse iteration to find the most recent match first

//         if (!q->call1) continue; // sanity check, should not happen
//         const char* qc1 = q->call1;
//         const char* qc2 = (q->call2 && strlen(q->call2) > 0) ? q->call2 : ""; // normalize call2

//         if(f.type <= MSG_CQ_TEST){
//              // For CQ messages we want to match on call1 only
//             if (strcmp(qc1, a) == 0 && q->state == QSO_CQ  ||
//                 // or for CALL messages where I'm call2 (i.e. I tried to call someone but I got a CQ back)
//                 strcmp(qc1, a) == 0 && strcmp(qc2, ui_get_mycall()) == 0 && q->state == QSO_CALLING ) {
//                 return &(*q);
//             }
//         }else if(f.type == MSG_CALL && q->state == QSO_CQ){
//             // for CALLING messages and qso is in state CQ we compare only call1
//             if (strcmp(qc1, a) == 0) {
//                 return &(*q);
//             }
//         }
       
//         else {
//              // both call1 and call2 should be valid, but in any order (bidirectional match))
//             if ((strcmp(qc1, a) == 0 && strcmp(qc2, b) == 0) ||  // qc2 and b can be empty
//                 (strcmp(qc1, b) == 0 && strcmp(qc2, a) == 0)) {
//                 return &(*q);
//             }
//         }
//     }
//     // Serial.printf("No existing QSO found matching call1=%s call2=%s\r\n", f.call1, f.hasCall2 ? f.call2 : "N/A");
//     return nullptr;
// }

QSO* QSOManager::getQSOByFields(Ft8Fields &f) {
    if (!f.hasCall1 || !f.call1 || f.call1[0] == '\0') {
        Serial.println("getQSOByFields: call1 is missing or empty, cannot find matching QSO");
        return nullptr;
    }

    const char* a = f.call1;
    const char* b = (f.hasCall2 && f.call2 && f.call2[0] != '\0') ? f.call2 : "";
    const bool spotIsCQ = (f.type <= MSG_CQ_TEST);

    QSO* doneExactPair = nullptr;

    // Newest first
    for (auto it = qso_list.rbegin(); it != qso_list.rend(); ++it) {
        QSO& q = *it;

        if (q.call1[0] == '\0') continue;

        const char* qc1 = q.call1;
        const char* qc2 = (q.call2[0] != '\0') ? q.call2 : "";

        // Option included: unordered pair match (A,B) == (B,A)
        const bool samePair =
            (strcmp(qc1, a) == 0 && strcmp(qc2, b) == 0) ||
            (strcmp(qc1, b) == 0 && strcmp(qc2, a) == 0);

        if (samePair) {
            if (q.state == QSO_DONE) {
                // exclude completed QSOs with same pair, but keep track of the most recent one 
                // to return it if we find nothing better and the incoming message is 
                // an end-of-QSO type (RR73 or 73)
                if (!doneExactPair) doneExactPair = &q;
                continue;
            }
            return &q;
        }

        // CQ fallback: same call1 and open call2
        if (spotIsCQ &&
            strcmp(qc1, a) == 0 &&
            qc2[0] == '\0' &&
            q.state == QSO_CQ) {
                // CQ message on a CQ state
                return &q;
        }
    }

    // If only match is DONE and incoming type is before RR73, force new QSO
    if (doneExactPair) {
        if (f.type < MSG_RR73) {
            // the msg is before MSG_RR73 but the only match is QSO DONE 
            return nullptr;
        }
        // the msg is MSG_RR73 or 73 and the only match is QSO DONE, 
        // we can consider it a match and update the existing QSO 
        // with the new end-of-QSO message (e.g. we received a RR73 
        // for a QSO that we had in DONE state because we decoded 
        // the RR73 before the CALL or CQ)
        return doneExactPair;
    }

    return nullptr;
}


QSO* QSOManager::getQsoById(int qso_id) {

    for (auto &q : qso_list) {

        if(q.qso_id == qso_id)
            return &q;
    }
    return nullptr;
      
}


QSO * QSOManager::addOrUpdate(Ft8MsgType type, Ft8Fields &f, uint32_t timestamp, int8_t snr_db, const uint32_t* qso_id){ //, const String &reply) {

    if (!f.hasCall1) return nullptr; // safety check: call1 is mandatory for all message types we handle

    lockQsoList();

    QSO *qso = nullptr;
    bool forcedQso = false;

    if (qso_id) {
        qso = getQsoById(static_cast<int>(*qso_id));
        forcedQso = (qso != nullptr);
    }

    if (!qso) {
        qso = getQSOByFields(f);
    }

    // --------------------------------------------------------
    // CREATE if needed (ANY message, not only CQ)
    // --------------------------------------------------------
    if (!qso ||  // if there's still no QSO
                 // or the QSO exist and it's a CQ message and the QSO is completed, we create a new QSO
        (!forcedQso && type <= MSG_CQ_TEST && qso->state == QSO_DONE ) 
        ) {
        
        // if(!qso){
        //     Serial.printf("No existing QSO found for message, creating new QSO for call1=%s call2=%s...\r\n", f.call1, f.hasCall2 ? f.call2 : "N/A");
        // } else if (qso && type <= MSG_CQ_TEST && qso->state == QSO_DONE){
        //     Serial.printf("Existing QSO %d is completed, but received a new CQ message, creating new QSO for call1=%s call2=%s...\r\n", qso->qso_id, f.call1, f.hasCall2 ? f.call2 : "N/A");
        // }
        

        static int next_qso_id = 0;    

        QSO q{};
        safeCopy(q.call1, f.call1, sizeof(q.call1));
        q.dxcc1 = qsoStats.dxccFromCall(f.call1);
        q.score1 = qsoStats.scoreCQ(q.call1, q.grid1, q.dxcc1, q.snr1, ui_get_vfo_freq(), timestamp);
        
        if (f.hasCall2) {
            safeCopy(q.call2, f.call2, sizeof(q.call2));
            q.dxcc2 = qsoStats.dxccFromCall(f.call2);
        }
        q.report1[0] = '\0';
        q.report2[0] = '\0';

        q.snr1 = INT8_MIN;
        q.snr2 = INT8_MIN;

        q.firstSeen = timestamp;
        q.lastHeard = timestamp;

        q.logCount = 0;
        q.counted_in_stats = false;

        q.cq = (type <= MSG_CQ_TEST);
        q.state = (type <= MSG_CQ_TEST) ? QSO_CQ : QSO_CALLING;

        q.qso_id = next_qso_id++;

        
        switch(type) {
            case MSG_CQ:
            case MSG_CQ_NO_GRID:
            case MSG_CQ_GRID:
            case MSG_CQDX:
            case MSG_CQDX_GRID:
            case MSG_CQDX_ONLY:
            case MSG_CQ_REGION:
            case MSG_CQ_TEST:
                q.cared[0] = 'C';
                break;
            case MSG_CALL:
            case MSG_CALL_NO_GRID:
                q.cared[1] = 'A';
                break;
            case MSG_REPORT:
                q.cared[2] = 'R';
                break;
            case MSG_R_REPORT:
                q.cared[3] = 'E';
                break;
            case MSG_RR73:
            case MSG_73:
                q.cared[4] = 'D';
                break;
        }

        qso_list.push_back(q);
        qso = &qso_list.back();
        
    }

    // --------------------------------------------------------
    // UPDATE BASIC
    // --------------------------------------------------------
    qso->lastHeard = timestamp;

    if (type <= MSG_CQ_TEST)
        qso->cq = true;

    // --------------------------------------------------------
    // COMPLETE CALL2 if missing
    // --------------------------------------------------------
    if (strlen(qso->call2) == 0 && f.hasCall2) {
        if (strcmp(f.call1, qso->call1) == 0){
            safeCopy(qso->call2, f.call2, sizeof(qso->call2));
            qso->dxcc2 = qsoStats.dxccFromCall(f.call2);
            qso->score2 = qsoStats.scoreCQ(qso->call2, qso->grid2, qso->dxcc2, qso->snr2, ui_get_vfo_freq(), timestamp);
        
        }else{
            safeCopy(qso->call2, f.call1, sizeof(qso->call2));
            qso->dxcc2 = qsoStats.dxccFromCall(f.call1);
            qso->score2 = qsoStats.scoreCQ(qso->call2, qso->grid2, qso->dxcc2, qso->snr2, ui_get_vfo_freq(), timestamp);
        }
        // 🔴 as soon as call2 appears, the QSO is no longer CQ
        if (qso->state == QSO_CQ)
            qso->state = QSO_CALLING;
    }
    // --------------------------------------------------------
    // GRID (directional)
    // --------------------------------------------------------
    if (f.hasGrid){
        if(type <= MSG_CQ_TEST)
            safeCopy(qso->grid1, f.grid, sizeof(qso->grid1));
        else
            safeCopy(qso->grid2, f.grid, sizeof(qso->grid2));
    }    

    // --------------------------------------------------------
    // SNR (directional)
    // --------------------------------------------------------
    if (snr_db != INT8_MIN) {
        if(type <= MSG_CQ_TEST){
            qso->snr1 = snr_db;
        } else {
            if (strcmp(f.call2, qso->call1) == 0)
                qso->snr1 = snr_db;
            else if (strcmp(f.call2, qso->call2) == 0)
                qso->snr2 = snr_db;
        }
    }

    // --------------------------------------------------------
    // REPORTS (directional)
    // --------------------------------------------------------
    if (f.hasReport && strlen(qso->call2) > 0) {

        if (strcmp(f.call1, qso->call1) == 0)
            safeCopy(qso->report1, f.report, sizeof(qso->report1));

        else if (strcmp(f.call1, qso->call2) == 0)
            safeCopy(qso->report2, f.report, sizeof(qso->report2));
    }

    // --------------------------------------------------------
    // STATE = DIRECT mapping from message
    // --------------------------------------------------------
    switch (type) {

        case MSG_CQ:
        case MSG_CQ_NO_GRID:
        case MSG_CQ_GRID:
        case MSG_CQDX:
        case MSG_CQDX_GRID:
        case MSG_CQDX_ONLY:
        case MSG_CQ_REGION:
        case MSG_CQ_TEST:
            //qso->state = QSO_CQ;
            break;

        case MSG_CALL:
        case MSG_CALL_NO_GRID:
            if (qso->state < QSO_CALLING)
                qso->state = QSO_CALLING;
            break;

        case MSG_REPORT:
            if (qso->state < QSO_REPORT_RCVD)
                qso->state = QSO_REPORT_RCVD;
            break;

        case MSG_R_REPORT:
            if (qso->state < QSO_REPORT_EXCHANGED)
                qso->state = QSO_REPORT_EXCHANGED;
            break;

        case MSG_RR73:
        case MSG_73:
            qso->state = QSO_DONE;
            break;

        default:
            break;
    }

    switch(type) {
        case MSG_CQ:
        case MSG_CQ_NO_GRID:
        case MSG_CQ_GRID:
        case MSG_CQDX:
        case MSG_CQDX_GRID:
        case MSG_CQDX_ONLY:
        case MSG_CQ_REGION:
        case MSG_CQ_TEST:
            qso->cared[0] = 'C';
            break;
        case MSG_CALL:
        case MSG_CALL_NO_GRID:
            qso->cared[1] = 'A';
            break;
        case MSG_REPORT:
            qso->cared[2] = 'R';
            break;
        case MSG_R_REPORT:
            qso->cared[3] = 'E';
            break;
        case MSG_RR73:
        case MSG_73:
            qso->cared[4] = 'D';
            break;
    }

    qso->is_mine = (strcmp(qso->call1, ui_get_mycall()) == 0) ||
                    (strcmp(qso->call2, ui_get_mycall()) == 0); // mark as "mine" if call1 or call2 matches my call (this is a simplification, but works for most cases)


    

    unlockQsoList();
    return qso;

}

void QSOManager::addLog(QSO* q, unsigned char rtx, uint32_t ts, QsoState state, const char* msg) {
    if (!q || !msg) return;

    uint8_t idx;

    if (q->logCount < MAX_LOG) {
        idx = q->logCount++;
    } else {
        // shift left (drop oldest)
        for (int i = 1; i < MAX_LOG; i++) {
            q->log[i - 1] = q->log[i];
        }
        idx = MAX_LOG - 1;
    }

    q->log[idx].rtx = rtx;

    q->log[idx].timestamp = ts;
    q->log[idx].state = state;

    strncpy(q->log[idx].msg, msg, sizeof(q->log[idx].msg) - 1);
    q->log[idx].msg[sizeof(q->log[idx].msg) - 1] = '\0';

    // Serial.printf("Added log entry to QSO %d: rtx=%c ts=%lu state=%d msg=%s count=%d idx=%d\r\n", q->qso_id, rtx, ts, state, msg, q->logCount, idx);
}