#include "iq_adc.h"

#include <Arduino.h>

#include "configuration.h"

#if defined(ARDUINO_ARCH_ESP32)
  #include <esp_err.h>
  #include <esp_adc/adc_continuous.h>
  #include <esp_adc/adc_oneshot.h>  // used for gpio->(unit,channel) mapping
  #include <soc/soc_caps.h>

  #include <freertos/FreeRTOS.h>
  #include <freertos/task.h>
#else
  #error "iq_adc.cpp requires ESP32 (ARDUINO_ARCH_ESP32)"
#endif

// Arduino-ESP32 provides these ADC attenuation constants and helpers.
// Arduino.h pulls in the necessary headers.

static uint8_t currentAttLevel = 255;
static int16_t prevIRaw = 0;
static bool adcReady = false;
static bool pinsConfigured = false;

// DC estimates stored in fixed-point to avoid "stuck" behavior for small offsets.
static int32_t dcI = 0;  // Q8 (i.e. value << 8)
static int32_t dcQ = 0;  // Q8 (i.e. value << 8)

static inline int16_t center12bit(int raw);

static inline int16_t dcDecouple(int16_t x, int32_t* dc) {
  // Same concept as main.ori "DC decoupling": keep a running average and subtract it.
  // Use a slow average so we remove bias (e.g. 1.65V) without affecting audio-band test tones.
  // Keep dc in Q8 so even small deltas update the estimate.
  const int32_t xi = (int32_t)x;
  const int32_t xq = xi << 8;
  *dc += (xq - *dc) >> (int)IQ_ADC_DC_SHIFT;
  const int32_t dcInt = (*dc) >> 8;
  int32_t y = xi - dcInt;
  if (y > INT16_MAX) y = INT16_MAX;
  if (y < INT16_MIN) y = INT16_MIN;
  return (int16_t)y;
}

static volatile uint32_t droppedPairs = 0;
static volatile uint32_t adcTotalReads = 0;
static volatile uint32_t adcZeroLenReads = 0;
static volatile uint32_t adcTotalSamples = 0;
static volatile uint32_t adcUnitMismatch = 0;
static volatile uint32_t adcPushedPairs = 0;

// Lock-free-ish ring buffer for I/Q pairs (single producer, single consumer).
static IqSample iqRing[IQ_ADC_RING_SIZE];
static volatile uint32_t ringWrite = 0;
static volatile uint32_t ringRead = 0;

static inline uint32_t ringCount() {
  return (uint32_t)(ringWrite - ringRead);
}

static inline void ringPush(const IqSample& s) {
  // If full, drop the oldest item (keeps stream "most recent" and preserves phase continuity).
  if (ringCount() >= (uint32_t)IQ_ADC_RING_SIZE) {
    ringRead++;
    droppedPairs++;
  }
  iqRing[ringWrite % (uint32_t)IQ_ADC_RING_SIZE] = s;
  ringWrite++;
}

static inline bool ringPop(IqSample* out) {
  if (ringCount() == 0) return false;
  *out = iqRing[ringRead % (uint32_t)IQ_ADC_RING_SIZE];
  ringRead++;
  return true;
}

static adc_continuous_handle_t adcHandle = nullptr;
static adc_unit_t adcUnit = ADC_UNIT_1;
static adc_channel_t adcChanI;
static adc_channel_t adcChanQ;
static bool idfConfigured = false;

static adc_digi_pattern_config_t adcPattern[2];
static adc_continuous_config_t adcCfg;

static TaskHandle_t adcTask = nullptr;
static volatile uint32_t poolOvfCount = 0;
static volatile uint32_t adcReadErrors = 0;

static bool IRAM_ATTR onPoolOvf(adc_continuous_handle_t, const adc_continuous_evt_data_t*, void*) {
  poolOvfCount++;
  return false;
}

static inline adc_atten_t toIdfAtten(uint8_t level) {
  switch (level) {
    case 0: return ADC_ATTEN_DB_0;
    case 1: return ADC_ATTEN_DB_6;
    default: return ADC_ATTEN_DB_12;
  }
}

static bool idfStop() {
  if (!adcHandle) return true;
  const esp_err_t err = adc_continuous_stop(adcHandle);
  return (err == ESP_OK) || (err == ESP_ERR_INVALID_STATE);
}

static bool idfStart(adc_atten_t atten) {
  if (!adcHandle) return false;
  // Pattern order matters: main.ori alternates conversions I, Q, I, Q ...
  adcPattern[0].atten = (uint8_t)atten;
  adcPattern[1].atten = (uint8_t)atten;

  esp_err_t err = adc_continuous_config(adcHandle, &adcCfg);
  if (err != ESP_OK) {
    Serial.printf("[IQ] adc_continuous_config failed: %d\n", (int)err);
    return false;
  }

  (void)adc_continuous_flush_pool(adcHandle);
  
  err = adc_continuous_start(adcHandle);
  if (err != ESP_OK) {
    Serial.printf("[IQ] adc_continuous_start failed: %d\n", (int)err);
    return false;
  }

  return true;
}

static bool idfInit() {
  adc_unit_t unitI;
  adc_unit_t unitQ;
  adc_channel_t chanI;
  adc_channel_t chanQ;

  esp_err_t err = adc_oneshot_io_to_channel((gpio_num_t)GPIO_ADC_I, &unitI, &chanI);
  if (err != ESP_OK) {
    Serial.printf("[IQ] GPIO%d is not ADC-capable (err=%d)\n", GPIO_ADC_I, (int)err);
    return false;
  }
  err = adc_oneshot_io_to_channel((gpio_num_t)GPIO_ADC_Q, &unitQ, &chanQ);
  if (err != ESP_OK) {
    Serial.printf("[IQ] GPIO%d is not ADC-capable (err=%d)\n", GPIO_ADC_Q, (int)err);
    return false;
  }
  if (unitI != unitQ) {
    Serial.printf("[IQ] I and Q are on different ADC units (I=u%d, Q=u%d). Not supported.\n", (int)unitI, (int)unitQ);
    return false;
  }

  adcUnit = unitI;
  adcChanI = chanI;
  adcChanQ = chanQ;

  const uint32_t maxStore = IQ_ADC_DMA_MAX_STORE_BYTES;
  const uint32_t frameSize = IQ_ADC_DMA_FRAME_BYTES;  // must be a multiple of SOC_ADC_DIGI_DATA_BYTES_PER_CONV (4 on ESP32-S3)

  adc_continuous_handle_cfg_t hcfg = {};
  hcfg.max_store_buf_size = maxStore;
  hcfg.conv_frame_size = frameSize;
  hcfg.flags.flush_pool = 1;

  err = adc_continuous_new_handle(&hcfg, &adcHandle);
  if (err != ESP_OK) {
    Serial.printf("[IQ] adc_continuous_new_handle failed: %d\n", (int)err);
    adcHandle = nullptr;
    return false;
  }

  // Configure conversion pattern: I then Q.
  adcPattern[0].atten = (uint8_t)ADC_ATTEN_DB_0;
  adcPattern[0].channel = (uint8_t)adcChanI;
  adcPattern[0].unit = (uint8_t)adcUnit;
  adcPattern[0].bit_width = (uint8_t)ADC_BITWIDTH_12;

  adcPattern[1].atten = (uint8_t)ADC_ATTEN_DB_0;
  adcPattern[1].channel = (uint8_t)adcChanQ;
  adcPattern[1].unit = (uint8_t)adcUnit;
  adcPattern[1].bit_width = (uint8_t)ADC_BITWIDTH_12;

  adcCfg.pattern_num = 2;
  adcCfg.adc_pattern = adcPattern;
  adcCfg.sample_freq_hz = IQ_ADC_CONV_RATE_HZ;
  adcCfg.conv_mode = (adcUnit == ADC_UNIT_1) ? ADC_CONV_SINGLE_UNIT_1 : ADC_CONV_SINGLE_UNIT_2;
  adcCfg.format = ADC_DIGI_OUTPUT_FORMAT_TYPE2;

  // Register overflow callback (helps detect "lost samples" due to internal pool full).
  adc_continuous_evt_cbs_t cbs = {};
  cbs.on_pool_ovf = onPoolOvf;
  (void)adc_continuous_register_event_callbacks(adcHandle, &cbs, nullptr);

  return true;
}

static void adcReaderTask(void*) {
  // main.ori logic: conversions alternate I/Q and I is interpolated to compensate skew.
  bool havePendingI = false;
  int16_t pendingICorr = 0;

  // Use a large local buffer to drain DMA efficiently.
  static uint8_t rawBuf[IQ_ADC_DMA_FRAME_BYTES];

  uint32_t lastReport = millis();
  
  for (;;) {
    if (!adcHandle || !idfConfigured) {
      vTaskDelay(1);
      continue;
    }

    uint32_t outLen = 0;
    const esp_err_t err = adc_continuous_read(adcHandle, rawBuf, sizeof(rawBuf), &outLen, ADC_MAX_DELAY);
    adcTotalReads++;
    
    if (err != ESP_OK) {
      adcReadErrors++;
      vTaskDelay(1);
      continue;
    }
    if (outLen == 0) {
      adcZeroLenReads++;
      continue;
    }
    
    // Debug report every 5 seconds
    if ((millis() - lastReport) >= 5000) {
      Serial.printf("[ADC Task] reads=%u, errors=%u, zeroLen=%u, outLen=%u bytes\n", 
                    adcTotalReads, adcReadErrors, adcZeroLenReads, outLen);
      Serial.printf("[ADC Task] totalSamples=%u, unitMismatch=%u, pushedPairs=%u\n",
                    adcTotalSamples, adcUnitMismatch, adcPushedPairs);
      lastReport = millis();
    }

    const size_t n = outLen / SOC_ADC_DIGI_RESULT_BYTES;
    const adc_digi_output_data_t* s = (const adc_digi_output_data_t*)rawBuf;
    for (size_t k = 0; k < n; ++k) {
      adcTotalSamples++;
      const uint32_t unit = s[k].type2.unit;
      const uint32_t chan = s[k].type2.channel;
      const uint32_t data = s[k].type2.data;
      if (unit != (uint32_t)adcUnit) {
        adcUnitMismatch++;
        continue;
      }

      if (chan == (uint32_t)adcChanI) {
        const int16_t iRaw = center12bit((int)data);
        pendingICorr = (int16_t)((prevIRaw + iRaw) / 2);
        prevIRaw = iRaw;
        havePendingI = true;
      } else if (chan == (uint32_t)adcChanQ) {
        if (!havePendingI) continue;
        const int16_t q = center12bit((int)data);
        const int16_t iAc = dcDecouple(pendingICorr, &dcI);
        const int16_t qAc = dcDecouple(q, &dcQ);
        ringPush({ iAc, qAc });
        adcPushedPairs++;
        havePendingI = false;
      }
    }
  }
}

static inline int16_t center12bit(int raw) {
  // raw is typically 0..4095 on ESP32.
  // Convert to signed roughly centered around 0.
  return (int16_t)(raw - 2048);
}

void iq_adc_setup() {
  prevIRaw = 0;
  dcI = 0;
  dcQ = 0;
  currentAttLevel = 255;
  pinsConfigured = false;
  adcReady = false;

  droppedPairs = 0;
  ringWrite = 0;
  ringRead = 0;

  Serial.printf("[IQ] Using IDF ADC continuous mode @ %u conv/s (I/Q alternating).\n", (unsigned)IQ_ADC_CONV_RATE_HZ);
  idfConfigured = idfInit();
  if (!idfConfigured) {
    Serial.printf("[IQ] ADC continuous init failed.\n");
    return;
  }

  const adc_atten_t atten = toIdfAtten(0);
  if (!idfStart(atten)) {
    Serial.printf("[IQ] Failed to start ADC continuous.\n");
    return;
  }

  // Start a dedicated reader task so loop() stalls won't overflow the ADC pool.
  if (adcTask == nullptr) {
    if (IQ_ADC_TASK_CORE >= 0) {
      xTaskCreatePinnedToCore(adcReaderTask, "iq_adc", IQ_ADC_TASK_STACK, nullptr, IQ_ADC_TASK_PRIORITY, &adcTask, IQ_ADC_TASK_CORE);
    } else {
      xTaskCreate(adcReaderTask, "iq_adc", IQ_ADC_TASK_STACK, nullptr, IQ_ADC_TASK_PRIORITY, &adcTask);
    }
  }

  pinsConfigured = true;
  adcReady = true;
}

bool iq_adc_ready() { return adcReady; }

void iq_adc_set_att_level(uint8_t level) {
  if (!adcReady) return;
  if (level == currentAttLevel) return;
  currentAttLevel = level;

  if (idfConfigured && adcHandle) {
    const adc_atten_t atten = toIdfAtten(level);
    idfStop();
    (void)idfStart(atten);
    return;
  }
}

IqSample iq_adc_read_iq() {
  if (idfConfigured && adcHandle) {
    IqSample s;
    const uint32_t startMs = millis();
    while (!ringPop(&s)) {
      // Wait up to ~50ms for data, then return a zero sample.
      if ((uint32_t)(millis() - startMs) > 50U) return { 0, 0 };
      delay(0);
    }
    return s;
  }

  return { 0, 0 };
}

size_t iq_adc_available() {
  if (!adcReady) return 0;
  return (size_t)ringCount();
}

uint32_t iq_adc_dropped_pairs() { return droppedPairs; }

uint32_t iq_adc_pool_overflows() {
  return poolOvfCount;
}

void iq_adc_print_stats(size_t nSamples) {
  if (!adcReady) {
    Serial.printf("[IQ] Not ready (ADC pins not attached).\n");
    return;
  }
  if (nSamples == 0) return;

  int32_t sumI = 0, sumQ = 0;
  int16_t minI = INT16_MAX, maxI = INT16_MIN;
  int16_t minQ = INT16_MAX, maxQ = INT16_MIN;

  // Quick-and-dirty energy estimate.
  uint64_t sumSqI = 0, sumSqQ = 0;

  for (size_t k = 0; k < nSamples; ++k) {
    const IqSample s = iq_adc_read_iq();
    sumI += s.i;
    sumQ += s.q;
    if (s.i < minI) minI = s.i;
    if (s.i > maxI) maxI = s.i;
    if (s.q < minQ) minQ = s.q;
    if (s.q > maxQ) maxQ = s.q;
    sumSqI += (int32_t)s.i * (int32_t)s.i;
    sumSqQ += (int32_t)s.q * (int32_t)s.q;

    // Avoid starving the RTOS/WDT while doing large sample bursts.
    // Yield frequently to avoid starving the loop task WDT.
    delay(0);
  }

  const float meanI = (float)sumI / (float)nSamples;
  const float meanQ = (float)sumQ / (float)nSamples;
  const float rmsI = sqrtf((float)sumSqI / (float)nSamples);
  const float rmsQ = sqrtf((float)sumSqQ / (float)nSamples);

  Serial.printf(
    "[IQ] N=%u att=%u buf=%u drop=%u ovf=%u err=%u | I: mean=%.2f min=%d max=%d rms=%.2f | Q: mean=%.2f min=%d max=%d rms=%.2f\n",
    (unsigned)nSamples,
    (unsigned)currentAttLevel,
    (unsigned)iq_adc_available(),
    (unsigned)iq_adc_dropped_pairs(),
    (unsigned)iq_adc_pool_overflows(),
    (unsigned)adcReadErrors,
    meanI,
    (int)minI,
    (int)maxI,
    rmsI,
    meanQ,
    (int)minQ,
    (int)maxQ,
    rmsQ
  );
}
