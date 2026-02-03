#include "wifi_config.h"

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <WebSocketsClient.h>
#include <Preferences.h>
#include <LittleFS.h>
#include "ui.h"
#include <ArduinoJson.h>
#include <sys/time.h>
#include <time.h>
#include <vector>
#include <string>
#include <mutex>
#include "qso_manager.cpp"

static WebServer server(80);
static WebSocketsClient ws;

#define CHUNK_SAMPLES 160
#define FRAME_SIZE (16 + CHUNK_SAMPLES*2) // header 16B + audio

#define WS_AUDIO_BUF_SAMPLES 256   // example

typedef struct {
    uint64_t ts_ms;
    int32_t  freq_hz;
    uint16_t n_samples;
    int16_t  samples[WS_AUDIO_BUF_SAMPLES];
} AudioFrame;

#define AUDIO_FRAME_POOL 8
AudioFrame audioPool[AUDIO_FRAME_POOL];

volatile uint8_t audioPoolHead = 0;

// ---- Queue ----
QueueHandle_t audioQueue;      // filled audio frames
QueueHandle_t freeFrameQueue;  // free reusable frames
QueueHandle_t ft8Queue;        // text only, no pointers


const char*    ws_server_host = "192.168.1.184";   // Python PC IP
const uint16_t ws_server_port = 8765;

static bool timeConfigured = false;
static bool timeSynced = false;
static bool serverStarted = false;
static uint32_t lastReconnectMs = 0;
static bool staConnecting = false;
static uint32_t staStartMs = 0;

static std::vector<Ft8Spot> ft8_spots;

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

QSOManager qsoManager("K1ABC", "FN31");

// ===== Helpers =====

static void loadCredentials() {
    Preferences prefs;
    if (!prefs.begin("net", true)) return;
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

// ===== WebSocket Event Handler=====
#define MAX_FT8_MSG 512  // maximum length of an FT8 string

void webSocketEvent(WStype_t type, uint8_t * payload, size_t length) {
  char msg[MAX_FT8_MSG];
  size_t len ;
  switch(type) { 
    
    case WStype_TEXT: 
      len = min(length, (size_t)MAX_FT8_MSG-1);
      memcpy(msg, payload, len);
      msg[len] = 0;
      //Serial.printf("[FT8] Received and sent to ft8Queue: %s\n", msg);
      xQueueSend(ft8Queue, msg, 0);   // copy data
      break;
 
    case WStype_CONNECTED:
        Serial.println("✅ Connected to FT8 server");
         g_ft8ServerConnected = true;
        break;

    case WStype_DISCONNECTED:
        Serial.println("❌ Disconnected — will auto reconnect");
        g_ft8ServerConnected = false;
        g_ft8ServerActive    = false;
        break;

    case WStype_ERROR:
        Serial.println("⚠ WebSocket error");
        break;

    case WStype_PONG:
        // keepalive
        break;

    default:
        break;
  }  
}

// ===== HTTP Handlers =====

static void handleApiStatus() {
    JsonDocument doc;
    doc["ip"] = WiFi.localIP().toString();
    doc["mode"] = (WiFi.getMode() & WIFI_STA) ? "STA" : "AP";
    //doc["ws_clients"] = wsServer.connectedClients();
    doc["time_synced"] = timeSynced;
    String out;
    serializeJson(doc, out);
    server.send(200, "application/json", out);
}

static void handleApiUi() {
    UiSettings s;
    if (!ui_get_settings(&s)) {
        server.send(500, "application/json", "{\"error\":\"read failed\"}");
        return;
    }
    JsonDocument doc;
    doc["vfoA"] = s.vfoA; doc["vfoB"] = s.vfoB; doc["vfoSel"] = s.vfoSel;
    doc["mode"] = s.mode; doc["bandval"] = s.bandval; doc["stepsize"] = s.stepsize;
    doc["rit"] = s.rit; doc["ritActive"] = s.ritActive; doc["volume"] = s.volume;
    doc["filt"] = s.filt; doc["agc"] = s.agc; doc["nr"] = s.nr; doc["att"] = s.att;
    doc["smode"] = s.smode; doc["cw_tone"] = s.cw_tone; doc["cw_offset"] = s.cw_offset;
    doc["vox"] = s.vox; doc["vox_gain"] = s.vox_gain; doc["drive"] = s.drive;
    doc["txdelay"] = s.txdelay; doc["mox"] = s.mox; doc["backlight"] = s.backlight;
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

    JSET(vfoA); JSET(vfoB); JSET(vfoSel); JSET(mode); JSET(bandval);
    JSET(stepsize); JSET(rit); JSET(ritActive); JSET(volume);
    JSET(filt); JSET(agc); JSET(nr); JSET(att); JSET(smode);
    JSET(cw_tone); JSET(cw_offset); JSET(vox); JSET(vox_gain);
    JSET(drive); JSET(txdelay); JSET(mox); JSET(backlight);
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
    server.send(200, "application/json", "{\"message\":\"Started\"}");
}

// Stop FT8 decoding
static void handleFt8Stop() {
    ft8_running = false;
    server.send(200, "application/json", "{\"message\":\"Stopped\"}");
}

// Send a test FT8 CQ (or message)
static void handleFt8Send() {
    JsonDocument doc;
    deserializeJson(doc, server.arg("plain"));
    int freq = doc["freq"] | 0;
    // For now we just log
    Serial.printf("[FT8] Send test at %d Hz\n", freq);
    server.send(200, "application/json", "{\"message\":\"Sent\"}");
}

// Get decoded spots
static void handleFt8Spots() {
    JsonDocument doc;
    JsonArray arr = doc.to<JsonArray>();  
    //std::lock_guard<std::mutex> lock(ft8_mutex);
    int idx = 0;
    for(auto &s : ft8_spots){
        JsonObject o = arr.add<JsonObject>();
        o["decoded_line"] = s.decoded_line;
        o["callsign"] = s.callsign;
        o["grid"] = s.grid;
        o["receiver_callsign"] = s.receiver_callsign;
        o["receiver_grid"] = s.receiver_grid;
        o["snr"] = s.snr_db; 
        o["mode"] = s.mode;
        o["time"] = s.timestamp;
        o["freq"] = s.freq_hz;
        o["report"] = s.report;
        o["cq"] = s.cq;
        o["directed_to_me"] = s.directed_to_me;
        idx++;
    }
    String out;
    serializeJson(doc, out);
    server.send(200, "application/json", out);

    
}

// ===== Web Server Setup =====

static void startServer() {
    if (serverStarted) return;

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

    server.on("/api/status", HTTP_GET, handleApiStatus);
    server.on("/api/ui", HTTP_GET, handleApiUi);
    server.on("/api/ui/save", HTTP_POST, handleApiUiSave);
    server.on("/api/wifi/save", HTTP_POST, handleApiWifiSave);
    server.on("/api/ft8/start", HTTP_POST, handleFt8Start);
    server.on("/api/ft8/stop", HTTP_POST, handleFt8Stop);
    server.on("/api/ft8/send", HTTP_POST, handleFt8Send);
    server.on("/api/ft8/spots", HTTP_GET, handleFt8Spots);

    server.on("/api/ft8/qsos", HTTP_GET, []() {
      server.send(200, "application/json", qsoManager.toJson());
    });

    server.on("/api/ft8/qsos/active", HTTP_GET, []() {
        server.send(200, "application/json", qsoManager.getActiveQSOsJson());
    });

    server.on("/api/ft8/qsos/completed", HTTP_GET, []() {
        server.send(200, "application/json", qsoManager.getCompletedQSOsJson());
    });

    server.begin(); 
    serverStarted = true;     
    Serial.println("[WebServer] HTTP server started");
}


static void addFt8SpotFromJson(const char* json)
{
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, json);
    if (err) {
        Serial.println("[FT8] JSON parse error");
        return;
    }

    Serial.printf("json=%s locator=%s\n", json, (const char*)doc["locator"] );

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
    spot.snr_db   = doc["snr_db"] | 0;
    spot.timestamp = doc["timestamp"] | (utc_ms()/1000);

    // Derived fields
    spot.cq = doc["cq"] | false;
    spot.directed_to_me = false; // you can add logic later

    // ---- Store safely ----
    ft8_spots.push_back(spot);

    // Keep memory bounded (VERY important)
    if (ft8_spots.size() > 50)
        ft8_spots.erase(ft8_spots.begin());

    qsoManager.addOrUpdate(spot);

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

// ======== Main Network Task ========
unsigned long lastReconnectAttempt = 0;
// ======== Network Task (Core 0) ========
void NetworkTask(void* pvParameters) {

    staStartMs = millis();
    lastReconnectMs = 0;
    lastReconnectAttempt = 0;
    staConnecting = true;

    // Connect to WiFi
    WiFi.begin(savedSsid, savedPass);

    // Wait for WiFi STA connection
    while(WiFi.status() != WL_CONNECTED) {
        vTaskDelay(500);
    }
    Serial.printf("[WiFi] Connected, IP=%s\n", WiFi.localIP().toString().c_str());
    setup_time_once();

    // Start HTTP server
    startServer();

    // Main network loop
    for(;;) {

        // ---- HTTP server ----
        if (serverStarted) server.handleClient();

        // ---- WebSocket loop ----
        ws.loop();

        // ---- WebSocket reconnect ----
        if (!ws.isConnected() && WiFi.status() == WL_CONNECTED &&
            millis() - lastReconnectAttempt > 5000) {
            lastReconnectAttempt = millis();
            Serial.println("[WS] Reconnecting...");
            ws.begin(ws_server_host, ws_server_port, "/");
            ws.onEvent(webSocketEvent);
        }

        // ---- WiFi reconnecting ----
        if (WiFi.status() != WL_CONNECTED &&
            millis() - lastReconnectMs > 10000) {
            lastReconnectMs = millis();
            Serial.println("[WiFi] Reconnecting...");
            WiFi.disconnect();
            WiFi.begin(savedSsid, savedPass);
            staConnecting = true;
            staStartMs = millis();
        }

        // ---- Send audio to websocket if available ----
        AudioFrame* framePtr;
        if (xQueueReceive(audioQueue, &framePtr, 0) == pdTRUE) {
          
          if (ws.isConnected()) {

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

              memcpy(packet+16, framePtr->samples, framePtr->n_samples * 2);

              ws.sendBIN(packet, 16 + framePtr->n_samples * 2);
          }
          // Return frame to free pool
          xQueueSend(freeFrameQueue, &framePtr, 0);
        }

        // ---- Process FT8 messages received ----
        char ft8Msg[MAX_FT8_MSG];
        while (xQueueReceive(ft8Queue, ft8Msg, 0) == pdTRUE) {
            //Serial.printf("[FT8 RX] %s\n", ft8Msg);
            addFt8SpotFromJson(ft8Msg);

            g_ft8LastRxMs = millis();   // mark traffic
        }



        vTaskDelay(2); // give CPU time to other tasks (WiFi core, DSP)
    
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
                    g_wifiWarning = true;
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
        if (g_ft8ServerConnected) {
            g_ft8ServerActive = (millis() - g_ft8LastRxMs) < 15000;
        } else {
            g_ft8ServerActive = false;
        }

    } // for(;;)

}

// ===== Public Setup/Loop =====

void wifi_config_setup() {
    if (!LittleFS.begin(true)) Serial.println("[FS] LittleFS mount failed");
    loadCredentials();

    audioQueue     = xQueueCreate(6, sizeof(AudioFrame*));
    freeFrameQueue = xQueueCreate(AUDIO_FRAME_POOL, sizeof(AudioFrame*));
    
    for (int i=0;i<AUDIO_FRAME_POOL;i++) {
        AudioFrame* f = &audioPool[i];
        xQueueSend(freeFrameQueue, &f, portMAX_DELAY);
    }

    ft8Queue       = xQueueCreate(8,  MAX_FT8_MSG);

    // Start network task on Core 0
    xTaskCreatePinnedToCore(NetworkTask, "NET", 12000, NULL, 1, NULL, 0);

}

// ===== Audio Push =====
// It is called from the audio processing task to stream audio samples to WebSocket clients.
void wifi_config_audio_push(int32_t freq_hz, int16_t sample)
{
    static AudioFrame* frame = NULL;
    static uint16_t idx = 0;

    if (frame == NULL) {
      // get a free frame from the pool
      if (xQueueReceive(freeFrameQueue, &frame, 0) != pdTRUE)
         return; // there's no free frame in the pool of free frames → drop
    }
    // put the sample in the frame
    frame->samples[idx++] = sample;

    if (idx >= WS_AUDIO_BUF_SAMPLES) {
        // frame is full, add header info
        frame->ts_ms     = utc_ms();
        frame->freq_hz   = freq_hz;
        frame->n_samples = WS_AUDIO_BUF_SAMPLES;
        // send the ready frame to the audio queue
        if (xQueueSend(audioQueue, &frame, 0) != pdTRUE) {
          // audio queue full, drop the frame and return it to free pool
          xQueueSend(freeFrameQueue, &frame, 0); 
        }

        frame = NULL;
        idx = 0;
  
    }
}

