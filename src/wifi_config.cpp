#include "wifi_config.h"
#include "esp_wifi.h"
#include <Arduino.h>
#include <ETH.h>
#include <WebServer.h>
#include <WebSocketsServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <LittleFS.h>
#include "esp_heap_caps.h"
#include "ui.h"
#include <ArduinoJson.h>
#include <sys/time.h>
#include <time.h>
#include <vector>
#include <string>
#include <mutex>
#include <freertos/FreeRTOS.h>
#include "qso_manager.h"
#include "ft8_tx.h"
#include "secrets.h"
#include "ft8_freq_opt.h"
#include "qsostats.h"
#include "adif.h"
#include "task_profilers.h"

extern FT8_TX ft8tx; // declared in main.cpp
extern FT8FreqOptimizer ft8FreqOptimizer; // declared in main.cpp
extern QSOStats qsoStats; // declared in main.cpp 
extern std::vector<FT8_TX::TxJob> txJobs; // declared in ft8_tx.cpp
extern QueueHandle_t profilerMutex;

static WebServer server(80);
static WebSocketsServer wsServer(8765);  // Default port; will be updated from UI settings

fs::LittleFSFS LogsFS; // Separate LittleFS instance for logs to avoid wear on the main filesystem

#define CHUNK_SAMPLES 160
#define FRAME_SIZE (16 + CHUNK_SAMPLES*2) // header 16B + audio

#define WS_AUDIO_BUF_SAMPLES 512   // larger frame buffer for smoother websocket streaming

typedef struct {
    uint64_t ts_ms;
    int32_t  freq_hz;
    uint16_t n_samples;
    int16_t  samples[WS_AUDIO_BUF_SAMPLES];
} AudioFrame;

#define AUDIO_FRAME_POOL 8
static AudioFrame audioPool[AUDIO_FRAME_POOL];

volatile uint8_t audioPoolHead = 0;

// ---- Queue ----
QueueHandle_t audioQueue;      // filled audio frames
QueueHandle_t freeFrameQueue;  // free reusable frames
QueueHandle_t ft8Queue;        // text only, no pointers
QueueHandle_t adifQueue;       // ADIF upload items

#define ETH_PHY_TYPE ETH_PHY_W5500
#define ETH_PHY_ADDR 1
#define ETH_PHY_CS   1 // 15
#define ETH_PHY_IRQ  6 // wired to W5500 INT
#define ETH_PHY_RST  8 // 5

// SPI pins
#define ETH_SPI_SCK   18 // 14
#define ETH_SPI_MISO  7  // 12
#define ETH_SPI_MOSI  2  // 13

static bool timeConfigured = false;
static bool timeSynced = false;
static bool webServerStarted = false;
static bool wsServerStarted = false;
static uint32_t lastReconnectMs = 0;
static bool staConnecting = false;
static uint32_t staStartMs = 0;

// static std::vector<Ft8Spot> ft8_spots;

static String savedSsid;
static String savedPass;

// ==== Global WiFi status variables =======
uint8_t g_wifiBars        = 0;
int     g_wifiRssi        = -100;
bool    g_wifiConnected   = false;
bool    g_wifiReconnecting= false;
bool    g_wifiWarning     = false;

// ===== FT8 server status variables =====
bool     g_ft8ServerConnected = false;
bool     g_ft8ServerActive    = false;
uint32_t g_ft8LastRxMs        = 0;

static const uint8_t  FT8_MSGS_PER_LOOP_MAX = 6;
static const uint8_t  FT8_MSGS_WHEN_AUDIO_PENDING = 1;
static const uint8_t  AUDIO_FRAMES_PER_LOOP_MAX = 3;

QSOManager qsoManager;

// ===== Helpers =====

static void loadCredentials() {
    Preferences prefs;
    if (!prefs.begin("net", true)) 
        return;
    savedSsid = prefs.getString("ssid", "");
    savedPass = prefs.getString("pass", "");
    prefs.end();
}

static void saveCredentials(const String& ssid, const String& pass) {
    Preferences prefs;
    if (!prefs.begin("net", false)) return;
    prefs.putString("ssid", ssid);
    prefs.putString("pass", pass);
    prefs.end();
}

static uint64_t utc_ms() {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000ULL + tv.tv_usec / 1000;
}

static void setup_time_once() {
    if (!timeConfigured) {
        configTime(0, 0, "pool.ntp.org", "time.nist.gov");
        timeConfigured = true;
    }
    struct tm timeinfo;
    if (getLocalTime(&timeinfo, 5000)) timeSynced = true;
    printf("[Time] NTP time %s\n", timeSynced ? "synchronized" : "not synchronized");
    printf("[Time] Current time: %04d-%02d-%02d %02d:%02d:%02d UTC\n",
           timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday,
           timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
}

// ===== WebSocket Server Event Handler=====
#define MAX_FT8_MSG 512  // maximum length of an FT8 string

void webSocketEvent(uint8_t num, WStype_t type, uint8_t * payload, size_t length) {
  char msg[MAX_FT8_MSG];
  size_t len;
  
  switch(type) {
    case WStype_DISCONNECTED:
        Serial.printf("[WS] Client %u disconnected\n", num);
        break;
        
    case WStype_CONNECTED:
        Serial.printf("[WS] Client %u connected, IP: %s\n", num, wsServer.remoteIP(num).toString().c_str());
        g_ft8ServerConnected = true;
        break;
        
    case WStype_TEXT:
      len = min(length, (size_t)MAX_FT8_MSG-1);
      memcpy(msg, payload, len);
      msg[len] = 0;
      //Serial.printf("[FT8] Received from client %u: %s\n", num, msg);
      xQueueSend(ft8Queue, msg, 0);   // copy data
      break;
      
    case WStype_BIN:
      // Binary data from client (audio, etc.)
      Serial.printf("[WS] Binary data from client %u, %u bytes\n", num, length);
      break;
      
    case WStype_ERROR:
        Serial.printf("[WS] Error on client %u\n", num);
        break;
        
    case WStype_PONG:
        // Pong response
        break;
        
    default:
        break;
  }
}

// ===== HTTP Handlers =====

// static void handleApiStatus() {
//     JsonDocument doc;
//     doc["ip"] = WiFi.localIP().toString();
//     doc["mode"] = (WiFi.getMode() & WIFI_STA) ? "STA" : "AP";
//     //doc["ws_clients"] = wsServer.connectedClients();
//     doc["time_synced"] = timeSynced;
//     String out;
//     serializeJson(doc, out);
//     server.send(200, "application/json", out);
// }

static void handleApiUi() {
    UiSettings s;
    if (!ui_get_settings(&s)) {
        server.send(500, "application/json", "{\"error\":\"read failed\"}");
        return;
    }
    JsonDocument doc;
    doc["vfoA"] = s.vfoA; 
    doc["ft8_offset"] = s.ft8_offset;
    doc["ft8_offset_enabled"] = s.ft8_offset_enabled;
    doc["ft8_testmsg"] = s.ft8_testmsg;
    doc["ft8_mode"] = s.ft8_mode;
    doc["ft8_max_retries"] = s.ft8_max_retries;
    doc["ft8_send_parity"] = s.ft8_send_parity;
    doc["ws_server_host"] = s.ws_server_host;
    doc["ws_server_port"] = s.ws_server_port;
    doc["ws_server_enabled"] = s.ws_server_enabled;
    doc["mycall"] = s.mycall;
    doc["mygrid"] = s.mygrid;
    doc["myantenna"] = s.myantenna;
    doc["mysoftware"] = s.mysoftware;   
    doc["myrig"] = s.myrig; 
    doc["mode"] = s.mode; doc["bandval"] = s.bandval; doc["stepsize"] = s.stepsize;
    doc["volume"] = s.volume;
    doc["filt"] = s.filt; doc["agc"] = s.agc; doc["nr"] = s.nr; doc["att"] = s.att;
    doc["smode"] = s.smode;
    doc["backlight"] = s.backlight;
    doc["sifxtal"] = s.sifxtal; doc["iq_phase"] = s.iq_phase;
    doc["iq_balance"] = s.iq_balance; doc["iq_delay"] = s.iq_delay; doc["wf_thresh"] = s.wf_thresh;
    String out;
    serializeJson(doc, out);
    server.send(200, "application/json", out);
}

#include <ArduinoJson.h>

static void handleApiUiSave() {
    UiSettings s;

    if (!ui_get_settings(&s)) {
        server.send(500, "application/json", "{\"error\":\"read failed\"}");
        return;
    }

    JsonDocument doc;  // ← v7: no size needed

    DeserializationError err = deserializeJson(doc, server.arg("plain"));
    if (err) {
        server.send(400, "application/json", "{\"error\":\"bad json\"}");
        return;
    }

    // In v7: check existence with isNull()
    #define JSET(name) if (!doc[#name].isNull()) s.name = doc[#name];
    // Safely copy the string from the JSON document
    #define JSET_STRING(name) strlcpy(s.name, doc[#name] | "", sizeof(s.name));
    JSET(vfoA);
    JSET(ft8_offset); JSET(ft8_offset_enabled); JSET_STRING(ft8_testmsg); 
    JSET(ft8_mode); JSET(ft8_max_retries); JSET(ft8_send_parity);
    JSET_STRING(ws_server_host);
    JSET(ws_server_port); 
    JSET(ws_server_enabled);
    JSET_STRING(mycall); JSET_STRING(mygrid); 
    JSET_STRING(myantenna); JSET_STRING(mysoftware); JSET_STRING(myrig);
    JSET(mode); JSET(bandval);
    JSET(stepsize); 
    JSET(volume);
    JSET(filt); JSET(agc); JSET(nr); JSET(att); JSET(smode);
    JSET(backlight);
    JSET(sifxtal); JSET(iq_phase); JSET(iq_balance); JSET(iq_delay);
    JSET(wf_thresh);

    #undef JSET

    ui_apply_settings(s);

    server.send(200, "application/json", "{\"ok\":true}");
}

static void handleApiWifiSave() {
    JsonDocument doc;
    deserializeJson(doc, server.arg("plain"));
    String ssid = doc["ssid"] | "";
    String pass = doc["pass"] | "";
    if (!ssid.length()) {
        server.send(400, "application/json", "{\"error\":\"ssid required\"}");
        return;
    }
    saveCredentials(ssid, pass);
    server.send(200, "application/json", "{\"ok\":true,\"reboot\":true}");
    delay(500);
    ESP.restart();
}

// Start FT8 decoding
static void handleFt8Start() {
    ft8_running = true;
    JsonDocument doc;
    deserializeJson(doc, server.arg("plain"));
    int freq = doc["freq"] | 0;
    String msg = doc["msg"] | "";

    ft8tx.startContinuousTransmission(freq, msg.c_str()); // Example: start continuous transmission of a test message on 40m
    server.send(200, "application/json", "{\"message\":\"Started\"}");
}

// Stop FT8 decoding
static void handleFt8Stop() {
    ft8_running = false;
    ft8tx.stopContinuousTransmission();

    server.send(200, "application/json", "{\"message\":\"Stopped\"}");
}


// Get decoded spots
// static void handleFt8Spots() {
//     JsonDocument doc;
//     JsonArray arr = doc.to<JsonArray>();  
//     //std::lock_guard<std::mutex> lock(ft8_mutex);
//     int idx = 0;
//     for(auto &s : ft8_spots){
//         JsonObject o = arr.add<JsonObject>();
//         o["decoded_line"] = s.decoded_line;
//         o["callsign"] = s.callsign;
//         o["grid"] = s.grid;
//         o["receiver_callsign"] = s.receiver_callsign;
//         o["receiver_grid"] = s.receiver_grid;
//         o["snr"] = s.snr_db; 
//         o["mode"] = s.mode;
//         o["time"] = s.timestamp;
//         o["freq"] = s.freq_hz;
//         o["report"] = s.report;
//         o["cq"] = s.cq;
//         o["directed_to_me"] = s.directed_to_me;
//         idx++;
//     }
//     String out;
//     serializeJson(doc, out);
//     server.send(200, "application/json", out);
// }

static void handleApiStatus() {
    JsonDocument doc;

    const bool ethUp = ETH.linkUp() && (ETH.localIP() != IPAddress(0,0,0,0));
    IPAddress ip = ethUp ? ETH.localIP() : WiFi.localIP();

    doc["ip"] = ip.toString();
    doc["transport"] = ethUp ? "ETH" : "WIFI";
    doc["mode"] = (WiFi.getMode() & WIFI_STA) ? "STA" : "AP";
    doc["time_synced"] = timeSynced;

    String out;
    serializeJson(doc, out);
    server.send(200, "application/json", out);
}

static bool ethBeginWithReset() {
    Serial.println("[ETH] Starting Ethernet...");
    Serial.println("[ETH] Initializing SPI...");
    SPI.begin(ETH_SPI_SCK, ETH_SPI_MISO, ETH_SPI_MOSI);

    pinMode(ETH_PHY_RST, OUTPUT);
    digitalWrite(ETH_PHY_RST, LOW);
    delay(100);
    digitalWrite(ETH_PHY_RST, HIGH);
    delay(200);

    bool ret = ETH.begin(
        ETH_PHY_W5500,
        ETH_PHY_ADDR,
        ETH_PHY_CS,
        ETH_PHY_IRQ,
        ETH_PHY_RST,
        SPI);

    Serial.printf("[ETH] ETH.begin returned: %s\n", ret ? "true" : "false");
    return ret;
}

static bool ethWaitLinkAndIp(uint32_t timeoutMs) {
    const uint32_t start = millis();
    while (millis() - start < timeoutMs) {
        if (ETH.linkUp() && (ETH.localIP() != IPAddress(0,0,0,0))) {
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    return false;
}

static void markEthDownStatus() {
    g_wifiConnected = false;
    g_wifiBars = 0;
    g_wifiRssi = 0;
    g_ft8ServerConnected = (wsServer.connectedClients() > 0);
    g_ft8ServerActive = false;
}

// Send an FT8 CQ (or message)
static void handleFt8Send() {
    JsonDocument doc;
    deserializeJson(doc, server.arg("plain"));
    int freq = doc["freq"] | 0;
    String msg = doc["msg"] | "";

    uint32_t nowTsSec = (uint32_t)(utc_ms() / 1000ULL);
    uint8_t parity = ((nowTsSec / 15) + 1) % 2; // next FT8 slot parity

    QSOManager::TxEnqueuePlan txPlan = qsoManager.prepareOutgoingTx(msg.c_str(), nowTsSec, parity);
    if (!txPlan.ok) {
        const char* errorText = txPlan.error ? txPlan.error : "failed to prepare outgoing tx";
        String resp = String("{\"error\":\"") + errorText + "\"}";
        server.send(400, "application/json", resp);
        return;
    }

    Serial.printf("Scheduling FT8 TX for QSO %u at %d Hz: %s myParity=%u\r\n",
            txPlan.qsoId, ui_get_vfo_freq() + ui_get_ft8_offset(), txPlan.normalizedMsg, txPlan.parity);

    if (!ft8tx.requestTransmission(freq,
                                   txPlan.normalizedMsg,
                                   txPlan.msgType,
                                   txPlan.parity,
                                   txPlan.qsoId)) {
        server.send(500, "application/json", "{\"error\":\"failed to queue tx request\"}");
        return;
    }

    // then log on the WEB UI
    //Serial.printf("[FT8] Send test at %d Hz: %s\n", freq, msg.c_str());
    char buf[128];
    sprintf(buf,"{\"message\":\"[FT8] Send message at %d Hz: %s\"}", freq, txPlan.normalizedMsg);
    server.send(200, "application/json", buf);
}

//answer an FT8 CQ
void handleFt8Answer() {
    if (!server.hasArg("plain")) {
        server.send(400, "application/json", "{\"error\":\"missing body\"}");
        return;
    }

    String body = server.arg("plain");

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, body);

    if (err) {
        server.send(400, "application/json", "{\"error\":\"invalid json\"}");
        return;
    }

    uint32_t qso_id = doc["qso_id"];
    int freq = doc["freq"] | 0;
    
    // 🔧 TODO: find QSO and mark as "mine" or trigger answer
    bool found = false;

    for (auto &q : qsoManager.qso_list) {
        if (q.qso_id == qso_id) {
            q.is_mine = true;   // or q.isMine = true depending on your naming
            found = true;

            // 👉 trigger your FT8 reply logic here
            uint8_t theirParity = ((q.lastHeard / 15) % 2);
            uint8_t myParity = (theirParity == 0) ? 1 : 0;

            //if (strlen(q.reply) == 0) return;
            // make a local copy of the reply string like:
            // <call1> IW5ALZ JN53
            String reply = String(q.call1) + " " + String(ui_get_mycall()) + " " + String(ui_get_mygrid()); 

            uint32_t nowTsSec = (uint32_t)(utc_ms() / 1000ULL);
            QSOManager::TxEnqueuePlan txPlan = qsoManager.prepareOutgoingTx(reply.c_str(), nowTsSec, myParity);
            if (!txPlan.ok) {
                const char* errorText = txPlan.error ? txPlan.error : "failed to prepare outgoing tx";
                String resp = String("{\"error\":\"") + errorText + "\"}";
                server.send(400, "application/json", resp);
                return;
            }

            if (txPlan.qsoId != q.qso_id) {
                server.send(409, "application/json", "{\"error\":\"qso mismatch while preparing tx\"}");
                return;
            }

            Serial.printf("Scheduling FT8 reply to QSO %d at %d Hz: %s theirParity=%d myParity=%d\r\n", 
            q.qso_id, ui_get_vfo_freq() + ui_get_ft8_offset(), reply.c_str(), theirParity, myParity);
            if (!ft8tx.requestTransmission(freq,
                                           txPlan.normalizedMsg,
                                           txPlan.msgType,
                                           txPlan.parity,
                                           txPlan.qsoId)) {
                server.send(500, "application/json", "{\"error\":\"failed to queue tx request\"}");
                return;
            }
            break;
        }
    }

    if (!found) {
        server.send(404, "application/json", "{\"error\":\"not found\"}");
        return;
    }

    server.send(200, "application/json", "{\"status\":\"ok\"}");
}

//Call a specific callsign 
void handleFt8Call() {
    if (!server.hasArg("plain")) {
        server.send(400, "application/json", "{\"error\":\"missing body\"}");
        return;
    }

    String body = server.arg("plain");

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, body);

    if (err) {
        server.send(400, "application/json", "{\"error\":\"invalid json\"}");
        return;
    }

    // get the qso from where to get call2 (the recipient of the call)
    uint32_t qso_id = doc["qso_id"];
    int freq = doc["freq"] | 0;
   
    bool found = false;

    for (auto &q : qsoManager.qso_list) {
        if (q.qso_id == qso_id) {
            found = true;
            // 👉 trigger your FT8 reply logic here
            uint8_t theirParity = ((q.lastHeard / 15) % 2);
            uint8_t myParity = (theirParity == 0) ? 1 : 0;

            // make a local copy of the reply string like:
            // <call2> IW5ALZ JN53
            String reply = String(q.call2) + " " + String(ui_get_mycall()) + " " + String(ui_get_mygrid()); 

            uint32_t nowTsSec = (uint32_t)(utc_ms() / 1000ULL);
            QSOManager::TxEnqueuePlan txPlan = qsoManager.prepareOutgoingTx(reply.c_str(), nowTsSec, myParity);
            if (!txPlan.ok) {
                const char* errorText = txPlan.error ? txPlan.error : "failed to prepare outgoing tx";
                String resp = String("{\"error\":\"") + errorText + "\"}";
                server.send(400, "application/json", resp);
                return;
            }
            // copy the grid of my new recipient            
            strlcpy(qsoManager.getQsoById(txPlan.qsoId)->grid1, q.grid2, sizeof(qsoManager.getQsoById(txPlan.qsoId)->grid1) ); 
            Serial.printf("Scheduling FT8 reply to QSO %d at %d Hz: %s theirParity=%d myParity=%d\r\n", 
            q.qso_id, ui_get_vfo_freq() + ui_get_ft8_offset(), reply.c_str(), theirParity, myParity);
            if (!ft8tx.requestTransmission(freq,
                                           txPlan.normalizedMsg,
                                           txPlan.msgType,
                                           txPlan.parity,
                                           txPlan.qsoId)) {
                server.send(500, "application/json", "{\"error\":\"failed to queue tx request\"}");
                return;
            }
            break;
        }
    }

    if (!found) {
        server.send(404, "application/json", "{\"error\":\"not found\"}");
        return;
    }

    server.send(200, "application/json", "{\"status\":\"ok\"}");
}


void handleFt8Clear() {
    if (!server.hasArg("plain")) {
        server.send(400, "application/json", "{\"error\":\"missing body\"}");
        return;
    }

    String body = server.arg("plain");

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, body);

    if (err) {
        server.send(400, "application/json", "{\"error\":\"invalid json\"}");
        return;
    }

    uint32_t qso_id = doc["qso_id"];

    bool found = false;

    for (auto &q : qsoManager.qso_list) {
        if (q.qso_id == qso_id) {
            q.is_mine = false;   // or q.isMine = false
            found = true;

            // 👉 cancel also all the pending transmissions  
            ft8tx.cancelJobsForQso(q.qso_id);
            break;
        }
    }

    if (!found) {
        server.send(404, "application/json", "{\"error\":\"not found\"}");
        return;
    }

    server.send(200, "application/json", "{\"status\":\"ok\"}");
}


// Get memory stats
static void handleMemoryStats() {
    JsonDocument doc;
    JsonObject o = doc.to<JsonObject>();

    o["heap_total"]     = ESP.getHeapSize();
    o["heap_free"]      = ESP.getFreeHeap();
    o["heap_min_free"]  = ESP.getMinFreeHeap();
    o["heap_max_alloc"] = ESP.getMaxAllocHeap();

    o["psram_total"] = ESP.getPsramSize();
    o["psram_free"]  = ESP.getFreePsram();

    // Add task profiler stats
    static uint64_t last = esp_timer_get_time();
    uint64_t now = esp_timer_get_time();
    double interval_us = now - last; // elapsed time since last call in microseconds
    last = now;
    
    JsonArray arr = o["task_profilers"].to<JsonArray>();
    
    for (int i=0; i<profilerCount; i++)
    {
        TaskProfiler& p = profilers[i];
        
        xSemaphoreTake(profilerMutex, portMAX_DELAY);
        uint64_t busy = p.busy_us;
        uint32_t loops = p.loops;
        p.busy_us = 0;
        p.loops = 0;
        xSemaphoreGive(profilerMutex);

        int16_t pct =
            100.0 * busy / interval_us;

        JsonObject obj = arr.add<JsonObject>();
        obj["name"] = p.name;
        obj["core"] = p.core;
        obj["cpu_perc"] = pct;
        obj["loops"] = loops;
        obj["busy_us"] = busy;
        obj["busy_avg_us"] = loops ? busy / loops : 0;
    }
    
    String out;
    serializeJson(doc, out);
    server.send(200, "application/json", out);
}

// ===== Web Server Setup =====

static void startWebServer() {
    if (webServerStarted) return;

    server.on("/", HTTP_GET, [](){
        File f = LittleFS.open("/index.html", "r");
        if(!f){ server.send(404, "text/plain", "index.html not found"); return; }
        server.streamFile(f, "text/html"); f.close();
    });

    server.on("/ui", HTTP_GET, [](){
        File f = LittleFS.open("/ui.html", "r");
        if(!f){ server.send(404, "text/plain", "ui.html not found"); return; }
        server.streamFile(f, "text/html"); f.close();
    });

    server.serveStatic("/app.js", LittleFS, "/app.js");
    server.serveStatic("/style.css", LittleFS, "/style.css");
    server.serveStatic("/cty_extended.dat", LittleFS, "/cty_extended.dat");

    server.on("/api/status", HTTP_GET, handleApiStatus);
    server.on("/api/ui", HTTP_GET, handleApiUi);
    server.on("/api/ui/save", HTTP_POST, handleApiUiSave);
    server.on("/api/wifi/save", HTTP_POST, handleApiWifiSave);
    server.on("/api/ft8/start", HTTP_POST, handleFt8Start);
    server.on("/api/ft8/stop", HTTP_POST, handleFt8Stop);
    server.on("/api/ft8/send", HTTP_POST, handleFt8Send);
    //server.on("/api/ft8/spots", HTTP_GET, handleFt8Spots);
    server.on("/api/ft8/answer", HTTP_POST, handleFt8Answer);
    server.on("/api/ft8/clear",  HTTP_POST, handleFt8Clear);
    server.on("/api/ft8/call",   HTTP_POST, handleFt8Call);

    server.on("/api/ft8/qsos", HTTP_GET, []() {
      server.send(200, "application/json", qsoManager.getAllQSOsJson());
    });

    server.on("/api/ft8/qsos/active", HTTP_GET, []() {
        server.send(200, "application/json", qsoManager.getActiveQSOsJson());
    });

    server.on("/api/ft8/qsos/completed", HTTP_GET, []() {
        server.send(200, "application/json", qsoManager.getCompletedQSOsJson());
    });

    server.on("/api/ft8/worked_dxcc", HTTP_GET, []() {
        server.send(200, "application/json", qsoStats.getJsonWorkedDXCC());
    });

    server.on("/api/ft8/worked_callsign", HTTP_GET, []() {
        server.send(200, "application/json", qsoStats.getJsonWorkedCallsign());
    });


    server.on("/api/ft8/qsos_dxcc", HTTP_GET, []() {
        server.send(200, "application/json", qsoStats.getJsonQSOsDXCC());
    });

    server.on("/api/ft8/bands_dxcc", HTTP_GET, []() {
        server.send(200, "application/json", qsoStats.getJsonBandsDXCC());
    });

    server.on("/api/ft8/clearAllWorkedDXCC", HTTP_GET, []() {
        qsoStats.clearAllWorkedDXCC();
        server.send(200, "application/json", "{\"ok\":true}");
    });

    server.on("/api/memstats", HTTP_GET, handleMemoryStats);

    server.begin(); 
    webServerStarted = true;     
    Serial.println("[WebServer] HTTP server started");
}

// ===== WebSocket Server Setup =====

static void startWebSocketServer() {
    if (wsServerStarted) return;
    
    uint16_t requestedPort = ui_get_ws_server_port();
    const uint16_t wsPort = 8765;
    if (requestedPort != wsPort) {
        Serial.printf("[WS] Requested port %u, using fixed port %u\n", requestedPort, wsPort);
    }
    Serial.printf("[WS] Starting WebSocket server on port %u\n", wsPort);

    wsServer.onEvent(webSocketEvent);
    wsServer.begin();
    wsServerStarted = true;
    
    Serial.printf("[WS] WebSocket server started on port 8765\n");
}


static void addFt8SpotFromJson(const char* json)
{
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, json);
    if (err) {
        Serial.println("[FT8] JSON parse error");
        return;
    }
    
    // uint32_t spot_freq = ui_get_vfo_freq(); // get the current VFO frequency
    // ft8FreqOptimizer.store(json, spot_freq); // feed the FT8 frequency optimizer with the new spot data

    Ft8Spot spot{};   // zero-initialize safely

    strlcpy(spot.receiver_callsign,
            doc["receiver_callsign"] | "",
            sizeof(spot.receiver_callsign));
    
    strlcpy(spot.decoded_line,
            doc["decoded_line"] | "",
            sizeof(spot.decoded_line));

    strlcpy(spot.receiver_grid,
            doc["receiver_grid"] | "",
            sizeof(spot.receiver_grid));

    strlcpy(spot.decoding_software,
            doc["decoding_software"] | "",
            sizeof(spot.decoding_software));

    strlcpy(spot.antenna_description,
            doc["antenna_description"] | "",
            sizeof(spot.antenna_description));

    strlcpy(spot.callsign,
            doc["callsign"] | "",
            sizeof(spot.callsign));

    strlcpy(spot.grid,
            doc["locator"] | "",
            sizeof(spot.grid));

    strlcpy(spot.report,
            doc["report"] | "",
            sizeof(spot.report));

    strlcpy(spot.mode,
            doc["mode"] | "FT8",
            sizeof(spot.mode));

    spot.freq_hz  = doc["frequency_hz"] | 0;
    spot.snr_db   = doc["snr_db"] | INT8_MIN; // use INT8_MIN to indicate missing SNR
    spot.timestamp = doc["timestamp"] | (utc_ms()/1000);

    // Derived fields
    spot.cq = doc["cq"] | false;
    spot.directed_to_me = false; // you can add logic later

    qsoManager.processFt8Spot(spot);

}

// ===== WiFi Signal Bars from RSSI =====

uint8_t wifiBarsFromRSSI(int rssi)
{
    if (rssi >= -55) return 4;   // Excellent
    if (rssi >= -65) return 3;   // Good
    if (rssi >= -75) return 2;   // Fair
    if (rssi >= -85) return 1;   // Weak
    return 0;                    // No signal
}

// ======== Network Task (Core 0) ========
// void NetworkTask(void* pvParameters) {

//     Serial.println("[ETH] Network task started on Core 0");
//     static bool ethConnected = false;
//     // Wait for UI startup
//     vTaskDelay(2000 / portTICK_PERIOD_MS);

//     if(!heap_caps_check_integrity_all(true)) ets_printf("!!! Corrupted heap before WiFi.begin !!!\n");
    
//     // ---- Start Ethernet ----
//     Serial.println("[ETH] Starting Ethernet...");
//     Serial.println("Initializing SPI...");
//     SPI.begin(ETH_SPI_SCK, ETH_SPI_MISO, ETH_SPI_MOSI);
//     // Reset the Ethernet PHY
//     pinMode(ETH_PHY_RST, OUTPUT);
//     digitalWrite(ETH_PHY_RST, LOW);
//     delay(100);
//     digitalWrite(ETH_PHY_RST, HIGH);
//     delay(200);

//     Serial.println("Initializing Ethernet...");
      
//     bool ret = ETH.begin(
//       ETH_PHY_W5500,        //           ETH_PHY_TYPE,
//       ETH_PHY_ADDR,
//       ETH_PHY_CS,
//       ETH_PHY_IRQ,
//       ETH_PHY_RST,
//       SPI);

//   Serial.printf("ETH.begin returned: %s\n",
//                 ret ? "true" : "false");
 
//     // Wait for link + IP
//     while (!ETH.linkUp() || ETH.localIP() == IPAddress(0,0,0,0)) {
//         vTaskDelay(500 / portTICK_PERIOD_MS);
//         Serial.print(".");
//     }

//     Serial.printf("[ETH] Connected, IP=%s\n",
//                   ETH.localIP().toString().c_str());

//     ethConnected = true;


//     // ---- Setup time, mDNS, WebSocket server ----
//     setup_time_once();
//     startWebServer();

//     // Start mDNS responder
//     if (!MDNS.begin(ui_get_ws_server_host())) {
//         Serial.println("Error starting mDNS");
//     }else
//         Serial.printf("mDNS started: %s.local\n", ui_get_ws_server_host());


//     auto* p = static_cast<TaskProfiler*>(pvParameters);
   
//     // Start WebSocket server
//     startWebSocketServer();

//     // ---- Main loop ----
//     for(;;) {
//        uint64_t t0 = esp_timer_get_time();
        
//          // ---- HTTP server ----
//         if (webServerStarted)
//             server.handleClient();

//         // ---- WebSocket server ----
//         wsServer.loop();

//         // ---- Ethernet reconnect handling ----
//         bool link = ETH.linkUp();

//         if (!link && ethConnected) {

//             Serial.println("[ETH] Link lost");
//             ethConnected = false;
//             g_wifiConnected = false;
//             g_wifiBars = 0;
//             g_wifiRssi = 0;
//             g_ft8ServerConnected = (wsServer.connectedClients() > 0);
//             g_ft8ServerActive = false;
//         }

//         if (link && !ethConnected) {

//             Serial.println("[ETH] Link restored");

//             // Wait for DHCP if needed
//             while (ETH.localIP() == IPAddress(0,0,0,0)) {
//                 vTaskDelay(100 / portTICK_PERIOD_MS);
//             }

//             Serial.printf("[ETH] IP=%s\n",
//                           ETH.localIP().toString().c_str());

//             setup_time_once();

//             if (!webServerStarted)
//                 startWebServer();

//             if (!wsServerStarted)
//                 startWebSocketServer();

//             ethConnected = true;
//         }

//         // ---- Send audio to websocket ----
//         AudioFrame* framePtr;
//         uint8_t sentAudioFrames = 0;
//         while (sentAudioFrames < AUDIO_FRAMES_PER_LOOP_MAX &&
//                xQueueReceive(audioQueue, &framePtr, 0) == pdTRUE) {

//             if (wsServer.connectedClients() > 0) {

//                 uint8_t packet[16 + WS_AUDIO_BUF_SAMPLES * 2];

//                 // timestamp
//                 for (int i=0;i<8;i++)
//                     packet[7-i] =
//                         (framePtr->ts_ms >> (8*i)) & 0xFF;

//                 // n_samples
//                 for (int i=0;i<4;i++)
//                     packet[8+3-i] =
//                         (framePtr->n_samples >> (8*i)) & 0xFF;

//                 // freq
//                 for (int i=0;i<4;i++)
//                     packet[12+3-i] =
//                         (framePtr->freq_hz >> (8*i)) & 0xFF;

//                 size_t samplesToCopy =
//                     min((uint16_t)framePtr->n_samples,
//                         (uint16_t)WS_AUDIO_BUF_SAMPLES);

//                 memcpy(packet + 16,
//                        framePtr->samples,
//                        samplesToCopy * 2);

//                 wsServer.broadcastBIN(packet,
//                            16 + samplesToCopy * 2);
//             }

//             xQueueSend(freeFrameQueue, &framePtr, 0);
//             sentAudioFrames++;
//         }

//         // ---- Send ADIF to WebSocket clients ----
//         if (wsServer.connectedClients() > 0) {

//             AdifUploadItem adifItem;

//             if (xQueueReceive(adifQueue,
//                               &adifItem,
//                               0) == pdTRUE) {

//                 size_t len =
//                     strnlen(adifItem.adif,
//                             sizeof(adifItem.adif));

//                 wsServer.broadcastTXT(adifItem.adif, len);
//             }
//         }

//         // ---- get FT8 messages from the external ft8 decoder via websocket ----
//         char ft8Msg[MAX_FT8_MSG];
//         const uint8_t ft8Budget = (uxQueueMessagesWaiting(audioQueue) > 0)
//                         ? FT8_MSGS_WHEN_AUDIO_PENDING
//                         : FT8_MSGS_PER_LOOP_MAX;
//         uint8_t processedFt8Msgs = 0;
//         while (processedFt8Msgs < ft8Budget &&
//                xQueueReceive(ft8Queue, ft8Msg, 0) == pdTRUE) {

//             if(ui_get_ws_server_enabled()) {
//                 addFt8SpotFromJson(ft8Msg);
//             }
            
//             g_ft8LastRxMs = millis();
//             processedFt8Msgs++;
//         }

//         // ---- Global network status ----
//         g_wifiConnected = ETH.linkUp();

//         // Ethernet has no RSSI
//         g_wifiRssi = 0;
//         g_wifiBars = ETH.linkUp() ? 5 : 0;

//         // ---- FT8 activity timeout ----
//         g_ft8ServerConnected = (wsServer.connectedClients() > 0);
//         if (g_ft8ServerConnected) {
//             g_ft8ServerActive =
//                 (millis() - g_ft8LastRxMs) < 15000;
//         } else {
//             g_ft8ServerActive = false;
//         }
    
//         // profiling
//         p->busy_us += esp_timer_get_time() - t0;
//         p->loops++;


//         vTaskDelay(pdMS_TO_TICKS(2));
//     }
// }

void NetworkTask(void* pvParameters) {
    Serial.println("[ETH] Network task started on Core 0");
    vTaskDelay(pdMS_TO_TICKS(2000));

    if(!heap_caps_check_integrity_all(true)) {
        ets_printf("!!! Corrupted heap before ETH init !!!\n");
    }

    auto* p = static_cast<TaskProfiler*>(pvParameters);

    static constexpr uint32_t ETH_IP_WAIT_MS = 8000;
    static constexpr uint32_t ETH_REINIT_AFTER_DOWN_MS = 15000;
    static constexpr uint32_t ETH_REINIT_COOLDOWN_MS = 5000;

    bool ethConnected = false;
    uint32_t ethDownSince = 0;
    uint32_t lastReinitMs = 0;

    // Initial bring-up (bounded wait, no infinite block)
    if (ethBeginWithReset() && ethWaitLinkAndIp(ETH_IP_WAIT_MS)) {
        Serial.printf("[ETH] Connected, IP=%s\n", ETH.localIP().toString().c_str());
        ethConnected = true;
        setup_time_once();
    } else {
        Serial.println("[ETH] Initial link/IP not ready, entering reconnect loop");
        markEthDownStatus();
    }

    startWebServer();
    if (!MDNS.begin(ui_get_ws_server_host())) {
        Serial.println("Error starting mDNS");
    } else {
        Serial.printf("mDNS started: %s.local\n", ui_get_ws_server_host());
    }
    startWebSocketServer();

    for (;;) {
        uint64_t t0 = esp_timer_get_time();

        if (webServerStarted) server.handleClient();
        wsServer.loop();

        const bool link = ETH.linkUp();
        const bool hasIp = (ETH.localIP() != IPAddress(0,0,0,0));
        const bool ethHealthy = link && hasIp;

        if (ethHealthy) {
            if (!ethConnected) {
                Serial.printf("[ETH] Recovered, IP=%s\n", ETH.localIP().toString().c_str());
                setup_time_once();
                ethConnected = true;
            }
            ethDownSince = 0;
        } else {
            if (ethConnected) {
                Serial.println("[ETH] Link/IP lost");
                ethConnected = false;
                markEthDownStatus();
            }

            if (ethDownSince == 0) {
                ethDownSince = millis();
            }

            const uint32_t now = millis();
            if ((now - ethDownSince) >= ETH_REINIT_AFTER_DOWN_MS &&
                (now - lastReinitMs) >= ETH_REINIT_COOLDOWN_MS) {

                lastReinitMs = now;
                Serial.println("[ETH] Reinitializing W5500...");

                if (ethBeginWithReset() && ethWaitLinkAndIp(ETH_IP_WAIT_MS)) {
                    Serial.printf("[ETH] Reinit OK, IP=%s\n", ETH.localIP().toString().c_str());
                    setup_time_once();
                    ethConnected = true;
                    ethDownSince = 0;
                } else {
                    Serial.println("[ETH] Reinit failed, will retry");
                }
            }
        }

        // ---- Send audio to websocket ----
        AudioFrame* framePtr;
        uint8_t sentAudioFrames = 0;
        while (sentAudioFrames < AUDIO_FRAMES_PER_LOOP_MAX &&
               xQueueReceive(audioQueue, &framePtr, 0) == pdTRUE) {

            if (wsServer.connectedClients() > 0) {
                uint8_t packet[16 + WS_AUDIO_BUF_SAMPLES * 2];

                for (int i = 0; i < 8; i++) packet[7 - i] = (framePtr->ts_ms >> (8 * i)) & 0xFF;
                for (int i = 0; i < 4; i++) packet[8 + 3 - i] = (framePtr->n_samples >> (8 * i)) & 0xFF;
                for (int i = 0; i < 4; i++) packet[12 + 3 - i] = (framePtr->freq_hz >> (8 * i)) & 0xFF;

                size_t samplesToCopy = min((uint16_t)framePtr->n_samples, (uint16_t)WS_AUDIO_BUF_SAMPLES);
                memcpy(packet + 16, framePtr->samples, samplesToCopy * 2);
                wsServer.broadcastBIN(packet, 16 + samplesToCopy * 2);
            }

            xQueueSend(freeFrameQueue, &framePtr, 0);
            sentAudioFrames++;
        }

        // ---- Send ADIF to WebSocket clients ----
        if (wsServer.connectedClients() > 0) {
            AdifUploadItem adifItem;
            if (xQueueReceive(adifQueue, &adifItem, 0) == pdTRUE) {
                size_t len = strnlen(adifItem.adif, sizeof(adifItem.adif));
                wsServer.broadcastTXT(adifItem.adif, len);
            }
        }

        // ---- FT8 messages from decoder websocket ----
        char ft8Msg[MAX_FT8_MSG];
        const uint8_t ft8Budget = (uxQueueMessagesWaiting(audioQueue) > 0)
                                    ? FT8_MSGS_WHEN_AUDIO_PENDING
                                    : FT8_MSGS_PER_LOOP_MAX;
        uint8_t processedFt8Msgs = 0;
        while (processedFt8Msgs < ft8Budget &&
               xQueueReceive(ft8Queue, ft8Msg, 0) == pdTRUE) {
            if (ui_get_ws_server_enabled()) {
                addFt8SpotFromJson(ft8Msg);
            }
            g_ft8LastRxMs = millis();
            processedFt8Msgs++;
        }

        // ---- Global network status ----
        g_wifiConnected = ethHealthy;
        g_wifiRssi = 0;
        g_wifiBars = ethHealthy ? 5 : 0;

        g_ft8ServerConnected = (wsServer.connectedClients() > 0);
        if (g_ft8ServerConnected) {
            g_ft8ServerActive = (millis() - g_ft8LastRxMs) < 15000;
        } else {
            g_ft8ServerActive = false;
        }

        p->busy_us += esp_timer_get_time() - t0;
        p->loops++;

        vTaskDelay(pdMS_TO_TICKS(2));
    }
}

void WiFiNetworkTask(void* pvParameters) {

    Serial.println("[WiFi] Network task started on Core 0");
    staStartMs = millis();
    lastReconnectMs = 0;
    static unsigned long lastReconnectAttempt = 0;
    staConnecting = true;
    static uint8_t targetBssid[] = SECRET_BSSID;

//    WiFi.onEvent(WiFiEvent);
    // Wait for UI to be fully initialized and I2C bus to be idle
    vTaskDelay(2000 / portTICK_PERIOD_MS); 

    // Connect to WiFi
    if(!heap_caps_check_integrity_all(true)) ets_printf("!!! Corrupted heap before WiFi.begin !!!\n");
            
    Serial.printf("[WiFi] Connecting to SSID: %s\n", SECRET_SSID);
    WiFi.begin(SECRET_SSID, SECRET_PASS);
    // Wait for WiFi STA connection
    while(WiFi.status() != WL_CONNECTED) {
        vTaskDelay(500);
        Serial.print(".");
    }
    Serial.printf("[WiFi] Connected, IP=%s\n", WiFi.localIP().toString().c_str());
    setup_time_once();

    // ---- Setup time, mDNS, WebSocket server ----
    setup_time_once();
    startWebServer();

    // Start mDNS responder
    if (!MDNS.begin(ui_get_ws_server_host())) {
        Serial.println("Error starting mDNS");
    }else
        Serial.printf("mDNS started: %s.local\n", ui_get_ws_server_host());

    // Start WebSocket server
    startWebSocketServer();

    // Main network loop
    for(;;) {

        // ---- HTTP server ----
        if (webServerStarted) 
            server.handleClient();

        // ---- WebSocket server loop ----
        wsServer.loop();

        // ---- WiFi reconnecting ----
        if (WiFi.status() != WL_CONNECTED &&
            millis() - lastReconnectMs > 10000) {
            lastReconnectMs = millis();
            
            ets_printf("[WiFi] Pre-reconnect heap check...\n");
            // If this fails, the heap was already corrupted BEFORE WiFi.begin
            if(!heap_caps_check_integrity_all(true)) ets_printf("!!! HEAP CORRUPTED before WiFi.begin !!!\n");

            ets_printf("[WiFi] Reconnecting...\r\n");
            WiFi.disconnect(true);
            vTaskDelay(500); // short delay before reconnecting
            WiFi.begin(SECRET_SSID, SECRET_PASS,0);
            esp_wifi_set_ps(WIFI_PS_NONE);
            staConnecting = true;
            staStartMs = millis();
        }

        // Start server once connected
        if(WiFi.status() == WL_CONNECTED && staConnecting) {
            WiFi.setSleep(false);
            esp_wifi_set_ps(WIFI_PS_NONE);
            ets_printf("[WiFi] Connected, IP=%s\n", WiFi.localIP().toString().c_str());
            setup_time_once();
            // Start HTTP server
            startWebServer();
            // Start WebSocket server
            startWebSocketServer();
            staConnecting = false;
        }

        vTaskDelay(2); // give CPU time to other tasks (WiFi core, DSP)
    
        // ---- Send audio to websocket if available ----
        AudioFrame* framePtr;
        uint8_t sentAudioFrames = 0;
        while (sentAudioFrames < AUDIO_FRAMES_PER_LOOP_MAX &&
               xQueueReceive(audioQueue, &framePtr, 0) == pdTRUE) {

          if (wsServer.connectedClients() > 0) {

              uint8_t packet[16 + WS_AUDIO_BUF_SAMPLES * 2];

              // Timestamp
              for (int i=0;i<8;i++)
                  packet[7-i] = (framePtr->ts_ms >> (8*i)) & 0xFF;

              // n_samples
              for (int i=0;i<4;i++)
                  packet[8+3-i] = (framePtr->n_samples >> (8*i)) & 0xFF;

              // freq
              for (int i=0;i<4;i++)
                  packet[12+3-i] = (framePtr->freq_hz >> (8*i)) & 0xFF;

              size_t samplesToCopy = min((uint16_t)framePtr->n_samples, (uint16_t)WS_AUDIO_BUF_SAMPLES);
              memcpy(packet+16, framePtr->samples, samplesToCopy * 2);
              // Serial.printf("[Audio] Sending frame with %d samples at %d Hz\n", framePtr->n_samples, framePtr->freq_hz);
              wsServer.broadcastBIN(packet, 16 + samplesToCopy * 2);
          }
          // Return frame to free pool
          xQueueSend(freeFrameQueue, &framePtr, 0);
          sentAudioFrames++;
        }

        // ---- Send ADIF to websocket if available ----
        if (wsServer.connectedClients() > 0) {
            AdifUploadItem adifItem;
            if (xQueueReceive(adifQueue, &adifItem, 0) == pdTRUE) {
                size_t len = strnlen(adifItem.adif, sizeof(adifItem.adif));

                Serial.printf("Sending ADIF len=%u: '%s'\n", len, adifItem.adif);
                wsServer.broadcastTXT(adifItem.adif, len); 
            }
        }

        // ---- get FT8 messages from the external ft8 decoder via websocket ----
        char ft8Msg[MAX_FT8_MSG];
        const uint8_t ft8Budget = (uxQueueMessagesWaiting(audioQueue) > 0)
                                    ? FT8_MSGS_WHEN_AUDIO_PENDING
                                    : FT8_MSGS_PER_LOOP_MAX;
        uint8_t processedFt8Msgs = 0;
        while (processedFt8Msgs < ft8Budget &&
               xQueueReceive(ft8Queue, ft8Msg, 0) == pdTRUE) {
            //Serial.printf("[FT8 RX] %s\n", ft8Msg);
            if(ui_get_ws_server_enabled()) {
                addFt8SpotFromJson(ft8Msg);
            }
            
            g_ft8LastRxMs = millis();   // mark traffic
            processedFt8Msgs++;
        }

        // ---- Update global WiFi status variables ----
        static uint32_t lastRssiRead = 0;
        static int lastRssi = -100;

        if (WiFi.status() == WL_CONNECTED) {
            g_wifiConnected = true;
            g_wifiReconnecting = false;

            if (millis() - lastRssiRead > 1000) {
                lastRssiRead = millis();
                int rssi = WiFi.RSSI();

                // Detect sudden drop (>10 dB)
                if (rssi < lastRssi - 10) {
                    Serial.printf("[WiFi] RSSI drop detected: %d -> %d dB\n", lastRssi, rssi);
                }
                lastRssi = rssi;

                g_wifiRssi = rssi;
                g_wifiBars = wifiBarsFromRSSI(rssi);
            }
        } else {
            g_wifiConnected = false;
            g_wifiBars = 0;
            g_wifiRssi = -100;
        }

        // FT8 activity timeout (no spots for 15s → idle)
        g_ft8ServerConnected = (wsServer.connectedClients() > 0);
        if (g_ft8ServerConnected) {
            g_ft8ServerActive = (millis() - g_ft8LastRxMs) < 15000;
        } else {
            g_ft8ServerActive = false;
        }
    } // for(;;)
}

void listLittleFS() {
    Serial.println("\n--- LittleFS file list ---");

    File root = LittleFS.open("/");
    File file = root.openNextFile();

    while (file) {
        Serial.print("FILE: ");
        Serial.print(file.name());
        Serial.print(" | SIZE: ");
        Serial.println(file.size());

        file = root.openNextFile();
    }

    Serial.println("--------------------------\n");
}


void listLogsFS() {
    Serial.println("\n--- LogsFS file list ---");

    File root = LogsFS.open("/");
    File file = root.openNextFile();

    while (file) {
        Serial.print("FILE: ");
        Serial.print(file.name());
        Serial.print(" | SIZE: ");
        Serial.println(file.size());

        file = root.openNextFile();
    }

    Serial.println("--------------------------\n");
}
// ===== Public Setup/Loop =====

void wifi_config_setup() {
    // if (!LittleFS.begin(true)) 
    if(!LittleFS.begin(true, "/littlefs", 10, "spiffs")) 
        Serial.println("[FS] LittleFS mount failed");
    if(!LogsFS.begin(true, "/logs", 10, "logs")) 
        Serial.println("[FS] LogsFS mount failed");

    Serial.printf("SPIFFS total: %d\n", LittleFS.totalBytes());
    Serial.printf("SPIFFS used : %d\n", LittleFS.usedBytes());
    listLittleFS();
    listLogsFS();

    // Initialize QSOManager after Serial is ready
    qsoManager.begin();
    
    loadCredentials();

    if(audioQueue == NULL)  audioQueue = xQueueCreate(6, sizeof(AudioFrame*));
    if(freeFrameQueue == NULL) freeFrameQueue = xQueueCreate(AUDIO_FRAME_POOL, sizeof(AudioFrame*));
    
    if(adifQueue == NULL) adifQueue = xQueueCreate(1, sizeof(AdifUploadItem)); 

    // 3. Popolate the queue (be sure audioPool is GLOBAL or STATIC)
    if (uxQueueMessagesWaiting(freeFrameQueue) == 0) {
        for (int i=0; i < AUDIO_FRAME_POOL; i++) {
            AudioFrame* f = &audioPool[i];
            if (xQueueSend(freeFrameQueue, &f, 0) != pdTRUE) break;
        }
    }

    if(ft8Queue == NULL) ft8Queue = xQueueCreate(8,  MAX_FT8_MSG);

    Serial.printf("Start network task on Core 0\n");
    xTaskCreatePinnedToCore(NetworkTask, "NET", 36000, &profilers[0], 1, NULL, 0); 
}

// ===== Audio Push =====
// It is called from the audio processing task to stream audio samples to WebSocket clients.
void wifi_config_audio_push(int32_t freq_hz, int16_t sample)
{
    static AudioFrame* frame = NULL;
    static uint16_t idx = 0;

    if (frame == NULL) {
      // get a free frame from the pool
      if (xQueueReceive(freeFrameQueue, &frame, 0) != pdTRUE){
        //idx = 0; // no free frame available, drop the sample
        return; // there's no free frame in the pool of free frames → drop
      }
      //idx = 0; // reset index for new frame 
    }
    // put the sample in the frame
    if (idx < WS_AUDIO_BUF_SAMPLES) {
        frame->samples[idx++] = sample;
    }
    
    if (idx >= WS_AUDIO_BUF_SAMPLES) {
        // frame is full, add header info
        //ets_printf("[Audio] Frame ready with %d samples at %d Hz\n", frame->n_samples, frame->freq_hz); 
        frame->ts_ms     = utc_ms();
        frame->freq_hz   = freq_hz;
        frame->n_samples = WS_AUDIO_BUF_SAMPLES;
        // send the ready frame to the websocket audio queue
        if (xQueueSend(audioQueue, &frame, 0) != pdTRUE) {
          // audio queue full, drop the frame and return it to free pool
          xQueueSend(freeFrameQueue, &frame, 0); 
        }

        frame = NULL;
        idx = 0;
  
    }
}


// ===== ADIF Text Push =====
// It is called from QSO Manager to stream ADIF text to WebSocket server
void wifi_config_adif_push(AdifUploadItem adif)
{
    // send the ADIF text to the websocket queue
    xQueueSend(adifQueue, &adif, 0); 
}

