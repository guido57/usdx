#include <Arduino.h>
#include <Wire.h>

#include "configuration.h"
#include "phy/si5351.h"
#include "iq_adc.h"
#include "ui.h"
#include "audio_filter.h"
#include "audio_i2s.h"
#include "demodulator.h"
#include "cic_filter.h"

// PSRAM init functions for manual initialization after I2S
extern "C" {
  esp_err_t esp_psram_init(void);
  size_t esp_psram_get_size(void);
}

static SI5351 si5351;

static bool synthInitialized = false;
static Si5351RxSynthState lastSynth = { 0, 0, SI5351_RX_MODE_LSB, 0, false, 700 };

// Convert UiMode to DemodMode
static DemodMode ui_mode_to_demod_mode(UiMode mode) {
  switch (mode) {
    case UI_LSB: return DEMOD_LSB;
    case UI_USB: return DEMOD_USB;
    case UI_AM:  return DEMOD_AM;
    case UI_FM:  return DEMOD_FM;
    case UI_CW:  return DEMOD_USB; // CW uses USB demod + tone offset
    default:     return DEMOD_USB;
  }
}

// Audio processing with CIC decimation, Hilbert transform and LSB/USB/CW/AM/FM demodulation
static void processAudioTest() {
  static CicFilter cic;
  static uint32_t audioSampleCount = 0;
  static uint32_t iqPairCount = 0;
  static uint32_t lastAudioDebugMs = 0;
  static int16_t lastAudioSample = 0;  // For interpolation
  
  // DC removal state for I and Q channels
  static int32_t i_dc = 0;
  static int32_t q_dc = 0;
  
  // Process ALL available IQ samples
  while (iq_adc_ready() && iq_adc_available() > 0) {
    IqSample sample = iq_adc_read_iq();
    iqPairCount++;
    
    // Apply CIC decimating filter (decimation by 8: 32kHz → 4kHz)
    if (cic.processSample(sample.i, sample.q)) {
      // CIC filter has produced decimated output at 4kHz
      // Scale down: CIC has gain, >>4 (divide by 16) based on Python test
      int16_t iDecimated = cic.getOutputI() >> 4;
      int16_t qDecimated = cic.getOutputQ() >> 4;

      // DC removal (high-pass filter)
      // Uses slow-moving average to track DC component
      i_dc += (((int32_t)iDecimated << 8) - i_dc) >> 8;  // Time constant ~256 samples
      q_dc += (((int32_t)qDecimated << 8) - q_dc) >> 8;
      
      // Remove DC offset
      int16_t iClean = iDecimated - (i_dc >> 8);
      int16_t qClean = qDecimated - (q_dc >> 8);

      // Get current mode from UI
      UiMode uiMode = ui_get_mode();
      DemodMode demodMode = ui_mode_to_demod_mode(uiMode);
      
      // Apply Hilbert transform and demodulation
      int16_t audioSample = demod_process(iClean, qClean, demodMode);

      // Apply UI-selected audio filter (SSB/AM/FM shared; CW narrow filters when selected)
      int8_t filt = ui_get_filter();
      int8_t cw_tone = ui_get_cw_tone();
      int8_t volume = ui_get_volume();
      audioSample = audio_filter_apply(audioSample, filt, cw_tone, volume);
      
      // Scale to reasonable audio level
      audioSample = audioSample >> 1;  // Divide by 2 (increased gain)
      
      // Clip to prevent overflow
      if (audioSample > 32767) audioSample = 32767;
      if (audioSample < -32768) audioSample = -32768;
      
      // Interpolate 4kHz → 8kHz using linear interpolation
      // Send interpolated sample first
      int16_t interpolated = (lastAudioSample + audioSample) / 2;
      if (audio_i2s_write_sample(interpolated)) {
        audioSampleCount++;
      }
      // Then send actual sample
      if (audio_i2s_write_sample(audioSample)) {
        audioSampleCount++;
      }
      
      lastAudioSample = audioSample;
    }
  }
  
  // Debug: print audio stats every 2 seconds
  uint32_t nowMs = millis();
  if ((nowMs - lastAudioDebugMs) >= 2000) {
    uint32_t iqRate = iqPairCount / 2;
    uint32_t adcRate = iqRate * 2;
    Serial.printf("[AUDIO] IQ pairs: %u/s, ADC: %u Hz, Audio out: %u Hz\n", 
                  iqRate, adcRate, audioSampleCount/2);
    audioSampleCount = 0;
    iqPairCount = 0;
    lastAudioDebugMs = nowMs;
  }
}

void setup() {

  Serial.begin(115200); 
  delay(1000); 

  // Test PSRAM availability
  Serial.printf("\n=== Memory Status ===\n");
  Serial.printf("Total heap: %u bytes\n", ESP.getHeapSize());
  Serial.printf("Free heap: %u bytes\n", ESP.getFreeHeap());
  Serial.printf("PSRAM size: %u bytes\n", ESP.getPsramSize());
  Serial.printf("Free PSRAM: %u bytes\n", ESP.getFreePsram());
  
  if (ESP.getPsramSize() > 0) {
    Serial.printf("PSRAM is available!\n");
    // Try to allocate in PSRAM
    void* psram_test = heap_caps_malloc(1024, MALLOC_CAP_SPIRAM);
    if (psram_test) {
      Serial.printf("PSRAM allocation test: SUCCESS\n");
      heap_caps_free(psram_test);
    } else {
      Serial.printf("PSRAM allocation test: FAILED\n");
    }
  } else {
    Serial.printf("PSRAM is NOT available\n");
  }
  Serial.printf("=====================\n\n");

  Serial.printf("Initialize shared I2C bus once (OLED + SI5351 use the same Wire instance).\r\n");
  Wire.begin(I2C_SDA, I2C_SCL);
  Wire.setTimeOut(20);
  Wire.setClock(400000);

  Serial.printf("Apply initial SI5351 programming based on UI state at startup.\r\n");
  const UiMode uiModeNow = ui_get_mode();
  Si5351RxSynthState now = {
    (uint32_t)ui_get_sifxtal(),
    ui_get_vfo_freq(),
    ui_mode_to_si5351_rx_mode(uiModeNow),
    ui_get_rit(),
    ui_get_rit_active(),
    ui_get_cw_offset(),
  };
  si5351.fxtal = now.fxtalHz;
  si5351.powerDown();

  const int32_t programmedHz = programSi5351Rx(si5351, now);
  if (si5351.i2cError() != 0) {
    Serial.printf("SI5351 I2C error during programming: %u. Synth disabled.\n", si5351.i2cError());
    synthInitialized = false;
    return;
  }else {
    Serial.printf("SI5351 programmed successfully.\n");
    synthInitialized = true;
  }
  lastSynth = now;

  Serial.printf("Initialize UI.\r\n");
  ui_setup();

  Serial.printf("Initialize demodulator (Hilbert transform).\r\n");
  demod_init();

  Serial.printf("Initialize IQ ADC (pins + attenuation).\r\n");
  iq_adc_setup();
  iq_adc_set_att_level((uint8_t)ui_get_att());

  //set the resolution to 12 bits (0-4096)
  analogReadResolution(12);

  Serial.printf("Initialize I2S audio output (MAX98357).\r\n");
  if (audio_i2s_setup()) {
    Serial.printf("I2S audio initialized successfully.\r\n");
  } else {
    Serial.printf("Failed to initialize I2S audio!\r\n");
  }
}

void loop() {
  ui_loop();

  // Print I/Q buffer status (non-blocking)
  static uint32_t lastIqStatsMs = 0;
  const uint32_t nowMs = millis();
  if ((uint32_t)(nowMs - lastIqStatsMs) >= 2000U) {
    lastIqStatsMs = nowMs;
    size_t avail = iq_adc_available();
    uint32_t dropped = iq_adc_dropped_pairs();
    uint32_t ovf = iq_adc_pool_overflows();
    Serial.printf("[IQ] Buffer: %u samples available, %u dropped, %u pool overflows\n", 
                  (unsigned)avail, (unsigned)dropped, (unsigned)ovf);
  }

  // Process audio (simple test: decimation + pass-through)
  processAudioTest();

  if (!synthInitialized) return;

  // ------------------------------------------------------------------
  // Check for changes in UI state 
  // ------------------------------------------------------------------
  const UiMode uiModeNow = ui_get_mode();
  Si5351RxSynthState now = {
    (uint32_t)ui_get_sifxtal(),
    ui_get_vfo_freq(),
    ui_mode_to_si5351_rx_mode(uiModeNow),
    ui_get_rit(),
    ui_get_rit_active(),
    ui_get_cw_offset(),
  };
  // Including F_XTAL in synthStateChanged to handle crystal frequency changes
  if (synthStateChanged(now, lastSynth)) {
    si5351.fxtal = now.fxtalHz;
    const int32_t programmedHz = programSi5351Rx(si5351, now);
    lastSynth = now;
  }

  // Keep ADC attenuation in sync with UI ATT (main.ori-style sensitivity control).
  iq_adc_set_att_level((uint8_t)ui_get_att());
}
