#include <Arduino.h>
#include <ArduinoJson.h>
#include <map>
#include "wifi_config.h"
#include <vector>

class QSOManager {
public:
    QSOManager(const String &myCall_, const String &myGrid_) 
        : myCall(myCall_), myGrid(myGrid_) {}

    struct QSO {
        char call1[12];
        char call2[12];
        char grid1[8];
        char grid2[8];
        int snr1;
        int snr2;
        char mode[4];
        uint32_t firstSeen;   // Unix time
        uint32_t lastHeard;   // Unix time
        char report1[12];
        char report2[12];
        bool completed;
        bool cq;  // true if last message was a CQ
    };

    std::map<String, String> gridCache;  // CALL → GRID

    // Add or update QSOs from Spot
void addOrUpdate(const Ft8Spot &s) {

    const char * caller   = s.callsign;          // who transmitted
    const char * receiver = s.receiver_callsign; // who it was directed to

    if (strlen(caller) == 0)
        return;

    QSO *qso = nullptr;

    // -------------------------
    // 1️⃣ Look for an existing QSO
    // -------------------------
    Serial.printf("Processing spot: %s -> %s  s.grid=%s  qso_list has %d entries\n", caller, receiver, s.grid, (int)qso_list.size());
    for (auto &q : qso_list) {
        if ( ((strcmp(q.call1, caller) == 0  ) && (strlen(q.call1) > 0) && (strlen(q.call2) == 0)) ||                       // call2 is empty.                 caller is call1 
             ((strcmp(q.call2, caller) == 0  ) && (strlen(q.call2) > 0) && (strlen(q.call1) == 0)) ||                       // call1 is empty. It shouldn't happen but just in case. caller is call2
             ((strcmp(q.call1, receiver) == 0) && (strlen(q.call1) > 0) && (strlen(q.call2)  > 0) && (strcmp(q.call2, caller) == 0)) ||  // call1 and call2 are note empty. receiver is call1
             ((strcmp(q.call2, receiver) == 0) && (strlen(q.call2) > 0) && (strlen(q.call1)  > 0) && (strcmp(q.call1, caller) == 0)) ||  // call1 and call2 are note empty. receiver is call2
             ((strcmp(q.call1, receiver) == 0) && (strlen(q.call1) > 0) && (strlen(q.call2) == 0)) ||  // call2 is empty.                 receiver is call1  
             ((strcmp(q.call2, receiver) == 0) && (strlen(q.call2) > 0) && (strlen(q.call1) == 0))                          // call1 is empty. It shouldn't happen but just in case. receiver is call2
        
        ) {
            qso = &q;
            //Serial.printf("caller:%s receiver:%s  Existing QSO found call1:%s call2:%s\n", caller, receiver, q.call1.c_str(), q.call2.c_str());
            break;
        }
    }

    if(!qso) {
        Serial.printf("No existing QSO found for %s   cq=%d\n", caller, s.cq);
    }
    // -------------------------
    // 2️⃣ Create new QSO if CQ
    // -------------------------
    if (!qso && s.cq) {
        QSO q;
        strcpy(q.call1, caller);
        strcpy(q.grid1, s.grid);
        strcpy(q.call2, "");      // not known yet
        strcpy(q.grid2, "");
        q.snr1 = s.snr_db;
        q.snr2 = 0;
        strcpy(q.report1, "");
        strcpy(q.report2, "");
        strcpy(q.mode, s.mode);
        q.firstSeen = s.timestamp;
        q.lastHeard = s.timestamp;
        q.completed = false;
        q.cq = s.cq;
 
        qso_list.push_back(q);
        // Keep memory bounded (VERY important)
        if (qso_list.size() > 50)
            qso_list.erase(qso_list.begin());

        qso = &qso_list.back();

        Serial.printf("CQ seen from %s (%s)\n", q.call1, q.grid1);
        return;
    }

    // -------------------------
    // 3️⃣ If QSO exists, update it
    // -------------------------
    if (!qso)
        return; // ignore unrelated message

    qso->lastHeard = s.timestamp;
    qso->cq = s.cq;
    // -------------------------
    // 4️⃣ Assign call2 if missing and receiver==call1
    // -------------------------
    if (strlen(qso->call2) == 0 && strcmp(receiver,qso->call1) == 0) {
        Serial.printf("QSO to be updated: call1=%s call2=%s  grid1=%s grid2=%s s.grid=%s\n", 
            qso->call1, qso->call2, qso->grid1, qso->grid2, s.grid);
 
        strcpy(qso->call2, caller);
        if(strlen(qso->grid2) == 0 && strlen(s.grid) > 0) strcpy(qso->grid2, s.grid);
        if(qso->snr2 == 0 && s.snr_db != 0) qso->snr2 = s.snr_db;
        Serial.printf("QSO updated: call1=%s call2=%s  grid1=%s grid2=%s\n", 
            qso->call1, qso->call2, qso->grid1, qso->grid2);
    }

    // -------------------------
    // 5️⃣ Update SNRs
    // -------------------------
    if(s.snr_db != 0) {
        if (strcmp(caller, qso->call1) == 0) qso->snr1 = s.snr_db;
        if (strcmp(caller, qso->call2) == 0) qso->snr2 = s.snr_db;
    }    
    
    // -------------------------
    // 8️⃣ Completion
    // -------------------------
    if ((strcmp(s.report, "73") == 0) || (strcmp(s.report, "RR73") == 0)) {
        qso->completed = true;
        Serial.printf("QSO completed: %s <-> %s\n", qso->call1, qso->call2);
        return;
    }

    // -------------------------
    // 7️⃣ Update reports
    // -------------------------
    if (strlen(s.report) > 0) {
        if (strcmp(caller, qso->call1) == 0 && strlen(qso->report2) == 0) strcpy(qso->report2, s.report);
        if (strcmp(caller, qso->call2) == 0 && strlen(qso->report1) == 0) strcpy(qso->report1, s.report);
    }

}


private:
    String myCall;
    String myGrid;
    std::vector<QSO> qso_list;

#include <ArduinoJson.h>

    String qsoListToJson(const std::vector<QSO> &list) {
        JsonDocument doc;                 // v7: no StaticJsonDocument
        JsonArray arr = doc.to<JsonArray>();

        for (const auto &q : list) {
            JsonObject o = arr.add<JsonObject>();   // v7 way to create nested object

            o["call1"]  = q.call1;
            o["grid1"]  = q.grid1;
            o["call2"]     = q.call2;
            o["grid2"]     = q.grid2;
            o["snr1"]        = q.snr1;
            o["snr2"]        = q.snr2;
            o["mode"]       = q.mode;
            o["firstSeen"]  = q.firstSeen;
            o["lastHeard"]  = q.lastHeard;
            o["report1"]     = q.report1;
            o["report2"]     = q.report2;
            o["completed"]  = q.completed;
            o["cq"]         = q.cq;
        }

        String out;
        serializeJson(doc, out);
        return out;
    }

    String filterByCompletion(bool completed) {
        std::vector<QSO> filtered;
        for (auto &q : qso_list) if (q.completed == completed) filtered.push_back(q);
        return qsoListToJson(filtered);
    }

public:
    String getCompletedQSOsJson() {
        return filterByCompletion(true);
    }   

    String getPendingQSOsJson() {
        return filterByCompletion(false);
    }       

   String getAllQSOsJson() {
        return qsoListToJson(qso_list);
    }       

    String toJson() {
        return getAllQSOsJson();
    }
    
    String getActiveQSOsJson() {
        return getPendingQSOsJson();
    }
    

};
