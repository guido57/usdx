#include <Arduino.h>
#include <WiFi.h>
#include <Wire.h>
#include <math.h>

#include "configuration.h"
#include "secrets.h"
#include "si5351.h"
#include "ft8_tx.h"
// #include "iq_adc.h"
#include "pcm1808.h"
#include "ui.h"
#include "audio_filter.h"
#include "audio_i2s.h"
#include "demodulator.h"
#include "cic_filter.h"
#include "cic_filter32.h"
#include "wifi_config.h"
//#include "rx_att_pwm.h"
//#include "tx_bias_pwm.h"
#include "ft8_freq_opt.h"
#include "ant_filters.h"
#include "pwrswrmeter.h"
#include "qsostats.h"
#include "adif.h"
// #include "ft8_decoder.h"
#include "ft8_consumer_module.h"
#include "ws_audiostream.h"
#include "rgb.h"

static SI5351 si5351;
FT8_TX ft8tx(si5351);
FT8FreqOptimizer ft8FreqOptimizer;
QSOStats qsoStats;
Adif adif;

PCM1808 * pcm1808;
AntennaFilters *antFilters;
PowerSWRMeter *pwrswrmeter;
extern WSAudioStream wsAudioStream;
static bool setup_done = false;
static bool synthInitialized = false;
static Si5351RxSynthState lastSynth = { 0, 0, SI5351_RX_MODE_LSB, 0, false, 700 };

bool transmitting = false;
uint8_t audioVolume = 0; // Start with volume at 0 until we read the UI setting in the loop

// Convert UiMode to DemodMode
static DemodMode ui_mode_to_demod_mode(UiMode mode) {
  switch (mode) {
    case UI_LSB: return DEMOD_LSB;
    case UI_USB: return DEMOD_USB;
    case UI_AM:  return DEMOD_AM;
    case UI_CW:  return DEMOD_USB; // CW uses USB demod + tone offset
    default:     return DEMOD_USB;
  }
}

// ===============================
// tasks profiler
// ===============================

#include "task_profilers.h" 

TaskProfiler profilers[] = {
    { "NetworkTask", 0, 0, 0 },
    { "UI", 1, 0, 0 },
    { "DSP", 1, 0, 0 },
    { "loop", 1, 0, 0 },
};

size_t profilerCount  =
    sizeof(profilers) / sizeof(profilers[0]);

// ===============================
// SDR Voice AGC
// ===============================

static float agcGain      = 1.0f;
static float agcEnvelope  = 0.0f;

#define AGC_TARGET        4000.0f   // target peak level
#define AGC_ATTACK_TC     0.005f     // 5 ms
#define AGC_RELEASE_TC    0.300f     // 300 ms
#define AGC_SAMPLE_RATE   8000.0f    // audio rate

// Precomputed coefficients
static const float agcAttackCoef  = expf(-1.0f / (AGC_ATTACK_TC  * AGC_SAMPLE_RATE));
static const float agcReleaseCoef = expf(-1.0f / (AGC_RELEASE_TC * AGC_SAMPLE_RATE));

int16_t applyAGC(int16_t in)
{
    if (ui_get_agc() != 1)
        return in;   // AGC disabled

    float x = (float)in;
    float absx = fabsf(x);

    // ===== Envelope detector =====
    if (absx > agcEnvelope)
        agcEnvelope = agcAttackCoef * agcEnvelope + (1.0f - agcAttackCoef) * absx;
    else
        agcEnvelope = agcReleaseCoef * agcEnvelope + (1.0f - agcReleaseCoef) * absx;

    // ===== Gain computation =====
    if (agcEnvelope > 1.0f)
        agcGain = AGC_TARGET / agcEnvelope;
    else
        agcGain = 1.0f;

    // Limit maximum gain (avoid noise explosion)
    if (agcGain > 20.0f)
        agcGain = 20.0f;

    float y = x * agcGain;

    // ===== Soft clip safety =====
    if (y > 32767.0f)  y = 32767.0f;
    if (y < -32768.0f) y = -32768.0f;

    return (int16_t)y;
}

// ===== DEBUG PEAK METERS =====
#define PEAK_ALERT_LEVEL   28000   // near clipping (~-1.4 dBFS)
#define PEAK_WARN_LEVEL    16000   // high level (~-3 dBFS)

static int16_t peakIDec = 0;
static int16_t peakQDec = 0;
static int16_t peakAudio = 0;
static int16_t peakWebAudio = 0;
static int16_t peakAudioOut = 0;
static uint32_t peakLastMs = 0;

static inline int16_t fastAbs16(int16_t v) {
  return v < 0 ? -v : v;
}

inline bool peakAlert(int16_t v) {
  return fastAbs16(v) >= PEAK_ALERT_LEVEL;
}

inline bool peakWarn(int16_t v) {
  return fastAbs16(v) >= PEAK_WARN_LEVEL;
}

static void processAudioPCM1808() {

  static CicFilter32 cic;
  static int32_t i_dc = 0;
  static int32_t q_dc = 0;

  // --- Timing Debug Variables ---
  static uint32_t rawSampleCount = 0;
  static uint32_t lastReportMs = 0;

  uint32_t si5351_now = millis();

  // Process all samples currently waiting
  while (pcm1808->iq_adc_ready() && pcm1808->iq_adc_available() > 0) {

    IQSample32 sample32;
    pcm1808->getNextSample(sample32);

    // ADC scaling (prevent overflow in CIC)
    sample32.I >>= 6;
    sample32.Q >>= 6;

    // Apply I Q balance from UI
    float iqb = ui_get_iq_balance(); // read IQ balance from UI (currently not applied in processing, just read here to ensure we have the latest value for display in the UI)
    sample32.I = (int32_t)(sample32.I * iqb);
    sample32.Q = (int32_t)(sample32.Q * (2.0f -iqb)); // apply inverse of balance to Q to keep overall gain constant

    rawSampleCount++;

    // Feed CIC
    if (cic.processSample(sample32.I, sample32.Q)) {

      // CIC decimation output (8 kHz)
      int16_t iDecimated = (int16_t)(cic.getOutputI() >> 7);
      int16_t qDecimated = (int16_t)(cic.getOutputQ() >> 7);

      UiMode uiMode = ui_get_mode();
      DemodMode demodMode = ui_mode_to_demod_mode(uiMode);

      // DC removal (not for AM)
      if (demodMode != DEMOD_AM) {
        i_dc += (((int32_t)iDecimated << 8) - i_dc) >> 8;
        q_dc += (((int32_t)qDecimated << 8) - q_dc) >> 8;
        iDecimated -= (i_dc >> 8);
        qDecimated -= (q_dc >> 8);
      }

      // ===== PEAK IQ =====
      int16_t absI = fastAbs16(iDecimated);
      int16_t absQ = fastAbs16(qDecimated);
      if (absI > peakIDec) peakIDec = absI;
      if (absQ > peakQDec) peakQDec = absQ;

      // Demodulation
      int16_t audioSample = demod_process(iDecimated, qDecimated, demodMode);

      // ===== WEBSOCKET STREAM =====
      int32_t webAudio = audioSample;

      int16_t webAbs = abs(webAudio) > 32767 ? 32767 : abs(webAudio);
      if (webAbs > peakWebAudio) peakWebAudio = webAbs;

      wsAudioStream.pushSample(lastSynth.vfoHz, webAudio);

      // Never block the audio loop; drop FT8 sample if decoder queue is full.
      (void)ft8_consumer_module_enqueue_i16(&audioSample, 1, 0);

      // =========== AGC (if enabled) ===========
      // apply AGC if enabled in UI (AGC should be applied before volume control and peak measurement)
      audioSample = applyAGC(audioSample);
      // ===== PEAK AUDIO DEMOD =====
      int16_t absAudio = fastAbs16(audioSample);
      if (absAudio > peakAudio) peakAudio = absAudio;
      
      // ===== VOLUME CONTROL =====
      int32_t loudAudio = ((int32_t)audioSample * audioVolume) / 10; // scale volume (0-10) to 0.0-0.1 range

      // limiter
      if (loudAudio > 32767) loudAudio = 32767;
      if (loudAudio < -32768) loudAudio = -32768;

      int16_t out16 = (int16_t)loudAudio;

      // ===== PEAK AUDIO OUTPUT (LOUDSPEAKER) =====
      int16_t absOut = fastAbs16(out16);
      if (absOut > peakAudioOut) peakAudioOut = absOut;

      // I2S output
      audio_i2s_write_sample(out16);
    }

    // --- Sample rate report ---
    if (si5351_now - lastReportMs >= 1000) {

      float elapsedSec = (si5351_now - lastReportMs) / 1000.0f;
      float rate = rawSampleCount / elapsedSec;

      // Serial.printf("[ADC] Raw: %.1f Hz | Audio: %.1f Hz\r\n",
      //               rate, rate / 4.0f);

      rawSampleCount = 0;
      lastReportMs = si5351_now;
    }

    // --- Peak report ---
    if (si5351_now - peakLastMs >= 1000) {

      bool alert =
          peakAlert(peakIDec) ||
          peakAlert(peakQDec) ||
          peakAlert(peakAudio) ||
          peakAlert(peakWebAudio) ||
          peakAlert(peakAudioOut);

      bool warn =
          peakWarn(peakIDec) ||
          peakWarn(peakQDec) ||
          peakWarn(peakAudio) ||
          peakWarn(peakWebAudio) ||
          peakWarn(peakAudioOut);

        //if ( (alert || warn) && !transmitting) {


          Serial.printf(
            "[PEAK %s] IQ I=%5d Q=%5d | Aud=%5d | WS=%5d | OUT=%5d\r\n",
            alert ? "alert" : "warning",
            peakIDec,
            peakQDec,
            peakAudio,
            peakWebAudio,
            peakAudioOut
          );
        //}

      peakIDec = peakQDec = peakAudio = peakWebAudio = peakAudioOut = 0;
      peakLastMs = si5351_now;
    }
  }
}


#define AUDIO_BUFFER_SIZE 2048
static int16_t audioBuffer[AUDIO_BUFFER_SIZE];
static volatile uint16_t writeIndex = 0;
static volatile uint16_t readIndex  = 0;
static int16_t dmaBlock[64];  // preallocate once

static inline uint16_t bufferCount() {
    if (writeIndex >= readIndex) {
        return writeIndex - readIndex;
    } else {
        return AUDIO_BUFFER_SIZE - (readIndex - writeIndex);
    }
}

static void processAudioToneOnlyBuffered() {
    static float phase = 0.0f;
    const float fs = 8000.0f;
    const float fSignal = 1000.0f;
    const float twoPi = 6.28318530718f;
    const float phaseInc = twoPi * fSignal / fs;

    // --- 1️⃣ Fill ring buffer (max 32 samples per call to avoid blocking) ---
    const int MAX_SAMPLES_PER_CALL = 128;
    int samplesGenerated = 0;

    // Serial.printf("Buffer fill: writeIndex=%d readIndex=%d\r\n", writeIndex, readIndex);
    while (bufferCount() < AUDIO_BUFFER_SIZE - 256 && samplesGenerated < MAX_SAMPLES_PER_CALL){    
        
        int32_t sample = (int32_t)(cosf(phase) * 1023.0f * audioVolume);

        phase += phaseInc;
        if (phase >= twoPi) phase -= twoPi;

        if (sample > 32767) sample = 32767;
        if (sample < -32768) sample = -32768;

        int16_t loud = (int16_t)sample;
        
        audioBuffer[writeIndex] = loud;
        writeIndex = (writeIndex + 1) % AUDIO_BUFFER_SIZE;

        samplesGenerated++;
    }

    // --- 2️⃣ Feed I2S DMA in blocks if enough samples ---
    const int dmaBlockSize = sizeof(dmaBlock) / sizeof(dmaBlock[0]);
    //Serial.printf("DMA write: writeIndex=%d readIndex=%d\r\n", writeIndex, readIndex);
    
    if (bufferCount() >= dmaBlockSize) {

      for (int i = 0; i < dmaBlockSize; i++) {
          dmaBlock[i] = audioBuffer[readIndex];
          readIndex = (readIndex + 1) % AUDIO_BUFFER_SIZE;
      }

      size_t bytesWritten = 0;
      esp_err_t err = i2s_write(I2S_NUM_1,
                                dmaBlock,
                                dmaBlockSize * sizeof(int16_t),
                                &bytesWritten,
                                portMAX_DELAY);

      if (err != ESP_OK) {
          Serial.printf("i2s_write err=%d\r\n", err);
      }
      if(bytesWritten != dmaBlockSize * sizeof(int16_t)) {
          Serial.printf("i2s_write incomplete: %d bytes written\r\n", bytesWritten);
      } 
    }
}

static void processAudioPCM1808_simulatedIQ() {
    static CicFilter32 cic;
    static int32_t i_dc = 0;
    static int32_t q_dc = 0;

    // --- Timing Debug Variables ---
    static uint32_t rawSampleCount = 0;
    static uint32_t lastReportMs = 0;
    static uint32_t lastCallMs = 0;
    uint32_t si5351_now = millis();
    uint32_t deltaMs = si5351_now - lastCallMs;
    lastCallMs = si5351_now;

    // --- Simulation variables ---
    static float phase = 0.0f;
    const float fs = 32000.0f;     // nominal ADC sampling rate
    const float fSignal = 1000.0f; // 1 kHz test tone
    const float twoPi = 6.283185307179586f;

    // Calculate how many samples we need to generate based on elapsed time
    int samplesToGenerate = (int)((deltaMs / 1000.0f) * fs);
    if (samplesToGenerate < 1) samplesToGenerate = 1; // generate at least 1 sample

    for (int n = 0; n < samplesToGenerate; n++) {
        // Generate IQ sinusoid: I = cos(ωt), Q = sin(ωt)
        int32_t iSample = (int32_t)(cosf(phase) * 32767.0f);
        int32_t qSample = (int32_t)(sinf(phase) * 32767.0f);

        // Increment phase based on nominal sample rate
        phase += twoPi * fSignal / fs;
        if (phase >= twoPi) phase -= twoPi;

        rawSampleCount++;

        // Feed the sample into the CIC filter
        if (cic.processSample(iSample, qSample)) {
            int16_t iDecimated = (int16_t)(cic.getOutputI() >> 7);
            int16_t qDecimated = (int16_t)(cic.getOutputQ() >> 7);

            UiMode uiMode = ui_get_mode();
            DemodMode demodMode = ui_mode_to_demod_mode(uiMode);

            if (demodMode != DEMOD_AM) {
                i_dc += (((int32_t)iDecimated << 8) - i_dc) >> 8;
                q_dc += (((int32_t)qDecimated << 8) - q_dc) >> 8;
                iDecimated -= (i_dc >> 8);
                qDecimated -= (q_dc >> 8);
            }

            int16_t audioSample = demod_process(iDecimated, qDecimated, demodMode);

            // Feed audio to FT8 decoder
            // ft8_decoder_add_sample(audioSample);
                   

            wifi_config_audio_push(lastSynth.vfoHz, 16 * audioSample);

            int32_t loudAudio = (int32_t)audioSample * audioVolume;
            if (loudAudio > 32767) loudAudio = 32767;
            if (loudAudio < -32768) loudAudio = -32768;

            audio_i2s_write_sample((int16_t)loudAudio);
        }
    }

    // --- Periodic Sample Rate Print ---
    if (si5351_now - lastReportMs >= 1000) {
        float elapsedSec = (si5351_now - lastReportMs) / 1000.0f;
        float rate = rawSampleCount / elapsedSec;

        Serial.printf("[Sim IQ Stats] Raw Rate: %.1f Hz | Target: 32000 Hz | Audio Rate: %.1f Hz\r\n",
                      rate, rate / 4.0f);

        rawSampleCount = 0;
        lastReportMs = si5351_now;
    }
}




void setup() {

  Serial.begin(115200); 
  delay(1000); 

  RGB::startupTest();  

  // Test PSRAM availability
  Serial.printf("\r\n=== Memory Status ===\r\n");
  Serial.printf("Total heap: %u bytes\r\n", ESP.getHeapSize());
  Serial.printf("Free heap: %u bytes\r\n", ESP.getFreeHeap());
  Serial.printf("PSRAM size: %u bytes\r\n", ESP.getPsramSize());
  Serial.printf("Free PSRAM: %u bytes\r\n", ESP.getFreePsram());
  
  if (ESP.getPsramSize() > 0) {
    Serial.printf("PSRAM is available!\r\n");
    // Try to allocate in PSRAM
    void* psram_test = heap_caps_malloc(1024, MALLOC_CAP_SPIRAM);
    if (psram_test) {
      Serial.printf("PSRAM allocation test: SUCCESS\r\n");
      heap_caps_free(psram_test);
    } else {
      Serial.printf("PSRAM allocation test: FAILED\r\n");
    }
  } else {
    Serial.printf("PSRAM is NOT available\r\n");
  }
  Serial.printf("=====================\r\n\r\n");

  if(!heap_caps_check_integrity_all(true)) ets_printf("!!! HEAP CORROTTO prima di inizializzare Wire !!!\r\n");
  
  Serial.printf("Initialize shared I2C bus once (OLED + SI5351 + MCP23017 use the same Wire instance).\r\n");
  Wire.begin(I2C_SDA, I2C_SCL,100000U);
  Wire.setTimeOut(20);
  delay(10);
    
  // // Load persisted UI settings before programming the synth
  ui_load_settings();
  
  // Serial.printf("Apply initial SI5351 programming based on UI state at startup.\r\n");
  const UiMode uiModeNow = ui_get_mode();
  Si5351RxSynthState si5351_now = {
    (uint32_t)ui_get_sifxtal(),
    ui_get_vfo_freq(),
    ui_mode_to_si5351_rx_mode(uiModeNow),
    ui_get_rit(),
    ui_get_rit_active(),
    ui_get_cw_offset(),
    ui_get_iq_phase(),
    ui_get_sidrive()
  };
  
  if(!heap_caps_check_integrity_all(true)) ets_printf("!!! HEAP CORROTTO prima di inizializzare UI !!!\r\n");
  
  Serial.printf("Initialize UI.\r\n");
  ui_setup();

  delay(500);

  if(!heap_caps_check_integrity_all(true)) ets_printf("!!! HEAP CORROTTO prima di inizializzare SI5351 !!!\r\n");
  
  si5351.fxtal = si5351_now.fxtalHz;
  si5351.powerDown();

  const int32_t programmedHz = programSi5351Rx(si5351, si5351_now);
  if (si5351.i2cError() != 0) {
    Serial.printf("SI5351 I2C error during programming: %u. Synth disabled.\r\n", si5351.i2cError());
    synthInitialized = false;
    RGB::si5351(false);
    //return;
  }else {
    Serial.printf("SI5351 programmed successfully.\r\n");
    synthInitialized = true;
    RGB::si5351(true);
  }
  lastSynth = si5351_now;

  Serial.printf("Initialize demodulator (Hilbert transform).\r\n");
  demod_init();

  // Start WiFi stack before ADC (this sequence was previously stable)
  if(!heap_caps_check_integrity_all(true)) ets_printf("!!! HEAP CORROTTO prima di chiamare wifi_config_setup() !!!\r\n");
  ft8tx.begin(); // Set FT8 TX base 
  wifi_config_setup();
  // delay(10000); // let WiFi task initialize before enabling ADC DMA

  delay(500);
  // Serial.printf("Initialize IQ ADC (pins + attenuation).\r\n");
  // iq_adc_setup();
  // //set the resolution to 12 bits (0-4096)
  // analogReadResolution(12);
 
  Serial.printf("Initialize IQ ADC PCM1808 at a 96KHz sampling rate\r\n");
  pcm1808 = new PCM1808(I2S_BCK_PCM1808, I2S_DIN_PCM1808, I2S_WS_PCM1808, I2S_MCLK_PCM1808, 32000);
  bool pcm1808Initialized = pcm1808->begin();
  if (!pcm1808Initialized) {
    Serial.printf("Failed to initialize PCM1808!\r\n");
    return;
  }

  Serial.printf("Initialize I2S audio output (MAX98357).\r\n");
  if (audio_i2s_setup()) {
    Serial.printf("I2S audio initialized successfully.\r\n");
    delay(500);
  } else {
    Serial.printf("Failed to initialize I2S audio!\r\n");
  }


  // if (audio_i2s_setup_with_dma()) {
  //   Serial.printf("I2S audio initialized successfully.\r\n");
  //   delay(500);
  // } else {
  //   Serial.printf("Failed to initialize I2S audio!\r\n");
  // }
  Serial.printf("Initialize antenna filters control (MCP23017).\r\n");
  antFilters = new AntennaFilters( ANT_FILTERS_ADDR); // the actual I2C address of your MCP23017
  antFilters->begin();
  antFilters->setFilter(lastSynth.vfoHz); // Set the filter based on the initial VFO frequency and mode
  antFilters->setRx();

  // Initialize RX attenuation PWM control
  //rxAttPwmInit();
  //setRxAttFromUi(30); // Set initial attenuation to minumum (3V out)
  // Initialize TX bias PWM control
  // txBiasPwmInit();
  // setTxBiasFromUi(0); // Set initial TX bias to minimum (0V out)
  
  //iq_adc_set_att_level(0); // ADC attenuation always 0 dB

  audioVolume = ui_get_volume(); // Initialize audio volume from UI setting

  Serial.printf("Initialize Power/SWR meter.\r\n");
  pwrswrmeter = new PowerSWRMeter(GPIO_NUM_4, GPIO_NUM_5);
  pwrswrmeter->begin();
  
  Serial.printf("Initialize QSO stats.\r\n");
  qsoStats.begin();
  qsoStats.loadCTYFromFile("/cty_extended.dat");

  
  Serial.printf("Initialize FT8 decoder.\r\n");

  // Configure and initialize the FT8 consumer module
  ft8_consumer_module_config_t consumer_cfg = {
      .sample_rate = 8000,
      .base_freq_mhz = 0.0f,
      .sample_queue_depth = 2048, // kSampleQueueDepth,
      .finalize_queue_depth = 4,
      .append_batch_size = 256,
      .consumer_task_stack = 8192, // 12288,
      .finalize_task_stack = 8192, // 24576,
      .consumer_task_priority = 2,
      .finalize_task_priority = 0, // lower than loopTask to avoid blocking loop() on Core1
      .consumer_task_core = 0, // APP_CPU_NUM,
        .finalize_task_core = 0,
  };

  if (!ft8_consumer_module_init(&consumer_cfg)) {
      Serial.println("[ft8] Failed to initialize consumer module");
      return;
  }

  if (!ft8_consumer_module_start()) {
      Serial.println("[ft8] Failed to start consumer module");
      return;
  }


  setup_done = true;
}

time_t makeTimestamp(int year,
                     int month,
                     int day,
                     int hour,
                     int minute,
                     int second)
{
    struct tm t = {};

    t.tm_year = year - 1900; // years since 1900
    t.tm_mon  = month - 1;   // 0-11
    t.tm_mday = day;
    t.tm_hour = hour;
    t.tm_min  = minute;
    t.tm_sec  = second;

    return mktime(&t);
}

auto* p_ui = &profilers[1]; // for easy access in the loop for profiling
auto* p_dsp = &profilers[2]; // for easy access in the loop for profiling
auto* p_loop = &profilers[3]; // for easy access in the loop for profiling
QueueHandle_t profilerMutex = xSemaphoreCreateMutex();
  
void loop() {

  uint64_t t0 = esp_timer_get_time(); // profiler start time 

  static uint32_t lastReport = 0;
  static uint32_t loopCount = 0;

  // timing accumulators
  static uint32_t uiTimeTotal = 0;
  static uint32_t audioTimeTotal = 0;
  static uint32_t loopTimeTotal = 0;
  static uint32_t cycles = 0;

  loopCount++;

  uint32_t now_report = millis();

  if (now_report - lastReport >= 1000) {   // every 1 second

    float uiAvg = cycles ? uiTimeTotal / (float)cycles : 0;
    float audioAvg = cycles ? audioTimeTotal / (float)cycles : 0;

    // Serial.printf(
    //   "Loop: %lu Hz | ui_loop(): %.2f us | processAudioToneOnly(): %.2f us WiFi status=%d\r\n",
    //   loopCount, uiAvg, audioAvg, WiFi.status());

    loopCount = 0;
    uiTimeTotal = 0;
    audioTimeTotal = 0;
    cycles = 0;
    lastReport = now_report;
  }

  if(!setup_done) {
    delay(100);
    return;
  }

  uint64_t start;

  // ---------------- UI ----------------
  start = esp_timer_get_time();
  ui_loop();
  p_ui->busy_us += esp_timer_get_time() - start; // update profiler busy time
  p_ui->loops++;
  // ------------- Audio ---------------
  start = esp_timer_get_time();
  processAudioPCM1808();
  p_dsp->busy_us += esp_timer_get_time() - start; // update profiler busy time
  p_dsp->loops++;
  cycles++;

  // ----------- Synth control ----------
  const UiMode uiModeNow = ui_get_mode();
  Si5351RxSynthState si5351_now = {
    (uint32_t)ui_get_sifxtal(),
    ui_get_vfo_freq(),
    ui_mode_to_si5351_rx_mode(uiModeNow),
    ui_get_rit(),
    ui_get_rit_active(),
    ui_get_cw_offset(),
    ui_get_iq_phase(),
    ui_get_sidrive()
  };

  if (synthStateChanged(si5351_now, lastSynth)) {
    si5351.fxtal = si5351_now.fxtalHz;
    programSi5351Rx(si5351, si5351_now);

    // set the right antenna filter
    if(lastSynth.vfoHz != si5351_now.vfoHz) {
      antFilters->setFilter(si5351_now.vfoHz);
      Serial.printf("Antenna filter updated for frequency %u Hz\r\n", si5351_now.vfoHz);
    } 

    lastSynth = si5351_now;
    printf("SI5351 reprogrammed due to UI change. freq=%u\r\n", si5351_now.vfoHz);  
  }

  // setRxAttFromUi((uint8_t)ui_get_att_rf());

  // // Set TX bias from UI
  // if(transmitting) {  
  //   setTxBiasFromUi((uint8_t)ui_get_tx_bias()); 
  // } else {
  //   setTxBiasFromUi(TX_BIAS_FOR_RX);
  // }  

  static unsigned long lastPwrswrReport = 0;
  if(millis() - lastPwrswrReport > 1000) {
    if(transmitting) {
      Serial.printf("pwr=%.2f SWR=%.2f\r\n", pwrswrmeter->readPower(), pwrswrmeter->readSWR());
    }else{
      
    }
    lastPwrswrReport = millis();
  }

  // Every 60 seconds, re-apply the antenna filter for the current VFO frequency to ensure we are not accidentally left in a state with wrong filters (for example if there was a glitch during programming when changing mode/frequency in the UI that caused the filter update to be skipped, or if we are in a mode that doesn't automatically update filters on every loop like CW where filters are only updated when CW offset changes)
  static unsigned long lastSetAntFilters = 0;
  if(millis() - lastSetAntFilters > 60000 && !transmitting) { // every 60 seconds just to be sure we are in RX mode and not accidentally left in TX mode with filters set for TX
    antFilters->setFilter(si5351_now.vfoHz); // just to be sure we are in RX mode and not accidentally left in TX mode with filters set for TX 
    lastSetAntFilters = millis();
  }

  if(!transmitting) {
    // Set audio volume from UI
    audioVolume = ui_get_volume();
  }

  static uint32_t lastStatusMs = 0;
  if (millis() - lastStatusMs > 60000) {
    lastStatusMs = millis();
    uint16_t bestFreq =
        ft8FreqOptimizer.best_freq(ui_get_vfo_freq(), false, false);
    // Serial.printf("[FT8 Freq Optimizer] Best frequency: %u Hz    it took %lu ms\r\n", bestFreq, millis() - lastStatusMs );      
  
  }
  qsoStats.periodicSave(millis());

  p_loop->busy_us += esp_timer_get_time() - t0; // update loop profiler busy time 
  p_loop->loops++;


  static unsigned long lastStatPrint = 0;
  // if(millis() - lastStatPrint > 10000) {
  //   Serial.printf("p_ui->busy_us=%lu p_dsp->busy_us=%lu p_loop->busy_us=%lu\r\n", 
  //     p_ui->busy_us, p_dsp->busy_us, p_loop->busy_us);
  //   Serial.printf("p_ui->loops=%lu p_dsp->loops=%lu p_loop->loops=%lu\r\n", 
  //     p_ui->loops, p_dsp->loops, p_loop->loops);
  //   Serial.printf("UI avg: %.2f us | DSP avg: %.2f us | Loop avg: %.2f us\r\n", 
  //     p_ui->loops ? (float)p_ui->busy_us / p_ui->loops : 0,
  //     p_dsp->loops ? (float)p_dsp->busy_us / p_dsp->loops : 0,
  //     p_loop->loops ? (float)p_loop->busy_us / p_loop->loops : 0);    
  //   p_ui->busy_us = 0;
  //   p_dsp->busy_us = 0;
  //   p_loop->busy_us = 0;  
  //   p_ui->loops = 0;
  //   p_dsp->loops = 0;
  //   p_loop->loops = 0;
  //   lastStatPrint = millis();
  // }

  RGB::update();


}

