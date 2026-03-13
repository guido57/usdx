#include "pcm1808.h"
#include <math.h>

#define MAX_SAFE 8388607
#define MIN_SAFE -8388608

// -------------------- Constructor --------------------
PCM1808::PCM1808(int bck, int din, int ws, int mclk, uint32_t sampleRate, size_t ringSize)
    : _bckPin(bck), _dinPin(din), _wsPin(ws), _mclkPin(mclk),
      _sampleRate(sampleRate), _ringSize(ringSize),
      _rbHead(0), _rbTail(0), _overflowFlag(false),
      _overflowCount(0), _samplesWritten(0),
      _lastRateCheck(0), _lastSamplesWritten(0)
{
    // allocate ring buffer in .bss
    _ringBuffer = new IQSample32[_ringSize];

    _running = false;
    _lastSampleMillis = 0;
}

// -------------------- Begin --------------------
bool PCM1808::begin() {
    i2s_config_t i2s_config = {
        .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
        .sample_rate = _sampleRate,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT,
        .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count = 8,
        .dma_buf_len = 256,
        .use_apll = true,
        .tx_desc_auto_clear = false,
        .fixed_mclk = 256 * _sampleRate
    };

    i2s_pin_config_t pin_config;
    pin_config.bck_io_num   = _bckPin;
    pin_config.ws_io_num    = _wsPin;
    pin_config.data_out_num = I2S_PIN_NO_CHANGE;
    pin_config.data_in_num  = _dinPin;
    pin_config.mck_io_num   = _mclkPin;

    if (i2s_driver_install(I2S_NUM_0, &i2s_config, 0, NULL) != ESP_OK) return false;
    if (i2s_set_pin(I2S_NUM_0, &pin_config) != ESP_OK) return false;

    xTaskCreatePinnedToCore(i2sReadTask, "i2s_reader", 4096, this, 1, NULL, 1);

    return true;
}

// -------------------- I2S DMA task --------------------
void PCM1808::i2sReadTask(void *arg) {
    PCM1808* self = static_cast<PCM1808*>(arg);
    int32_t i2sBlock[256*2];
    size_t bytesRead;

    while (1) {
        esp_err_t res = i2s_read(I2S_NUM_0, i2sBlock, sizeof(i2sBlock), &bytesRead, portMAX_DELAY);
        if (res == ESP_OK && bytesRead > 0) {
            self->_running = true;   // ADC is delivering data
            self->_lastSampleMillis = millis();
            size_t frames = bytesRead / sizeof(int32_t) / 2;

            for (size_t i = 0; i < frames; i++) {
                size_t nextHead = (self->_rbHead + 1) % self->_ringSize;

                if (nextHead != self->_rbTail) {
                    int32_t I = i2sBlock[i*2] >> 8;
                    int32_t Q = i2sBlock[i*2 + 1] >> 8;

                    self->_ringBuffer[self->_rbHead].I = I;
                    self->_ringBuffer[self->_rbHead].Q = Q;
                    self->_rbHead = nextHead;

                    self->_samplesWritten++;

                    // ---- accumulate power for dBFS ----
                    self->_powerAccum += (uint64_t)I*I + (uint64_t)Q*Q;
                    self->_powerCount++;
                    if (self->_powerCount >= _dbfsWindow) {

                        float avg = (float)self->_powerAccum / _dbfsWindow;
                        float maxPower = 2.0f * 8388607.0f * 8388607.0f;

                        self->_lastDbfs = 10.0f * log10f(avg / maxPower);

                        self->_powerAccum = 0;
                        self->_powerCount = 0;
                    }


                    if (I >= MAX_SAFE || I <= MIN_SAFE || Q >= MAX_SAFE || Q <= MIN_SAFE) {
                        self->_overflowFlag = true;
                        self->_overflowCount++;
                    }
                } else {
                    self->_overflowFlag = true;
                    self->_overflowCount++;
                }
            }
        }
    }
}

// -------------------- Get next sample --------------------
bool PCM1808::getNextSample(IQSample32 &sample) {
    if (_rbTail == _rbHead) return false;
    sample = *(IQSample32*)&_ringBuffer[_rbTail];
    _rbTail = (_rbTail + 1) % _ringSize;
    return true;
}

// -------------------- Available samples --------------------
uint32_t PCM1808::iq_adc_available() {
    size_t head = _rbHead;
    size_t tail = _rbTail;

    if (head >= tail)
        return head - tail;
    else
        return _ringSize - tail + head;
}

// -------------------- ADC running state --------------------
bool PCM1808::iq_adc_ready() {
    if (!_running) return false;

    // if no samples for 200 ms, consider ADC stopped
    if (millis() - _lastSampleMillis > 200)
        return false;

    return true;
}

// -------------------- Check overflow --------------------
bool PCM1808::checkOverflow() {
    if (_overflowFlag) {
        _overflowFlag = false;
        return true;
    }
    return false;
}

// -------------------- Sample rate --------------------
float PCM1808::getActualSampleRate() {
    unsigned long now = millis();
    unsigned long dt = now - _lastRateCheck;

    if (dt >= 1000) {
        uint32_t count = _samplesWritten;
        float rate = (float)(count - _lastSamplesWritten) * 1000.0f / dt;
        _lastSamplesWritten = count;
        _lastRateCheck = now;
        return rate;
    }
    return -1.0f;
}

// -------------------- dBFS measurement --------------------
float PCM1808::computePowerDbfs() {
    return _lastDbfs;
}
