#include "qso_manager.h"
#include "ft8_freq_opt.h"
#include "qsostats.h"
#include "adif.h"
#include "pskreporter.h"
extern FT8FreqOptimizer ft8FreqOptimizer; // declared in main.cpp
extern std::vector<FT8_TX::TxJob> txJobs; // declared in ft8_tx.cpp
extern QSOStats qsoStats; // declared in wifi_config.cpp 
extern FT8_TX ft8tx;      // declared in main.cpp
extern Adif adif;   // declared in main.cpp, used here to submit completed QSOs to QRZ.com
QSOManager::QSOManager() {}

void QSOManager::begin() {

    Serial.printf("\n=== Memory Status before qso_list.reserve ===\n");
    Serial.printf("Total heap: %u bytes\n", ESP.getHeapSize());
    Serial.printf("Free heap: %u bytes\n", ESP.getFreeHeap());
    Serial.printf("PSRAM size: %u bytes\n", ESP.getPsramSize());
    Serial.printf("Free PSRAM: %u bytes\n", ESP.getFreePsram());
  
    qso_list.reserve(100); // pre-allocate space for 100 QSOs to avoid fragmentation and improve performance
    
    Serial.printf("\n=== Memory Status after qso_list.reserve ===\n");
    Serial.printf("Total heap: %u bytes\n", ESP.getHeapSize());
    Serial.printf("Free heap: %u bytes\n", ESP.getFreeHeap());
    Serial.printf("PSRAM size: %u bytes\n", ESP.getPsramSize());
    Serial.printf("Free PSRAM: %u bytes\n", ESP.getFreePsram());

}
// ------------------------------------------------------------
// Helpers
// ------------------------------------------------------------
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
    JsonDocument doc;
    JsonArray arr = doc.to<JsonArray>();

    for (const auto &q : qso_list) {
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
    JsonDocument doc;
    JsonArray arr = doc.to<JsonArray>();

    for (const auto &q : qso_list) {
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
    JsonDocument doc;
    JsonArray arr = doc.to<JsonArray>();

    unsigned long start = millis();
    unsigned long scoreCQtotalMicros = 0UL;
    unsigned long scoreTotalLogUs = 0UL;
    uint32_t vfo_freq = ui_get_vfo_freq();
    for (const auto &q : qso_list) {
        JsonObject o = arr.add<JsonObject>();

        o["call1"] = q.call1;
        o["call2"] = q.call2;
        o["grid1"] = q.grid1;
        o["grid2"] = q.grid2;
        o["snr1"]  = q.snr1;
        o["snr2"]  = q.snr2;
        unsigned long start = micros();
        o["score1"] = qsoStats.scoreCQ(q.dxcc1, q.snr1, vfo_freq);
        o["score2"] = strcmp(q.call2,"") == 0 ? 0 : qsoStats.scoreCQ(q.dxcc2, q.snr2, vfo_freq);
        scoreCQtotalMicros += micros() - start;
        
        o["report1"] = q.report1;
        o["report2"] = q.report2;
        o["cq"] = q.cq;

        o["firstSeen"] = q.firstSeen;
        o["lastHeard"] = q.lastHeard;
        o["duration"] = (q.lastHeard > q.firstSeen) ? (q.lastHeard - q.firstSeen) : 0;

        o["state"] = stateToString(q.state);
        o["completed"] = (q.state == QSO_DONE);

        // o["reply"] = q.reply;
        o["cared"] = q.cared;

        o["isMine"] = q.is_mine;
        o["qso_id"] = q.qso_id;
        // ------------------------------------------------------------
        // LOG (debug history)
        // ------------------------------------------------------------
        unsigned long logStart = micros();
        JsonArray logArr = o["log"].to<JsonArray>();

        for (uint8_t i = 0; i < q.logCount; i++) {
            JsonObject le = logArr.add<JsonObject>();

            le["ts"] = q.log[i].timestamp;
            le["state"] = stateToString(q.log[i].state);
            le["msg"] = q.log[i].msg;
            le["rtx"] = q.log[i].rtx;
            // vTaskDelay(1); // yield to avoid watchdog on long lists
        }
        scoreTotalLogUs += micros() - logStart;
        // ------------------------------------------------------------
        // QUEUE (TX messages to be transmitted for this QSO, including retries)
        // ------------------------------------------------------------
        JsonArray queueArr = o["tx_queue"].to<JsonArray>();

        for (FT8_TX::TxJob t : txJobs) {

            if(t.qso_id != q.qso_id || t.cancelled) continue;
            
            JsonObject te = queueArr.add<JsonObject>();

            te["ts"] = t.targetSlot;
            te["msg"] = t.message;
            te["retries"] = t.retryCount;
            te["transmitting"] = t.transmitting;
            // vTaskDelay(1); // yield to avoid watchdog on long lists
        }

        uint8_t retries = ft8tx.getRetriesforQso(q.qso_id);
        if(retries > 0){
            o["retryCount"] = retries;
            
        } else {
            o["nextRetry"] = nullptr;
            o["retryCount"] = 0;
            o["retryMessage"] = "";
        }
        //vTaskDelay(1); // yield to avoid watchdog on long lists
    }

    String out;
    out.reserve(qso_list.size() * 200);
    serializeJson(doc, out);

    // Serial.printf("Serialized %d QSOs to JSON in %lu ms\r\n", qso_list.size(), millis() - start);   
    // Serial.printf("Total scoreCQ computation time: %lu microseconds\r\n", scoreCQtotalMicros );
    // Serial.printf("Total log processing time: %lu microseconds\r\n", scoreTotalLogUs );
    return out;
}

bool isRegion(const char *t) {
    return strcmp(t, "EU") == 0 ||
           strcmp(t, "NA") == 0 ||
           strcmp(t, "SA") == 0 ||
           strcmp(t, "AS") == 0 ||
           strcmp(t, "AF") == 0 ||
           strcmp(t, "OC") == 0 ||
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

    // Serial.println("Processing message: " + String(msg) + " type=" + String(type) + " call1=" + String(f.call1) + " call2=" + String(f.call2) + " grid=" + String(f.grid) + " report=" + String(f.report) + " snr_db=" + String(s.snr_db));

    // store the decoded fields in the ft8_frequency_optimizer for frequency optimization (CQ density tracking)
    // Serial.println("Storing spot in ft8FreqOptimizer: freq=" + String(s.freq_hz) + " snr_db=" + String(s.snr_db) + " ts=" + String(f.ts) + " is_cq=" + String(f.is_cq) + " vfo_freq=" + String(ui_get_vfo_freq()));
    ft8FreqOptimizer.store(s.freq_hz, s.snr_db, f.ts, f.is_cq, ui_get_vfo_freq());

    // Prepare and send PSKReporter spot for this message
    Ft8Spot psk_spot = s;
    if(f.type <= MSG_CQ_TEST){
        strlcpy(psk_spot.callsign, f.call1, sizeof(psk_spot.callsign));
        if (f.hasGrid) {
            strlcpy(psk_spot.grid, f.grid, sizeof(psk_spot.grid));
        }
    } else {
        strlcpy(psk_spot.callsign, f.call2, sizeof(psk_spot.callsign));
        if (f.hasGrid) {
            strlcpy(psk_spot.grid, f.grid, sizeof(psk_spot.grid));
        }
    }   
    
    if (f.hasReport) {
        strlcpy(psk_spot.report, f.report, sizeof(psk_spot.report));
    }
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
    
    // add this message to the QSO log if it is relevant to me (either a CQ or a message directed to me)
    // Serial.println("Checking if message is relevant to me or its is a CQ, for logging: " + String(msg) + " call1=" + String(f.call1) + " call2=" + String(f.call2) + " type=" + String(type) + " QSO state is " + String(q->state));
    if(strcmp(f.call1, ui_get_mycall()) == 0 || (f.hasCall2 && strcmp(f.call2, ui_get_mycall()) == 0)){
        
        time_t timestamp;
        time(&timestamp);
    
        // Serial.printf("Adding log entry to qso_id: %d for message involving me: %s call1=%s call2=%s type=%d QSO state is %d. Generated reply: %s\r\n", q->qso_id, msg, f.call1, f.call2, type, q->state, reply.c_str());
        addLog(q, 'R', timestamp, q->state, msg);
        
    }else if(type <= MSG_CQ_TEST){
        
        time_t timestamp;
        time(&timestamp);
    
        // Serial.printf("Adding log entry to qso_id: %d for CQ message: %s call1=%s call2=%s type=%d QSO state is %d.\r\n", q->qso_id, msg, f.call1, f.call2, type, q->state);
        addLog(q, 'R', timestamp, q->state, msg);
    }
    
    // manage QSO_END
    if (q->state == QSO_DONE){
        if (!q->counted_in_stats) {
            int band = freqToBand(s.freq_hz);
            char bandname[5];
            strcpy(bandname, BandNames[band]);
            char mode[4] = "FT8";
            if(strcmp(q->call1, ui_get_mycall()) == 0) {
                qsoStats.onQSOCompleted(q->call2, s.freq_hz, f.ts);
                adif.enqueue(*q, s.freq_hz, bandname, mode);
            } else if (strcmp(q->call2, ui_get_mycall()) == 0) {
                qsoStats.onQSOCompleted(q->call1, s.freq_hz, f.ts);
                adif.enqueue(*q, s.freq_hz, bandname, mode);
            }
            q->counted_in_stats = true;
        }    
    }    

    // if the message is directed to me
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
    if (qso_list.size() >= QSO_LIST_MAX) {

        bool removed = false;
        // Serial.printf("QSO list size is %d, exceeded max size, removing oldest non-mine QSO if any...\r\n", qso_list.size());
        for (auto it = qso_list.begin(); it != qso_list.end(); ++it) {
            if (!it->is_mine) {
                // Serial.printf("Removing oldest non-mine QSO %d\n", it->qso_id);
                qso_list.erase(it);
                removed = true;
                break;
            }
        }

        // fallback: if all are "mine", remove oldest anyway
        if (!removed) {
            // Serial.printf("All QSOs are mine, removing oldest QSO %d...\r\n", qso_list.front().qso_id);
            qso_list.erase(qso_list.begin());
        }
    }
}

QSO* QSOManager::getQSOByFields(Ft8Fields &f) {

    if (!f.hasCall1 || !f.call1 || strcmp(f.call1, "") == 0) {
        Serial.println("getQSOByFields: call1 is missing or empty, cannot find matching QSO");
        return nullptr;
    }
   
    const char* a = f.call1;
    const char* b = (f.hasCall2 && f.call2) ? f.call2 : ""; // normalize call2

    for (auto &q : qso_list) {

        if (!q.call1) continue;
        const char* qc1 = q.call1;

        const char* qc2 = (q.call2 && strlen(q.call2) > 0) ? q.call2 : ""; // normalize call2

        if(f.type <= MSG_CQ_TEST){
             // For CQ messages we want to match on call1 only, and ignore call2 (it can be empty or not, doesn't matter)
            if (strcmp(qc1, a) == 0 && q.state == QSO_CQ) {
                return &q;
            }
        }else if(f.type == MSG_CALL && q.state == QSO_CQ){
            // for CALLING messages and qso is in state CQ we compare only call1
            if (strcmp(qc1, a) == 0) {
                return &q;
            }
        }
       
        else {
             // both call1 and call2 should be valid, but in any order (bidirectional match))
            if ((strcmp(qc1, a) == 0 && strcmp(qc2, b) == 0) ||  // qc2 and b can be empty
                (strcmp(qc1, b) == 0 && strcmp(qc2, a) == 0)) {
                return &q;
            }
        }
    }
    // Serial.printf("No existing QSO found matching call1=%s call2=%s\r\n", f.call1, f.hasCall2 ? f.call2 : "N/A");
    return nullptr;
}

// QSO* QSOManager::getQSOByFields(Ft8Fields &f) {

//     if (!f.hasCall1 || !f.call1 || strcmp(f.call1, "") == 0) {
//         Serial.println("getQSOByFields: call1 is missing or empty, cannot find matching QSO");
//         return nullptr;
//     }
//     const char* a = f.call1;
//     const char* b = (f.hasCall2 && f.call2) ? f.call2 : "";

//     for (auto &q : qso_list) {

//         if (!q.call1) continue;
//         const char* qc1 = q.call1;

//         const char* qc2 = q.call2 ? q.call2 : "";

//         if (strlen(qc2) > 0 && f.hasCall2 && f.call2) {
//             // complete QSO → bidirectional match
//             if ((strcmp(qc1, a) == 0 && strcmp(qc2, b) == 0) ||
//                 (strcmp(qc1, b) == 0 && strcmp(qc2, a) == 0)) {
//                 return &q;
//             }
//         } else {
//             // incomplete QSO → match on call1
//             if (strcmp(qc1, a) == 0 &&
//             (strlen(qc2) == 0 || !f.hasCall2 || (f.call2 && strcmp(qc2, b) == 0))) {
//                 return &q;
//             }
//         }
//     }
//     Serial.printf("No existing QSO found matching call1=%s call2=%s\r\n", f.call1, f.hasCall2 ? f.call2 : "N/A");
//     return nullptr;
// }

QSO* QSOManager::getQsoById(int qso_id) {

    for (auto &q : qso_list) {

        if(q.qso_id == qso_id)
            return &q;
    }
    return nullptr;
      
}


QSO * QSOManager::addOrUpdate(Ft8MsgType type, Ft8Fields &f, uint32_t timestamp, int8_t snr_db){ //, const String &reply) {

    if (!f.hasCall1) return nullptr; // safety check: call1 is mandatory for all message types we handle

    QSO *qso = getQSOByFields(f);

    // --------------------------------------------------------
    // CREATE if needed (ANY message, not only CQ)
    // --------------------------------------------------------
    if (!qso ||  // if there's still no QSO
                 // or the QSO exist and it's a CQ message and the QSO is completed, we create a new QSO
        (type <= MSG_CQ_TEST && qso->state == QSO_DONE ) 
        ) {
        
        // if(!qso){
        //     Serial.printf("No existing QSO found for message, creating new QSO for call1=%s call2=%s...\n", f.call1, f.hasCall2 ? f.call2 : "N/A");
        // } else if (qso && type <= MSG_CQ_TEST && qso->state == QSO_DONE){
        //     Serial.printf("Existing QSO %d is completed, but received a new CQ message, creating new QSO for call1=%s call2=%s...\n", qso->qso_id, f.call1, f.hasCall2 ? f.call2 : "N/A");
        // }
        

        static int next_qso_id = 0;    

        QSO q{};
        safeCopy(q.call1, f.call1, sizeof(q.call1));
        q.dxcc1 = qsoStats.dxccFromCall(f.call1);

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
        }else{
            safeCopy(qso->call2, f.call1, sizeof(qso->call2));
            qso->dxcc2 = qsoStats.dxccFromCall(f.call1);
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