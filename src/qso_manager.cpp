#include "qso_manager.h"


QSOManager::QSOManager() {}
// ------------------------------------------------------------
// Helpers
// ------------------------------------------------------------
void QSOManager::safeCopy(char* dst, const char* src, size_t size) {
    if (!dst || !src || size == 0) return;
    strncpy(dst, src, size - 1);
    dst[size - 1] = '\0';
}

bool QSOManager::isCallsign(const char *s) {
    if (!s) return false;
    size_t len = strlen(s);
    if (len < 3 || len > 12) return false;

    bool hasLetter = false, hasDigit = false;

    for (size_t i = 0; i < len; i++) {
        if (!isalnum(s[i]) && s[i] != '/') return false;
        if (isalpha(s[i])) hasLetter = true;
        if (isdigit(s[i])) hasDigit = true;
    }
    return hasLetter && hasDigit;
}

bool QSOManager::isGrid(const char *s) {
    if (!s) return false;
    return strlen(s) == 4 &&
           isalpha(s[0]) && isalpha(s[1]) &&
           isdigit(s[2]) && isdigit(s[3]);
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

    for (const auto &q : qso_list) {
        JsonObject o = arr.add<JsonObject>();

        o["call1"] = q.call1;
        o["call2"] = q.call2;
        o["grid1"] = q.grid1;
        o["grid2"] = q.grid2;
        o["snr1"]  = q.snr1;
        o["snr2"]  = q.snr2;

        o["report1"] = q.report1;
        o["report2"] = q.report2;
        o["cq"] = q.cq;

        o["firstSeen"] = q.firstSeen;
        o["lastHeard"] = q.lastHeard;
        o["duration"] = (q.lastHeard > q.firstSeen) ? (q.lastHeard - q.firstSeen) : 0;

        o["state"] = stateToString(q.state);
        o["completed"] = (q.state == QSO_DONE);

        o["reply"] = q.reply;
        o["cared"] = q.cared;

        o["isMine"] = q.is_mine;
        o["qso_id"] = q.qso_id;

        // ------------------------------------------------------------
        // LOG (debug history)
        // ------------------------------------------------------------
        JsonArray logArr = o["log"].to<JsonArray>();

        for (uint8_t i = 0; i < q.logCount; i++) {
            JsonObject le = logArr.add<JsonObject>();

            le["ts"] = q.log[i].timestamp;
            le["state"] = stateToString(q.log[i].state);
            le["msg"] = q.log[i].msg;
            le["rtx"] = q.log[i].rtx;
        }

        uint8_t retries = ft8tx.getRetriesforQso(q.qso_id);
        // RetryEntry *retry = getRetryByQsoId(q.qso_id);
        if(retries > 0){
            //o["nextRetry"] = retry->nextTxSlot;
            o["retryCount"] = retries;
            //o["retryMessage"] = retry->message;
        } else {
            o["nextRetry"] = nullptr;
            o["retryCount"] = 0;
            o["retryMessage"] = "";
        }
    }

    String out;
    serializeJson(doc, out);
    return out;
}

// ------------------------------------------------------------
// Robust message parser (NO fragile substring matching)
// ------------------------------------------------------------
QSOManager::Ft8MsgType QSOManager::parseMessage(const char *msg, QSOManager::Ft8Fields &out) {
    if (!msg) return MSG_UNKNOWN;

    // Reset
    memset(&out, 0, sizeof(out));
    out.type = MSG_UNKNOWN;

    char tmp[64];
    strncpy(tmp, msg, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';

    char *tokens[5] = {0};
    int ntok = 0;

    // Tokenize
    char *p = strtok(tmp, " ");
    while (p && ntok < 5) {
        tokens[ntok++] = p;
        p = strtok(nullptr, " ");
    }

    if (ntok == 0) return MSG_UNKNOWN;

    // --------------------------------------------------------
    // CQ
    // --------------------------------------------------------
    if (strcmp(tokens[0], "CQ") == 0) {

        if (ntok >= 2 && isCallsign(tokens[1])) {
            safeCopy(out.call1, tokens[1], sizeof(out.call1));
            out.hasCall1 = true;
        }

        // CQ CALL GRID
        if (ntok >= 3 && isGrid(tokens[2])) {
            safeCopy(out.grid, tokens[2], sizeof(out.grid));
            out.hasGrid = true;

            out.type = MSG_CQ;
            return out.type;
        }

        // CQ CALL  (no grid)
        out.type = MSG_CQ_NO_GRID;
        return out.type;
    }
    // --------------------------------------------------------
    // CALL1 CALL2 ...
    // --------------------------------------------------------
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
    // --------------------------------------------------------
    // Third token (type discriminator)
    // --------------------------------------------------------
    if (ntok >= 3) {

        // 73 / RR73
        if (strcmp(tokens[2], "RR73") == 0) {
            out.type = MSG_RR73;
            return out.type;
        }

        if (strcmp(tokens[2], "73") == 0) {
            out.type = MSG_73;
            return out.type;
        }

        // Report
        if (isReport(tokens[2])) {
            safeCopy(out.report, tokens[2], sizeof(out.report));
            out.hasReport = true;

            if (tokens[2][0] == 'R') {
                out.type = MSG_R_REPORT;
            } else {
                out.type = MSG_REPORT;
            }
            return out.type;
        }

        // Grid (CALL message with locator)
        if (isGrid(tokens[2])) {
            safeCopy(out.grid, tokens[2], sizeof(out.grid));
            out.hasGrid = true;
        }
    }

    // --------------------------------------------------------
    // Default
    // --------------------------------------------------------
    
    if (out.hasCall1 && out.hasCall2) {
        if (out.hasGrid) {
            out.type = MSG_CALL;              // CALL1 CALL2 GRID
        } else {
            out.type = MSG_CALL_NO_GRID;      // CALL1 CALL2
        }
    } else {
        out.type = MSG_UNKNOWN;
    }


    return out.type;
}

String QSOManager::generateReply(Ft8MsgType type, const Ft8Fields &f, int snr_db) {

    String myCall = String(ui_get_mycall());
    String myGrid = String(ui_get_mygrid());

    // Safety: no callsign configured
    if (myCall.isEmpty()) return "";

    String call1 = String(f.call1);
    String call2 = String(f.call2);

    
    // --------------------------------------------------------
    // Determine if message is relevant to me
    // --------------------------------------------------------
    bool isCQ = (type == MSG_CQ || type == MSG_CQ_NO_GRID);

    bool directedToMe = (call1 == myCall);

    if (!isCQ && !directedToMe) {
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
    switch (type) {

        case MSG_CQ:
            // CQ CALL GRID → CALL MYCALL GRID
            reply = call1 + " " + myCall + " " + myGrid;
            break;

        case MSG_CQ_NO_GRID:
            // CQ CALL → CALL MYCALL
            reply = call1 + " " + myCall;
            break;

        case MSG_CALL:
        case MSG_CALL_NO_GRID:
            // MYCALL CALL → CALL MYCALL -RPT
            reply = call2 + " " + myCall + " " + String(rpt);
            break;

        case MSG_REPORT:
            // MYCALL CALL -RPT → CALL MYCALL R-RPT
            reply = call2 + " " + myCall + " R" + String(rpt);
            break;

        case MSG_R_REPORT:
            // MYCALL CALL R-RPT → CALL MYCALL RR73
            reply = call2 + " " + myCall + " RR73";
            break;


        case MSG_RR73:
            // MYCALL CALL RR73 → CALL MYCALL 73
            reply = call2 + " " + myCall + " 73";
            break;

        default:
            reply = "";
            
    }
    if(reply.length()){
        //Serial.println("From call1=" + String(call1) + " call2=" + String(call2) + " type=" + String(f.type) + " Generated reply: " + reply);
        return reply;
    } else
        return "";

}

// ------------------------------------------------------------
// Main received messages processor
// ------------------------------------------------------------
void QSOManager::processFt8Spot(const Ft8Spot &s) {
    const char* msg = extractFt8Message(s.decoded_line); // get the message part after '~' if present, otherwise the whole line

    Ft8Fields f;

    Ft8MsgType type = parseMessage(msg, f);              // parse the message and extract fields

    // Discard my transmissions 
    if(strcmp(f.call2, ui_get_mycall()) == 0){
        Serial.println("Decoded my own transmission. Discard it.");
        return; 
    }

    // Discard unknown messages
    if (type == MSG_UNKNOWN){ 
        Serial.println("Decoded message " + String(msg) + " with unknown type: " + String(type) + ". Ignoring it.");
        return;
    };

    // generate the appropriate reply message based on the message type and extracted fields
    String reply = "";  
    if(type == MSG_CQ || type == MSG_CQ_NO_GRID || strcmp(f.call1, ui_get_mycall()) == 0 ){
        reply = generateReply(type, f, s.snr_db);     
        // Serial.println("Decoded message: " + String(msg) + " type=" + String(type) + " call1=" + String(f.call1) + " call2=" + String(f.call2) + " grid=" + String(f.grid) + " report=" + String(f.report) + ". Generated reply: " + reply);
    }

    // add this received message to the QSO list
    QSO * q = addOrUpdate(type, f, s.timestamp, s.snr_db, reply);  // add/update the QSO list based on the message type, extracted fields, timestamp and SNR
    if(q == nullptr){
        // Serial.println("Failed to add/update QSO for message: " + String(msg));
        return;
    }  
    
    // add this message to the QSO log if it is relevant to me (either a CQ or a message directed to me)
    if(strcmp(f.call1, ui_get_mycall()) == 0 || (f.hasCall2 && strcmp(f.call2, ui_get_mycall()) == 0)){
        
        time_t timestamp;
        time(&timestamp);
    
        Serial.printf("Adding log entry to qso_id: %d for message involving me: %s call1=%s call2=%s type=%d QSO state is %d. Generated reply: %s\r\n", q->qso_id, msg, f.call1, f.call2, type, q->state, reply.c_str());
        addLog(q, 'R', timestamp, q->state, msg);
    }else if(type == MSG_CQ || type == MSG_CQ_NO_GRID){
        
        time_t timestamp;
        time(&timestamp);
    
        // Serial.printf("Adding log entry to qso_id: %d for CQ message: %s call1=%s call2=%s type=%d QSO state is %d. Generated reply: %s\r\n", q->qso_id, msg, f.call1, f.call2, type, q->state, reply.c_str());
        addLog(q, 'R', timestamp, q->state, msg);
    }
    
    if( (type == MSG_CQ || type == MSG_CQ_NO_GRID) && strlen(q->reply) == 0 ){
        Serial.println("addOrUpdate output without reply: " + String(msg) + " q.firstseen=" + String(q->firstSeen) + " q.lastHeard=" + String(q->lastHeard) + " cared=" + String(q->cared) + " call1=" + String(q->call1) + " call2=" + String(q->call2)  + ". QSO state is " + String(q->state) + ". Generated reply: " + q->reply);   
    }    

    // cancel any pending retry for this QSO if I am involved as a callee 
    if(strcmp(f.call1, ui_get_mycall()) == 0){ 

        //cancelRetry(q->qso_id);
        ft8tx.cancelJobsForQso(q->qso_id);
    }

    // if I am call1, the other station is call2, so reply to call2
    if(reply != "" && strcmp(ui_get_mycall(),f.call1)==0){ 
        Serial.println("with msg " + String(msg) + " I decoded f.call1=" + String(f.call1) + " f.call2=" + String(f.call2) + "QSO state is " + String(q->state) + ". Generated reply: " + reply);
        
        uint8_t theirParity = ((q->lastHeard / 15) % 2);
        uint8_t myParity = (theirParity == 0) ? 1 : 0;

        Serial.printf("Scheduling FT8 reply to QSO %d at %d Hz: %s theirParity=%d myParity=%d\r\n", 
            q->qso_id, ui_get_vfo_freq() + ui_get_ft8_offset(), q->reply, theirParity, myParity);
        ft8tx.requestTransmission(ui_get_vfo_freq() + ui_get_ft8_offset(), q->reply, myParity, q->qso_id);
    }
}

QSOManager::QSO* QSOManager::getQSOByFields(Ft8Fields &f) {

    if (!f.hasCall1 || !f.call1) return nullptr;

    const char* a = f.call1;
    const char* b = (f.hasCall2 && f.call2) ? f.call2 : "";

    for (auto &q : qso_list) {

        if (!q.call1) continue;
        const char* qc1 = q.call1;

        const char* qc2 = q.call2 ? q.call2 : "";

        if (strlen(qc2) > 0 && f.hasCall2 && f.call2) {
            // complete QSO → bidirectional match
            if ((strcmp(qc1, a) == 0 && strcmp(qc2, b) == 0) ||
                (strcmp(qc1, b) == 0 && strcmp(qc2, a) == 0)) {
                return &q;
            }
        } else {
            // incomplete QSO → match on call1
            if (strcmp(qc1, a) == 0 &&
            (strlen(qc2) == 0 || !f.hasCall2 || (f.call2 && strcmp(qc2, b) == 0))) {
                return &q;
            }
        }
    }

    return nullptr;
}

QSOManager::QSO * QSOManager::addOrUpdate(Ft8MsgType type, Ft8Fields &f, uint32_t timestamp, int8_t snr_db, const String &reply) {

    if (!f.hasCall1) return nullptr; // safety check: call1 is mandatory for all message types we handle

    QSO *qso = getQSOByFields(f);

    // --------------------------------------------------------
    // CREATE if needed (ANY message, not only CQ)
    // --------------------------------------------------------
    if (!qso) {

        static int next_qso_id = 0;    

        QSO q{};
        safeCopy(q.call1, f.call1, sizeof(q.call1));

        if (f.hasCall2)
            safeCopy(q.call2, f.call2, sizeof(q.call2));

        q.report1[0] = '\0';
        q.report2[0] = '\0';

        q.snr1 = INT8_MIN;
        q.snr2 = INT8_MIN;

        q.firstSeen = timestamp;
        q.lastHeard = timestamp;

        q.cq = (type == MSG_CQ);
        q.state = (type == MSG_CQ || type == MSG_CQ_NO_GRID) ? QSO_CQ : QSO_CALLING;

        q.qso_id = next_qso_id++;

        
        switch(type) {
            case MSG_CQ:
            case MSG_CQ_NO_GRID:
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

    if (type == MSG_CQ || type == MSG_CQ_NO_GRID)
        qso->cq = true;

    // --------------------------------------------------------
    // COMPLETE CALL2 if missing
    // --------------------------------------------------------
    if (strlen(qso->call2) == 0 && f.hasCall2) {
        if (strcmp(f.call1, qso->call1) == 0)
            safeCopy(qso->call2, f.call2, sizeof(qso->call2));
        else
            safeCopy(qso->call2, f.call1, sizeof(qso->call2));
        // 🔴 as soon as call2 appears, the QSO is no longer CQ
        if (qso->state == QSO_CQ)
            qso->state = QSO_CALLING;
    }
    // --------------------------------------------------------
    // GRID (directional)
    // --------------------------------------------------------
    if (f.hasGrid){
        if(type==MSG_CQ || type==MSG_CQ_NO_GRID)
            safeCopy(qso->grid1, f.grid, sizeof(qso->grid1));
        else
            safeCopy(qso->grid2, f.grid, sizeof(qso->grid2));
    }    

    // --------------------------------------------------------
    // SNR (directional)
    // --------------------------------------------------------
    if (snr_db != INT8_MIN) {
        if(type==MSG_CQ || type==MSG_CQ_NO_GRID){
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
            safeCopy(qso->report2, f.report, sizeof(qso->report2));

        else if (strcmp(f.call1, qso->call2) == 0)
            safeCopy(qso->report1, f.report, sizeof(qso->report1));
    }

    // --------------------------------------------------------
    // STATE = DIRECT mapping from message
    // --------------------------------------------------------
    switch (type) {

        case MSG_CQ:
        case MSG_CQ_NO_GRID:
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

    // --------------------------------------------------------
    // REPLY (store the latest generated reply for this QSO, if any)
    // --------------------------------------------------------
    if(reply.length() > 0)
        safeCopy(qso->reply, reply.c_str(), sizeof(qso->reply));

    // reduce the list size if needed (remove oldest non-"mine" first)
    if (qso_list.size() >= 100) {

        bool removed = false;

        for (auto it = qso_list.begin(); it != qso_list.end(); ++it) {
            if (!it->is_mine) {
                qso_list.erase(it);
                removed = true;
                break;
            }
        }

        // fallback: if all are "mine", remove oldest anyway
        if (!removed) {
            qso_list.erase(qso_list.begin());
        }
    }


    return qso;

}
// ------------------------------------------------------------
// Retry mechanism for unanswered CQ calls
// ------------------------------------------------------------
// void QSOManager::scheduleRetry(int qso_id, const char* msg, uint32_t slot) {

//     if (!msg || !msg[0]) return; // nothing to schedule

//     // ------------------------------------------------------------
//     // Check if retry already exists for this QSO
//     // ------------------------------------------------------------
//     for (auto &r : retryList) {

//         if (r.qso_id == qso_id) {
//             // 🔁 Update existing entry (no duplicates)

//             r.nextTxSlot = slot;

//             // reset retry counter ONLY if message changed
//             if (strncmp(r.message, msg, sizeof(r.message)) != 0) {
//                 strncpy(r.message, msg, sizeof(r.message) - 1);
//                 r.message[sizeof(r.message) - 1] = '\0';
//                 r.retryCount = 0;
//             }

//             return;
//         }
//     }

//     // ------------------------------------------------------------
//     // Create new retry entry
//     // ------------------------------------------------------------
//     RetryEntry r;

//     r.qso_id = qso_id;
//     r.nextTxSlot = slot;
//     r.retryCount = 0;

//     strncpy(r.message, msg, sizeof(r.message) - 1);
//     r.message[sizeof(r.message) - 1] = '\0';

//     retryList.push_back(r);
// }

// QSOManager::RetryEntry* QSOManager::getNextRetry(uint32_t currentSlot) {

//     RetryEntry* best = nullptr;

//     for (auto &r : retryList) {

//         // ------------------------------------------------------------
//         // Skip if not yet time
//         // ------------------------------------------------------------
//         if (r.nextTxSlot > currentSlot) continue;

//         // ------------------------------------------------------------
//         // Pick the oldest due retry (fair scheduling)
//         // ------------------------------------------------------------
//         if (!best || r.nextTxSlot < best->nextTxSlot) {
//             best = &r;
//         }
//     }

//     return best;
// }

// QSOManager::RetryEntry* QSOManager::getRetryByQsoId(int qso_id)
// {
//     for (auto &r : retryList) {

//         if (r.qso_id == qso_id) {
//             return &r;
//         }
//     }

//     return nullptr;
// }

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
}