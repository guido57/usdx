// ============================================================
// adifqrz.cpp
// ============================================================

#include "adif.h"
#include "wifi_config.h"
#include "ui.h"
// ------------------------------------------------------------
// Constructor
// ------------------------------------------------------------
Adif::Adif()
{
    _queue = nullptr;
    _taskHandle = nullptr;
}

// ------------------------------------------------------------
// begin()
// ------------------------------------------------------------
bool Adif::begin(
    const char* apiKey,
    uint8_t queueSize
) {
    _apiKey = apiKey;

    // _queue = xQueueCreate(
    //     queueSize,
    //     sizeof(AdifUploadItem)
    // );

    // return (_queue != nullptr);
    return true; // we are not using the queue in this simplified version, so just return true
}
// ------------------------------------------------------------
// epoch to date/time conversion
// ------------------------------------------------------------
String Adif::epochToDate(uint32_t t)
{
    time_t raw = t;
    struct tm* ti = gmtime(&raw);
    if (ti == nullptr) {
        return String("19700101");
    }

    char buf[16];
    snprintf(buf, sizeof(buf), "%04d%02d%02d",
        ti->tm_year + 1900,
        ti->tm_mon + 1,
        ti->tm_mday);

    return String(buf);
}

String Adif::epochToTime(uint32_t t)
{
    time_t raw = t;
    struct tm* ti = gmtime(&raw);
    if (ti == nullptr) {
        return String("000000");
    }

    char buf[16];
    snprintf(buf, sizeof(buf), "%02d%02d%02d",
        ti->tm_hour,
        ti->tm_min,
        ti->tm_sec);

    return String(buf);
}
// ------------------------------------------------------------
// startTask()
// ------------------------------------------------------------
// void Adif::startTask(
//     uint16_t stackSize,
//     UBaseType_t priority,
//     BaseType_t core
// ) {
//     xTaskCreatePinnedToCore(
//         taskEntry,
//         "AdifTask",
//         stackSize,
//         this,
//         priority,
//         &_taskHandle,
//         core
//     );
// }

// ------------------------------------------------------------
// enqueue()
// ------------------------------------------------------------
bool Adif::enqueue(const QSO& qso, uint32_t freq_hz, char * bandname, char * modename)
{
    
    static AdifUploadItem item{};
    item.qsoId = qso.qso_id;
    item.retries = 0;

    String adif = buildAdif(qso, freq_hz, bandname, modename);

    strncpy(item.adif, adif.c_str(), sizeof(item.adif) - 1);
    Serial.printf("[ADIF] Enqueuing QSO %d for upload: %s\n", item.qsoId, item.adif);
    // return xQueueSend(_queue, &item, 0) == pdTRUE;
    wifi_config_adif_push(item); // directly push to websocket queue for this simplified version
    return true; // we are not using the queue in this simplified version, so just return true
}
// ------------------------------------------------------------
// build ADIF
// ------------------------------------------------------------
String Adif::buildAdif(const QSO& qso, uint32_t freq_hz, char * bandname, char * modename)
{
    String s;
    bool call1_isDx = strcmp(qso.call1, ui_get_mycall()) != 0; 
    if(call1_isDx && strcmp(qso.call2, ui_get_mycall()) != 0 ) {
        // Neither call1 nor call2 is DX, this is unexpected, return empty string to avoid confusion
        return "";
    }

    // CALL (DX station)
    s += "<CALL:";
    s += call1_isDx ? strlen(qso.call1) : strlen(qso.call2);
    s += ">";
    s += call1_isDx ? qso.call1 : qso.call2 ;

    // GRID
    if (strlen(qso.grid2) > 0 && !call1_isDx) { // if grid2 is present and call1 is not mine, use grid2
        s += "<GRIDSQUARE:";
        s += strlen(qso.grid2);
        s += ">";
        s += qso.grid2;
    }else if (strlen(qso.grid1) > 0 && call1_isDx) { // otherwise, if grid1 is present, use grid1
        s += "<GRIDSQUARE:";
        s += strlen(qso.grid1);
        s += ">";
        s += qso.grid1;
    }

    // MODE
    s += "<MODE:3>FT8";

    if (freq_hz > 0) {
        char freqbuf[16];
        float freq_mhz = freq_hz / 1e6;
        dtostrf(freq_mhz, 0, 6, freqbuf);   // 6 decimals typical ADIF FT8 style

        s += "<FREQ:";
        s += strlen(freqbuf);
        s += ">";
        s += freqbuf;
    }

    // BAND (you must implement properly)
    String band = String(bandname);
    if (band.length()) {
        s += "<BAND:";
        s += band.length();
        s += ">";
        s += band;
    }

    // REPORTS (prefer parsed reports, fallback to SNR)
    
    if(call1_isDx) {
        if (strlen(qso.report1) > 0) {
            s += "<RST_SENT:";
            s += strlen(qso.report1);
            s += ">";
            s += qso.report1;
        } else {
            char buf[8];
            sprintf(buf, "%d", qso.snr1);
            s += "<RST_SENT:";
            s += strlen(buf);
            s += ">";
            s += buf;
        }
    } else {
        // call2 is DX
        if (strlen(qso.report2) > 0) {
            s += "<RST_RCVD:";
            s += strlen(qso.report2);
            s += ">";
            s += qso.report2;
        } else {
            char buf[8];
            sprintf(buf, "%d", qso.snr2);
            s += "<RST_RCVD:";
            s += strlen(buf);
            s += ">";
            s += buf;
        }
    }    
    // TIME
    String date = epochToDate(qso.firstSeen);
    String timeOn = epochToTime(qso.firstSeen);

    s += "<QSO_DATE:8>";
    s += date;

    s += "<TIME_ON:6>";
    s += timeOn;

    // optional TIME_OFF
    String timeOff = epochToTime(qso.lastHeard);
    s += "<TIME_OFF:6>";
    s += timeOff;

    s += "<EOR>";

    return s;
}
// ------------------------------------------------------------
// send ADIF to QRZ
// ------------------------------------------------------------
// bool Adif::sendAdif(
//     const char* adif
// ) {
//     if (WiFi.status() != WL_CONNECTED)
//         return false;

//     WiFiClientSecure client;

//     client.setInsecure();

//     HTTPClient https;

//     const char* url =
//         "https://logbook.qrz.com/api";

//     if (!https.begin(client, url))
//         return false;

//     https.setTimeout(10000);

//     https.addHeader(
//         "Content-Type",
//         "application/x-www-form-urlencoded"
//     );

//     String body;

//     body += "KEY=";
//     body += _apiKey;

//     body += "&ACTION=INSERT";

//     body += "&ADIF=";
//     body += urlEncode(adif);

//     int httpCode =
//         https.POST(body);

//     if (httpCode <= 0) {

//         https.end();
//         return false;
//     }

//     String response =
//         https.getString();

//     https.end();

//     Serial.println(response);

//     return (
//         response.indexOf("RESULT=OK")
//         >= 0
//     );
// }

// ------------------------------------------------------------
// URL encode
// ------------------------------------------------------------
// String Adif::urlEncode(
//     const String& s
// ) {
//     String out;

//     char hex[4];

//     for (size_t i = 0; i < s.length(); i++) {

//         char c = s[i];

//         if (isalnum(c)) {

//             out += c;

//         } else {

//             sprintf(
//                 hex,
//                 "%%%02X",
//                 (uint8_t)c
//             );

//             out += hex;
//         }
//     }

//     return out;
// }