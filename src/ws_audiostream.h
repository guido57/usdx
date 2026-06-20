#pragma once

#include <Arduino.h>
#include <WebSocketsServer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>

/* =========================================================
 * CONFIG
 * ========================================================= */
#ifndef WS_AUDIO_BUF_SAMPLES
#define WS_AUDIO_BUF_SAMPLES 256
#endif

#ifndef WS_AUDIO_NUM_FRAMES
#define WS_AUDIO_NUM_FRAMES 8
#endif

#ifndef WS_TEXT_NUM_FRAMES
#define WS_TEXT_NUM_FRAMES 8
#endif

/* =========================================================
 * AUDIO FRAME
 * ========================================================= */
struct AudioFrame
{
    uint64_t ts_ms;
    uint32_t freq_hz;
    uint16_t n_samples;
    int16_t  samples[WS_AUDIO_BUF_SAMPLES];
};

/* =========================================================
 * TEXT FRAME
 * ========================================================= */
struct TextFrame
{
    char data[256];
    uint16_t len;
};

/* =========================================================
 * WS AUDIO STREAM CLASS
 * ========================================================= */
class WSAudioStream
{
public:
    explicit WSAudioStream(WebSocketsServer& ws);

    bool begin(
        UBaseType_t taskPriority = 2,
        BaseType_t core = 0);

    /* -------------------------
     * AUDIO INPUT (DSP side)
     * ------------------------- */
    void pushSample(int32_t freq_hz, int16_t sample);

    /* -------------------------
     * TEXT INPUT (FT8 / UI side)
     * ------------------------- */
    bool sendText(const char* msg, uint16_t len);

    /* -------------------------
     * STATS
     * ------------------------- */
    uint32_t queuedFrames() const;
    uint32_t freeFrames() const;

    bool isStarted() const { return started; }

private:
    /* -------------------------
     * TASK
     * ------------------------- */
    static void taskEntry(void* arg);
    void taskLoop();

    void encodeAudioPacket(uint8_t* packet, AudioFrame* frame);

private:
    WebSocketsServer& _ws;

    TaskHandle_t _task = nullptr;
    volatile bool started = false;

    /* -------------------------
     * AUDIO QUEUES
     * ------------------------- */
    QueueHandle_t _audioQueue = nullptr;
    QueueHandle_t _freeQueue  = nullptr;

    AudioFrame _pool[WS_AUDIO_NUM_FRAMES];

    AudioFrame* _curFrame = nullptr;
    uint16_t _curIdx = 0;

    /* -------------------------
     * TEXT QUEUES
     * ------------------------- */
    QueueHandle_t _textQueue = nullptr;
    QueueHandle_t _textFreeQueue = nullptr;

    TextFrame _textPool[WS_TEXT_NUM_FRAMES];

    /* -------------------------
     * STATS
     * ------------------------- */
    uint32_t _framesSent = 0;
    uint32_t _framesDropped = 0;
};