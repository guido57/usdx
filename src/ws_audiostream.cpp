#include "ws_audiostream.h"

extern uint64_t utc_ms();

/* =========================================================
 * Constructor
 * ========================================================= */
WSAudioStream::WSAudioStream(WebSocketsServer& ws)
    : _ws(ws)
{
}

/* =========================================================
 * BEGIN
 * ========================================================= */
bool WSAudioStream::begin(
    UBaseType_t taskPriority,
    BaseType_t core)
{
    _audioQueue = xQueueCreate(WS_AUDIO_NUM_FRAMES, sizeof(AudioFrame*));
    _freeQueue  = xQueueCreate(WS_AUDIO_NUM_FRAMES, sizeof(AudioFrame*));

    _textQueue     = xQueueCreate(WS_TEXT_NUM_FRAMES, sizeof(TextFrame*));
    _textFreeQueue = xQueueCreate(WS_TEXT_NUM_FRAMES, sizeof(TextFrame*));

    if (!_audioQueue || !_freeQueue || !_textQueue || !_textFreeQueue)
        return false;

    // init audio pool
    for (uint32_t i = 0; i < WS_AUDIO_NUM_FRAMES; i++)
    {
        AudioFrame* p = &_pool[i];
        xQueueSend(_freeQueue, &p, portMAX_DELAY);
    }

    // init text pool
    for (uint32_t i = 0; i < WS_TEXT_NUM_FRAMES; i++)
    {
        TextFrame* p = &_textPool[i];
        xQueueSend(_textFreeQueue, &p, portMAX_DELAY);
    }

    BaseType_t rc =
        xTaskCreatePinnedToCore(
            taskEntry,
            "WSStreamTX",
            6144,
            this,
            taskPriority,
            &_task,
            core);

    started = (rc == pdPASS);
    return started;
}

/* =========================================================
 * AUDIO INPUT
 * ========================================================= */
void WSAudioStream::pushSample(int32_t freq_hz, int16_t sample)
{

    if (!started)
        return;

    if (_curFrame == nullptr)
    {
        if (xQueueReceive(_freeQueue, &_curFrame, 0) != pdTRUE)
            return;

        _curIdx = 0;
    }

    _curFrame->samples[_curIdx++] = sample;

    if (_curIdx < WS_AUDIO_BUF_SAMPLES)
        return;

    _curFrame->ts_ms     = utc_ms();
    _curFrame->freq_hz   = freq_hz;
    _curFrame->n_samples = WS_AUDIO_BUF_SAMPLES;

    if (xQueueSend(_audioQueue, &_curFrame, 0) != pdTRUE)
    {
        AudioFrame* p = _curFrame;
        xQueueSend(_freeQueue, &p, 0);
        _framesDropped++;
    }

    _curFrame = nullptr;
    _curIdx   = 0;
}

/* =========================================================
 * TEXT INPUT (NEW)
 * ========================================================= */
bool WSAudioStream::sendText(const char* msg, uint16_t len)
{
    if (!started)
        return false;

    TextFrame* f;

    if (xQueueReceive(_textFreeQueue, &f, 0) != pdTRUE)
        return false;

    if (len > sizeof(f->data))
        len = sizeof(f->data);

    memcpy(f->data, msg, len);
    f->len = len;

    if (xQueueSend(_textQueue, &f, 0) != pdTRUE)
    {
        xQueueSend(_textFreeQueue, &f, 0);
        return false;
    }

    return true;
}

/* =========================================================
 * TASK ENTRY
 * ========================================================= */
void WSAudioStream::taskEntry(void* arg)
{
    reinterpret_cast<WSAudioStream*>(arg)->taskLoop();
}

/* =========================================================
 * CENTRAL TX LOOP
 * ========================================================= */
void WSAudioStream::taskLoop()
{
    AudioFrame* a;
    TextFrame*  t;

    uint8_t audioPacket[16 + WS_AUDIO_BUF_SAMPLES * 2];
    for (;;)
    {
        /* -------------------------
         * AUDIO PATH
         * ------------------------- */
        if (xQueueReceive(_audioQueue, &a, 0) == pdTRUE)
        {
            encodeAudioPacket(audioPacket, a);

            if (_ws.connectedClients() > 0)
            {
                _ws.broadcastBIN(
                    audioPacket,
                    16 + a->n_samples * 2);
            }

            xQueueSend(_freeQueue, &a, portMAX_DELAY);
            _framesSent++;
        }

        /* -------------------------
         * TEXT PATH
         * ------------------------- */
        if (xQueueReceive(_textQueue, &t, 0) == pdTRUE)
        {
            if (_ws.connectedClients() > 0)
            {
                _ws.broadcastTXT(t->data, t->len);
            }

            xQueueSend(_textFreeQueue, &t, portMAX_DELAY);
        }

        vTaskDelay(1);

        /* -------------------------
         * REPORT
         * ------------------------- */
        static uint32_t lastReport = 0;

        if (millis() - lastReport > 1000)
        {
            lastReport = millis();

            Serial.printf(
                "framesSent=%d audioQ=%u textQ=%u freeA=%u freeT=%u clients=%u\n",
                _framesSent,
                uxQueueMessagesWaiting(_audioQueue),
                uxQueueMessagesWaiting(_textQueue),
                uxQueueMessagesWaiting(_freeQueue),
                uxQueueMessagesWaiting(_textFreeQueue),
                _ws.connectedClients()
            );
        }
    }
}

/* =========================================================
 * AUDIO ENCODER
 * ========================================================= */
void WSAudioStream::encodeAudioPacket(uint8_t* packet, AudioFrame* frame)
{
    for (int i = 0; i < 8; i++)
        packet[7 - i] = (frame->ts_ms >> (i * 8)) & 0xFF;

    for (int i = 0; i < 4; i++)
        packet[11 - i] = (frame->n_samples >> (i * 8)) & 0xFF;

    for (int i = 0; i < 4; i++)
        packet[15 - i] = (frame->freq_hz >> (i * 8)) & 0xFF;

    memcpy(packet + 16,
           frame->samples,
           frame->n_samples * sizeof(int16_t));
}

/* =========================================================
 * STATS
 * ========================================================= */
uint32_t WSAudioStream::queuedFrames() const
{
    return uxQueueMessagesWaiting(_audioQueue);
}

uint32_t WSAudioStream::freeFrames() const
{
    return uxQueueMessagesWaiting(_freeQueue);
}