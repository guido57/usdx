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
#include <math.h>

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

    // Waterfall capture at 32kHz IQ (128-point FFT -> 128 bins across 32kHz)
    // Throttle updates to reduce CPU load.
    static int16_t wf_i[128];
    static int16_t wf_q[128];
    static uint8_t wf_idx = 0;
    static uint8_t wf_skip = 0;
    wf_i[wf_idx] = sample.i;
    wf_q[wf_idx] = sample.q;
    wf_idx++;
    if (wf_idx >= 128) {
      wf_idx = 0;
      wf_skip = (uint8_t)((wf_skip + 1) & 0x1F); // update every 32 blocks
      if (wf_skip == 0) {
        static bool fft_init = false;
        static float tw_re[64];
        static float tw_im[64];
        if (!fft_init) {
          for (uint16_t k = 0; k < 64; ++k) {
            const float ang = -2.0f * (float)M_PI * (float)k / 128.0f;
            tw_re[k] = cosf(ang);
            tw_im[k] = sinf(ang);
          }
          fft_init = true;
        }

        float re[128];
        float im[128];
        for (uint16_t n = 0; n < 128; ++n) {
          re[n] = (float)wf_i[n];
          im[n] = (float)wf_q[n];
        }

        // bit reversal
        for (uint16_t i = 0; i < 128; ++i) {
          uint8_t x = (uint8_t)i;
          x = (uint8_t)(((x & 0x55u) << 1) | ((x & 0xAAu) >> 1));
          x = (uint8_t)(((x & 0x33u) << 2) | ((x & 0xCCu) >> 2));
          x = (uint8_t)(((x & 0x0Fu) << 4) | ((x & 0xF0u) >> 4));
          uint8_t j = (uint8_t)(x >> 1); // 7-bit reversal
          if (j > i) {
            float tr = re[i]; re[i] = re[j]; re[j] = tr;
            float ti = im[i]; im[i] = im[j]; im[j] = ti;
          }
        }

        // radix-2 FFT
        for (uint16_t m = 2; m <= 128; m <<= 1) {
          const uint16_t half = m >> 1;
          const uint16_t step = 128 / m;
          for (uint16_t k = 0; k < 128; k += m) {
            for (uint16_t j = 0; j < half; ++j) {
              const uint16_t tw = j * step;
              const float wr = tw_re[tw];
              const float wi = tw_im[tw];
              const uint16_t idx = k + j + half;
              const float tr = wr * re[idx] - wi * im[idx];
              const float ti = wr * im[idx] + wi * re[idx];
              const float ur = re[k + j];
              const float ui = im[k + j];
              re[k + j] = ur + tr;
              im[k + j] = ui + ti;
              re[idx] = ur - tr;
              im[idx] = ui - ti;
            }
          }
        }

        float maxMag = 1.0f;
        float mags[128];
        for (uint16_t k = 0; k < 128; ++k) {
          const float mag = re[k] * re[k] + im[k] * im[k];
          mags[k] = mag;
          if (mag > maxMag) maxMag = mag;
        }

        uint8_t bins[128];
        for (uint16_t x = 0; x < 128; ++x) {
          const uint16_t bin = (x + 64) & 127; // fftshift
          float v = mags[bin] / maxMag;
          if (v < 1e-6f) v = 1e-6f;
          if (v > 1.0f) v = 1.0f;
          // Log scaling (compress dynamic range) with tunable floor
          float vdb = log10f(v) + 6.0f; // maps 1e-6..1 -> 0..6
          float norm = vdb / 6.0f;
          float thresh = ui_get_wf_thresh() / 100.0f;
          if (norm < thresh) norm = 0.0f;
          uint8_t level = (uint8_t)(norm * 15.0f + 0.5f);
          if (level > 15) level = 15;
          bins[x] = level;
        }
        ui_set_waterfall_line(bins, 128);
      }
    }
    
    // Apply CIC decimating filter (decimation by 4: 32kHz → 8kHz)
    if (cic.processSample(sample.i, sample.q)) {
      // CIC filter has produced decimated output at 8kHz
      // Scale down: CIC has gain, >>3 (divide by 8) for decimation by 4
      int16_t iDecimated = cic.getOutputI() >> 3;
      int16_t qDecimated = cic.getOutputQ() >> 3;

      // DC removal (high-pass filter)
      // Uses slow-moving average to track DC component
      i_dc += (((int32_t)iDecimated << 8) - i_dc) >> 8;  // Time constant ~256 samples
      q_dc += (((int32_t)qDecimated << 8) - q_dc) >> 8;
      
      // Remove DC offset
      int16_t iClean = iDecimated - (i_dc >> 8);
      int16_t qClean = qDecimated - (q_dc >> 8);

      // Fractional-sample timing skew correction (IQ Delay)
      // Positive delay => delay Q by d samples; Negative delay => delay I by |d| samples
      static int16_t prevI = 0;
      static int16_t prevQ = 0;
      const int16_t rawI = iClean;
      const int16_t rawQ = qClean;
      const float d = ui_get_iq_delay(); // -0.50 .. +0.50
      if (d > 0.0f) {
        qClean = (int16_t)((1.0f - d) * rawQ + d * prevQ);
      } else if (d < 0.0f) {
        const float ad = -d;
        iClean = (int16_t)((1.0f - ad) * rawI + ad * prevI);
      }
      prevI = rawI;
      prevQ = rawQ;

  #if IQ_MEASURE_LOG
      // Image rejection measurement right after ADC + DC removal (8 kHz domain)
      // Computes complex tone power at +1 kHz and -1 kHz.
      static double ir_sum_re_pos = 0.0;
      static double ir_sum_im_pos = 0.0;
      static double ir_sum_re_neg = 0.0;
      static double ir_sum_im_neg = 0.0;
      static uint32_t ir_count = 0;
      static float ir_cos = 1.0f;
      static float ir_sin = 0.0f;
      static const float ir_cos_step = 0.70710678f; // cos(2*pi*1000/8000)
      static const float ir_sin_step = 0.70710678f; // sin(2*pi*1000/8000)

      // Mix to +1 kHz: (I + jQ) * (cos - j sin)
      float mix_re_pos = iClean * ir_cos + qClean * ir_sin;
      float mix_im_pos = qClean * ir_cos - iClean * ir_sin;

      // Mix to -1 kHz: (I + jQ) * (cos + j sin)
      float mix_re_neg = iClean * ir_cos - qClean * ir_sin;
      float mix_im_neg = qClean * ir_cos + iClean * ir_sin;

      ir_sum_re_pos += mix_re_pos;
      ir_sum_im_pos += mix_im_pos;
      ir_sum_re_neg += mix_re_neg;
      ir_sum_im_neg += mix_im_neg;
      ir_count++;

      // Update oscillator for next sample
      float next_cos = ir_cos * ir_cos_step - ir_sin * ir_sin_step;
      float next_sin = ir_sin * ir_cos_step + ir_cos * ir_sin_step;
      ir_cos = next_cos;
      ir_sin = next_sin;

      if (ir_count >= 4096) { // ~0.512s at 8kHz
        double p_pos = ir_sum_re_pos * ir_sum_re_pos + ir_sum_im_pos * ir_sum_im_pos;
        double p_neg = ir_sum_re_neg * ir_sum_re_neg + ir_sum_im_neg * ir_sum_im_neg;
        double strong = (p_pos > p_neg) ? p_pos : p_neg;
        double weak = (p_pos > p_neg) ? p_neg : p_pos;
        double ratio = (strong + 1e-12) / (weak + 1e-12);
        double ratio_db = 10.0 * log10(ratio);
        const char* strong_label = (p_pos > p_neg) ? "+1k" : "-1k";
        Serial.printf("[Image Reject @1k] +1k=%.3e -1k=%.3e strong=%s ratio=%.2f dB (%.3fx)\n",
                      p_pos, p_neg, strong_label, ratio_db, ratio);

        ir_sum_re_pos = 0.0;
        ir_sum_im_pos = 0.0;
        ir_sum_re_neg = 0.0;
        ir_sum_im_neg = 0.0;
        ir_count = 0;
      }
  #endif
      
      // Apply IQ phase correction FIRST (right after DC removal)
      // Phase correction: rotate I/Q by (iq_phase - 90) degrees
      // I' = I*cos(θ) - Q*sin(θ), Q' = I*sin(θ) + Q*cos(θ)
      int16_t phase_deg = ui_get_iq_phase();
      float phase_error_rad = (90 - phase_deg) * 0.0174533f; // Convert to radians (invert correction direction)
      float cos_theta = cosf(phase_error_rad);
      float sin_theta = sinf(phase_error_rad);
      
      int16_t iPhased = (int16_t)(iClean * cos_theta - qClean * sin_theta);
      int16_t qPhased = (int16_t)(iClean * sin_theta + qClean * cos_theta);
      iClean = iPhased;
      qClean = qPhased;
      
    #if IQ_MEASURE_LOG
      // Measure after phase correction, before amplitude balance
      static int64_t i_sum_sq_pre = 0;
      static int64_t q_sum_sq_pre = 0;
      static int64_t iq_cross_pre = 0;
      static uint32_t measure_count = 0;

      i_sum_sq_pre += (int32_t)iClean * iClean;
      q_sum_sq_pre += (int32_t)qClean * qClean;
      iq_cross_pre += (int32_t)iClean * qClean;
      measure_count++;
    #endif
      
      // THEN apply amplitude balance
      float iq_balance = ui_get_iq_balance();
      qClean = (int16_t)(qClean * iq_balance);
      
#if IQ_MEASURE_LOG
      if(measure_count >= 8000) {  // Every second at 8kHz
        float i_rms_pre = sqrtf(i_sum_sq_pre / (float)measure_count);
        float q_rms_pre = sqrtf(q_sum_sq_pre / (float)measure_count);
        float ratio_pre = q_rms_pre / (i_rms_pre + 0.001f);

        // Phase estimate from I*Q correlation (after phase correction)
        float iq_corr_pre = iq_cross_pre / (float)measure_count;
        float phase_pre = asinf(iq_corr_pre / (i_rms_pre * q_rms_pre + 0.001f)) * 57.2958f;

        // Post-balance amplitude ratio (apply balance factor to Q RMS)
        float ratio_post = (q_rms_pre * iq_balance) / (i_rms_pre + 0.001f);

        Serial.printf("[After Phase Correction] Amp ratio=%.3f, Phase deviation=%.1f deg\n",
                      ratio_pre, phase_pre);
        Serial.printf("[After Balance] Amp ratio=%.3f\n", ratio_post);
        Serial.printf("[Settings] IQ Bal=%d, IQ Phase=%d\n",
                      (int)(iq_balance * 100), phase_deg);

        // Reset accumulators
        i_sum_sq_pre = 0;
        q_sum_sq_pre = 0;
        iq_cross_pre = 0;
        measure_count = 0;
      }
#endif

      // Get current mode from UI
      UiMode uiMode = ui_get_mode();
      DemodMode demodMode = ui_mode_to_demod_mode(uiMode);
      
      // Apply Hilbert transform and demodulation
      // Note: demod_process handles the sign conventions internally
      int16_t audioSample = demod_process(iClean, qClean, demodMode);

      // Simple AM-only AGC to reduce overload distortion
      if (demodMode == DEMOD_AM && ui_get_agc()) {
        static float agc_env = 0.0f;
        static float agc_gain = 1.0f;
        const float x = fabsf((float)audioSample);
        const float attack = 0.35f;
        const float decay = 0.01f;
        if (x > agc_env) agc_env += (x - agc_env) * attack;
        else agc_env += (x - agc_env) * decay;

        const float target = 4000.0f;
        float g = target / (agc_env + 1.0f);
        if (g > 5.0f) g = 5.0f;
        if (g < 0.05f) g = 0.05f;
        agc_gain += (g - agc_gain) * 0.05f;
        audioSample = (int16_t)(audioSample * agc_gain);
      }

      // Apply UI-selected audio filter (SSB/AM/FM shared; CW narrow filters when selected)
      int8_t filt = ui_get_filter();
      int8_t cw_tone = ui_get_cw_tone();
      int8_t volume = ui_get_volume();
      audioSample = audio_filter_apply(audioSample, filt, cw_tone, volume);
      
      // Scale to reasonable audio level
      audioSample = audioSample >> 1;  // Divide by 2
      
      // Clip to prevent overflow
      if (audioSample > 32767) audioSample = 32767;
      if (audioSample < -32768) audioSample = -32768;
      
      // Output at 8kHz (4kHz bandwidth)
      if (audio_i2s_write_sample(audioSample)) {
        audioSampleCount++;
      }
    }
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

  // Load persisted UI settings before programming the synth
  ui_load_settings();

  Serial.printf("Apply initial SI5351 programming based on UI state at startup.\r\n");
  const UiMode uiModeNow = ui_get_mode();
  Si5351RxSynthState now = {
    (uint32_t)ui_get_sifxtal(),
    ui_get_vfo_freq(),
    ui_mode_to_si5351_rx_mode(uiModeNow),
    ui_get_rit(),
    ui_get_rit_active(),
    ui_get_cw_offset(),
    ui_get_iq_phase(),
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
    ui_get_iq_phase(),
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
