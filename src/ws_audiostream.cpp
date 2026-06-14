#include "ws_audiostream.h"

extern uint64_t utc_ms();

WSAudioStream::WSAudioStream(WebSocketsServer& ws)
    : _ws(ws)
{
}

bool WSAudioStream::begin(
    UBaseType_t taskPriority,
    BaseType_t core)
{
    _audioQueue = xQueueCreate(
        WS_AUDIO_NUM_FRAMES,
        sizeof(AudioFrame*));

    _freeQueue = xQueueCreate(
        WS_AUDIO_NUM_FRAMES,
        sizeof(AudioFrame*));

    if (!_audioQueue || !_freeQueue)
        return false;

    for (uint32_t i = 0; i < WS_AUDIO_NUM_FRAMES; i++)
    {
        AudioFrame* p = &_pool[i];
        xQueueSend(_freeQueue, &p, portMAX_DELAY);
    }

    BaseType_t rc =
        xTaskCreatePinnedToCore(
            taskEntry,
            "WSAudio",
            4096,
            this,
            taskPriority,
            &_task,
            core);

    started = (rc == pdPASS);
    
    return started;

    
}

void WSAudioStream::pushSample(
    int32_t freq_hz,
    int16_t sample)
{
    
    if(!started)
        return;
    
    if (_curFrame == nullptr)
    {
        if (xQueueReceive(
                _freeQueue,
                &_curFrame,
                0) != pdTRUE)
        {
            return;
        }

        _curIdx = 0;
    }

    _curFrame->samples[_curIdx++] = sample;

    if (_curIdx < WS_AUDIO_BUF_SAMPLES)
        return;

    _curFrame->ts_ms     = utc_ms();
    _curFrame->freq_hz   = freq_hz;
    _curFrame->n_samples = WS_AUDIO_BUF_SAMPLES;

    if (xQueueSend(
            _audioQueue,
            &_curFrame,
            0) != pdTRUE)
    {
        AudioFrame* p = _curFrame;
        xQueueSend(_freeQueue, &p, 0);
        _framesDropped = _framesDropped + 1;
    }

    _curFrame = nullptr;
    _curIdx = 0;
}

void WSAudioStream::taskEntry(void* arg)
{
    reinterpret_cast<WSAudioStream*>(arg)->taskLoop();
}

void WSAudioStream::taskLoop()
{
    AudioFrame* frame;

    uint8_t packet[16 + WS_AUDIO_BUF_SAMPLES * 2];

    for (;;)
    {
        if (xQueueReceive(
                _audioQueue,
                &frame,
                portMAX_DELAY) != pdTRUE)
        {
            continue;
        }

        for (int i = 0; i < 8; i++)
            packet[7 - i] =
                (frame->ts_ms >> (i * 8)) & 0xFF;

        for (int i = 0; i < 4; i++)
            packet[11 - i] =
                (frame->n_samples >> (i * 8)) & 0xFF;

        for (int i = 0; i < 4; i++)
            packet[15 - i] =
                (frame->freq_hz >> (i * 8)) & 0xFF;

        memcpy(
            packet + 16,
            frame->samples,
            frame->n_samples * sizeof(int16_t));

        if (_ws.connectedClients() > 0)
        {
            _ws.broadcastBIN(
                packet,
                16 + frame->n_samples * 2);
        }

        xQueueSend(
            _freeQueue,
            &frame,
            portMAX_DELAY);

        _framesSent = _framesSent + 1;



        static uint32_t lastReport = 0;
        if (millis() - lastReport > 1000) {
            lastReport = millis();

            Serial.printf(
                "audioQueue=%u freeFrames=%u clients=%u\n",
                uxQueueMessagesWaiting(_audioQueue),
                uxQueueMessagesWaiting(_freeQueue),
                _ws.connectedClients()
            );
        }

    }
}

uint32_t WSAudioStream::queuedFrames() const
{
    return uxQueueMessagesWaiting(_audioQueue);
}

uint32_t WSAudioStream::freeFrames() const
{
    return uxQueueMessagesWaiting(_freeQueue);
}