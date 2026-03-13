#pragma once
#include <Arduino.h>
#include "driver/i2s.h"

// -------------------- PCM1808 I2S Reader --------------------
struct IQSample32 {
    int32_t I;
    int32_t Q;
};

class PCM1808 {
public:
    // Constructor: specify pins and sample rate
    PCM1808(int bck, int din, int ws, int mclk, uint32_t sampleRate = 96000, size_t ringSize = 16384);

    // Initialize I2S and start DMA reading
    bool begin();

    // Get one sample from ring buffer
    bool getNextSample(IQSample32 &sample);

    // Check if overflow occurred and clear flag
    bool checkOverflow();

    // Compute actual sample rate
    float getActualSampleRate();

    // Compute dBFS 
    float computePowerDbfs();

    // Get number of available samples in ring buffer
    uint32_t iq_adc_available();

    // true when ADC is actively delivering samples
    bool iq_adc_ready();


private:
    // -------------------- Pins and configuration --------------------
    int _bckPin, _dinPin, _wsPin, _mclkPin;
    uint32_t _sampleRate;
    size_t _ringSize;
    volatile bool _running;
    volatile unsigned long _lastSampleMillis;

    // -------------------- Ring buffer --------------------
    volatile IQSample32* _ringBuffer;
    volatile size_t _rbHead;
    volatile size_t _rbTail;

    // Overflow flags
    volatile bool _overflowFlag;
    volatile uint32_t _overflowCount;
    volatile uint32_t _samplesWritten;

    // -------------------- Sample rate measurement --------------------
    unsigned long _lastRateCheck;
    uint32_t _lastSamplesWritten;

    // I2S task
    static void i2sReadTask(void *arg);

    // power measurement
    static const size_t _dbfsWindow = 1024;
    uint64_t _powerAccum;
    uint16_t _powerCount;
    float _lastDbfs;
};
