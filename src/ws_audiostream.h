#pragma once

#include <Arduino.h>
#include <WebSocketsServer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>

#ifndef WS_AUDIO_BUF_SAMPLES
#define WS_AUDIO_BUF_SAMPLES 512
#endif

#ifndef WS_AUDIO_NUM_FRAMES
#define WS_AUDIO_NUM_FRAMES 8
#endif

struct AudioFrame
{
    uint64_t ts_ms;
    uint32_t freq_hz;
    uint16_t n_samples;
    int16_t  samples[WS_AUDIO_BUF_SAMPLES];
};

class WSAudioStream
{
public:
    explicit WSAudioStream(WebSocketsServer& ws);

    bool begin(
        UBaseType_t taskPriority = 2,
        BaseType_t core = 0);

    void pushSample(
        int32_t freq_hz,
        int16_t sample);

    uint32_t queuedFrames() const;
    uint32_t freeFrames() const;

private:

    bool started = false;    
    static void taskEntry(void* arg);
    void taskLoop();

    WebSocketsServer& _ws;

    QueueHandle_t _audioQueue = nullptr;
    QueueHandle_t _freeQueue  = nullptr;

    AudioFrame _pool[WS_AUDIO_NUM_FRAMES];

    AudioFrame* _curFrame = nullptr;
    uint16_t    _curIdx   = 0;

    TaskHandle_t _task = nullptr;

    volatile uint32_t _framesSent    = 0;
    volatile uint32_t _framesDropped = 0;
};