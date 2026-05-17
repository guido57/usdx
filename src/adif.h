#pragma once

#include <Arduino.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include "ft8_types.h"

// ------------------------------------------------------------
// Queue item
// ------------------------------------------------------------
struct AdifUploadItem {
    int qsoId;
    char adif[512];
    uint8_t retries;
};

// ------------------------------------------------------------
// AdifQrz class
// ------------------------------------------------------------
class Adif {
public:

    Adif();

    bool begin(const char* apiKey, uint8_t queueSize = 10);

    bool enqueue(const QSO& qso, uint32_t freq_hz, char * bandname, char * modename);

    void startTask(
        uint16_t stackSize = 8192,
        UBaseType_t priority = 1,
        BaseType_t core = 1
    );

private:

    static void taskEntry(void* param);
    void taskLoop();

    String buildAdif(const QSO& qso, uint32_t freq_hz, char * bandname, char * modename   );

    bool sendAdif(const char* adif);

    String urlEncode(const String& s);

    String epochToDate(uint32_t t);
    String epochToTime(uint32_t t);

    String deriveBand(const QSO& qso); // YOU must customize

private:

    String _apiKey;
    QueueHandle_t _queue;
    TaskHandle_t _taskHandle;
};