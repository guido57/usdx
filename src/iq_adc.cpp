#include "iq_adc.h"
#include <Arduino.h>
#include <string.h>
#include "driver/adc.h"

// --- Configuration ---
// These are likely in your configuration.h, but defined here for safety
#ifndef IQ_ADC_DC_SHIFT
  #define IQ_ADC_DC_SHIFT 8 // Controls how slow the filter is
#endif

#define RING_BUF_SIZE 1024
#define ADC1_CHAN_I ADC1_CHANNEL_5 // GPIO 6
#define ADC1_CHAN_Q ADC1_CHANNEL_6 // GPIO 7

// --- Global State ---
static IqSample ringBuffer[RING_BUF_SIZE];
static volatile uint32_t ringWrite = 0;
static volatile uint32_t ringRead = 0;
static uint32_t droppedPairs = 0;

static TaskHandle_t adcTask = nullptr;
static bool adcReady = false;

// DC estimates stored in Q8 fixed-point (value << 8)
static int32_t dcI = 0;  
static int32_t dcQ = 0;  

// DMA Buffer - S3 requires Internal SRAM
static DMA_ATTR uint8_t rawBuf[2048];

// --- The Fixed-Point Decoupler ---

static inline int16_t dcDecouple(int16_t x, int32_t* dc) {
    // x is the raw centered value (-2048 to 2047)
    const int32_t xi = (int32_t)x;
    const int32_t xq = xi << 8; // Move to Q8 space
    
    // Leaky integrator: dc = dc + (current - dc) >> shift
    *dc += (xq - *dc) >> (int)IQ_ADC_DC_SHIFT;
    
    // Remove the average (dc >> 8) from the original signal
    const int32_t dcInt = (*dc) >> 8;
    int32_t y = xi - dcInt;
    
    // Clamp to signed 16-bit range
    if (y > INT16_MAX) y = INT16_MAX;
    if (y < INT16_MIN) y = INT16_MIN;
    
    return (int16_t)y;
}

static inline int16_t center12bit(uint16_t raw) {
    return (int16_t)raw - 2048;
}

static void ringPush(IqSample s) {
    uint32_t next = (ringWrite + 1) % RING_BUF_SIZE;
    if (next == ringRead) {
        droppedPairs++;
    } else {
        ringBuffer[ringWrite] = s;
        ringWrite = next;
    }
}

// --- The Hardware Reader Task ---

static void adcReaderTask(void* pvParameters) {
    bool havePendingI = false;
    int16_t pendingIValue = 0;

    for (;;) {
        uint32_t outLen = 0;
        esp_err_t err = adc_digi_read_bytes(rawBuf, sizeof(rawBuf), &outLen, 10);

        if (err == ESP_OK && outLen > 0) {
            for (int i = 0; i < (int)outLen; i += 4) {
                adc_digi_output_data_t *p = (adc_digi_output_data_t*)&rawBuf[i];
                
                if (p->type2.channel == 5) {
                    pendingIValue = center12bit(p->type2.data);
                    havePendingI = true;
                } 
                else if (p->type2.channel == 6 && havePendingI) {
                    int16_t qRaw = center12bit(p->type2.data);
                    
                    // APPLY FIXED-POINT DECOUPLER
                    //IqSample processed;
                    int32_t valI = dcDecouple(pendingIValue, &dcI);
                    int32_t valQ = dcDecouple(qRaw, &dcQ);
                    
                    // Apply 16x Digital Gain (Shift left by 4)
                    valI <<= 4;
                    valQ <<= 4;

                    // 3. Saturate (Clamp) to prevent 16-bit overflow
                    if (valI > 32767)  valI = 32767;
                    if (valI < -32768) valI = -32768;
                    if (valQ > 32767)  valQ = 32767;
                    if (valQ < -32768) valQ = -32768;

                    ringPush({ (int16_t)valI, (int16_t)valQ });
                    havePendingI = false;
                }
            }
        } else {
            vTaskDelay(1);
        }
    }
}

// --- Initialization ---

void iq_adc_setup() {
    pinMode(6, ANALOG);
    pinMode(7, ANALOG);

    // Initial DC estimates (optional: seed with your +170 value << 8)
    dcI = 0;
    dcQ = 0;

    adc_digi_stop();
    adc_digi_deinitialize();
    
    adc_digi_init_config_t init_cfg = {};
    init_cfg.max_store_buf_size = 4096;
    init_cfg.conv_num_each_intr = 256; 
    adc_digi_initialize(&init_cfg);

    static adc_digi_pattern_config_t pattern[2];
    pattern[0].atten = ADC_ATTEN_DB_0; // Your optimized 0dB
    pattern[0].channel = ADC1_CHAN_I;
    pattern[0].unit = 0;
    pattern[0].bit_width = SOC_ADC_DIGI_MAX_BITWIDTH;

    pattern[1].atten = ADC_ATTEN_DB_0;
    pattern[1].channel = ADC1_CHAN_Q;
    pattern[1].unit = 0;
    pattern[1].bit_width = SOC_ADC_DIGI_MAX_BITWIDTH;

    adc_digi_configuration_t dig_cfg = {};
    dig_cfg.conv_limit_en = false;
    dig_cfg.sample_freq_hz = 64000; 
    dig_cfg.conv_mode = ADC_CONV_SINGLE_UNIT_1;
    dig_cfg.format = ADC_DIGI_OUTPUT_FORMAT_TYPE2;
    dig_cfg.adc_pattern = pattern;
    dig_cfg.pattern_num = 2;

    adc_digi_controller_configure(&dig_cfg);
    adc_digi_start();

    if (adcTask == nullptr) {
        xTaskCreatePinnedToCore(adcReaderTask, "adc_task", 4096, NULL, 10, &adcTask, 1);
    }
    adcReady = true;
}

uint32_t iq_adc_available() {
    return (ringWrite - ringRead + RING_BUF_SIZE) % RING_BUF_SIZE;
}

IqSample iq_adc_read_iq() {
    if (ringRead == ringWrite) return {0, 0};
    IqSample s = ringBuffer[ringRead];
    ringRead = (ringRead + 1) % RING_BUF_SIZE;
    return s;
}

bool iq_adc_ready() { return adcReady; }
uint32_t iq_adc_dropped_pairs() { return droppedPairs; }

static uint8_t currentAttLevel = 255; // Initialize to an invalid value

static inline adc_atten_t toIdfAtten(uint8_t level) {
    switch (level) {
        case 0:  return ADC_ATTEN_DB_0;  // 0 - 950mV
        case 1:  return ADC_ATTEN_DB_6;  // 0 - 1250mV
        default: return ADC_ATTEN_DB_12; // 0 - 3100mV (S3 default)
    }
}

void iq_adc_set_att_level(uint8_t level) {
    if (!adcReady) return;
    if (level == currentAttLevel) return; // No change needed
    
    currentAttLevel = level;
    adc_atten_t newAtten = toIdfAtten(level);

    // 1. Stop the ADC to reconfigure
    adc_digi_stop();

    // 2. Update the existing pattern configuration
    // We use the same static pattern array defined in setup
    static adc_digi_pattern_config_t pattern[2];
    pattern[0].atten = (uint8_t)newAtten;
    pattern[0].channel = ADC1_CHANNEL_5; // GPIO 6
    pattern[0].unit = 0;
    pattern[0].bit_width = SOC_ADC_DIGI_MAX_BITWIDTH;

    pattern[1].atten = (uint8_t)newAtten;
    pattern[1].channel = ADC1_CHANNEL_6; // GPIO 7
    pattern[1].unit = 0;
    pattern[1].bit_width = SOC_ADC_DIGI_MAX_BITWIDTH;

    // 3. Re-apply the configuration
    adc_digi_configuration_t dig_cfg = {};
    dig_cfg.conv_limit_en = false;
    dig_cfg.sample_freq_hz = 64000; 
    dig_cfg.conv_mode = ADC_CONV_SINGLE_UNIT_1;
    dig_cfg.format = ADC_DIGI_OUTPUT_FORMAT_TYPE2;
    dig_cfg.adc_pattern = pattern;
    dig_cfg.pattern_num = 2;

    adc_digi_controller_configure(&dig_cfg);

    // 4. Restart
    adc_digi_start();
    
    Serial.printf("[IQ] Attenuation changed to level %u\n", level);
}