#include <Arduino.h>
#include <Wire.h>
#include <math.h>

#include "configuration.h"
#include "si5351.h"
#include "ft8_tx.h"
#include "iq_adc.h"
#include "ui.h"
#include "audio_filter.h"
#include "audio_i2s.h"
#include "demodulator.h"
#include "cic_filter.h"
#include "wifi_config.h"
#include "rx_att_pwm.h"

// PSRAM init functions for manual initialization after I2S
extern "C" {
  esp_err_t esp_psram_init(void);
  size_t esp_psram_get_size(void);
}

static SI5351 si5351;
//FT8_TX ft8tx(si5351);

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
  
  // Process ALL available IQ samples from ADC (or internal test source)
#if AM_TEST_MODE
  static float test_car_phase = 0.0f;
  static float test_mod_phase = 0.0f;
  const float car_step = 2.0f * (float)M_PI * AM_TEST_CARRIER_HZ / 32000.0f;
  const float mod_step = 2.0f * (float)M_PI * AM_TEST_MOD_HZ / 32000.0f;
  for (uint16_t n = 0; n < 128; ++n) {
    const float env = 1.0f + AM_TEST_DEPTH * sinf(test_mod_phase);
    const float i_f = env * cosf(test_car_phase) * AM_TEST_AMPLITUDE;
    const float q_f = env * sinf(test_car_phase) * AM_TEST_AMPLITUDE;
    IqSample sample = { (int16_t)i_f, (int16_t)q_f };
    test_car_phase += car_step;
    test_mod_phase += mod_step;
    if (test_car_phase > 2.0f * (float)M_PI) test_car_phase -= 2.0f * (float)M_PI;
    if (test_mod_phase > 2.0f * (float)M_PI) test_mod_phase -= 2.0f * (float)M_PI;
    iqPairCount++;
#else
  while (iq_adc_ready() && iq_adc_available() > 0) {
    IqSample sample = iq_adc_read_iq();
    iqPairCount++;
#endif

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
      // Scale down: CIC gain is R^N = 4^3 = 64
      int16_t iDecimated = (int16_t)(cic.getOutputI() >> 6);
      int16_t qDecimated = (int16_t)(cic.getOutputQ() >> 6);

      // Get current mode from UI early (needed for AM DC handling)
      UiMode uiMode = ui_get_mode();
      DemodMode demodMode = ui_mode_to_demod_mode(uiMode);

      // DC removal (high-pass filter)
      // Uses slow-moving average to track DC component
      int16_t iClean = iDecimated;
      int16_t qClean = qDecimated;
      if (demodMode != DEMOD_AM) {
        i_dc += (((int32_t)iDecimated << 8) - i_dc) >> 8;  // Time constant ~256 samples
        q_dc += (((int32_t)qDecimated << 8) - q_dc) >> 8;
        
        // Remove DC offset
        iClean = iDecimated - (i_dc >> 8);
        qClean = qDecimated - (q_dc >> 8);
      }

      // Fractional-sample timing skew correction (IQ Delay)
      // Positive delay => delay Q by d samples; Negative delay => delay I by |d| samples
      static int16_t prevI = 0;
      static int16_t prevQ = 0;
      const int16_t rawI = iClean;
      const int16_t rawQ = qClean;
#if !AM_TEST_MODE
      const float d = ui_get_iq_delay(); // -0.50 .. +0.50
      if (d > 0.0f) {
        qClean = (int16_t)((1.0f - d) * rawQ + d * prevQ);
      } else if (d < 0.0f) {
        const float ad = -d;
        iClean = (int16_t)((1.0f - ad) * rawI + ad * prevI);
      }
#endif
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
    #if !AM_TEST_MODE
      float phase_error_rad = (90 - phase_deg) * 0.0174533f; // Convert to radians (invert correction direction)
      float cos_theta = cosf(phase_error_rad);
      float sin_theta = sinf(phase_error_rad);
      
      int16_t iPhased = (int16_t)(iClean * cos_theta - qClean * sin_theta);
      int16_t qPhased = (int16_t)(iClean * sin_theta + qClean * cos_theta);
      iClean = iPhased;
      qClean = qPhased;
    #endif
      
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
    #if !AM_TEST_MODE
      qClean = (int16_t)(qClean * iq_balance);
    #endif
      
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

      // Apply Hilbert transform and demodulation
      // Note: demod_process handles the sign conventions internally
      int16_t demodSample = demod_process(iClean, qClean, demodMode);
    #if AM_TEST_MODE
      // Verify demod output: measure 1 kHz vs 2 kHz content (pre-processing)
      static float v_cos1 = 1.0f, v_sin1 = 0.0f;
      static float v_cos2 = 1.0f, v_sin2 = 0.0f;
      static const float v_cos1_step = 0.70710678f; // cos(2*pi*1000/8000)
      static const float v_sin1_step = 0.70710678f; // sin(2*pi*1000/8000)
      static const float v_cos2_step = 0.0f;        // cos(2*pi*2000/8000)
      static const float v_sin2_step = 1.0f;        // sin(2*pi*2000/8000)
      static double v_re1 = 0.0, v_im1 = 0.0;
      static double v_re2 = 0.0, v_im2 = 0.0;
      static uint32_t v_count = 0;

      v_re1 += (double)demodSample * v_cos1;
      v_im1 += (double)demodSample * v_sin1;
      v_re2 += (double)demodSample * v_cos2;
      v_im2 += (double)demodSample * v_sin2;
      v_count++;

      float next_cos1 = v_cos1 * v_cos1_step - v_sin1 * v_sin1_step;
      float next_sin1 = v_sin1 * v_cos1_step + v_cos1 * v_sin1_step;
      v_cos1 = next_cos1;
      v_sin1 = next_sin1;

      float next_cos2 = v_cos2 * v_cos2_step - v_sin2 * v_sin2_step;
      float next_sin2 = v_sin2 * v_cos2_step + v_cos2 * v_sin2_step;
      v_cos2 = next_cos2;
      v_sin2 = next_sin2;

      if (v_count >= 8000) {
        const double p1 = v_re1 * v_re1 + v_im1 * v_im1;
        const double p2 = v_re2 * v_re2 + v_im2 * v_im2;
        const double ratio = (p2 + 1e-12) / (p1 + 1e-12);
        const double ratio_db = 10.0 * log10(ratio);
        Serial.printf("[AM Test] 1k=%.3e 2k=%.3e 2k/1k=%.2f dB\n", p1, p2, ratio_db);
        v_re1 = v_im1 = v_re2 = v_im2 = 0.0;
        v_count = 0;
      }
    #endif
      int16_t audioSample = demodSample;

      // Apply UI-selected audio filter (SSB/AM/FM shared; CW narrow filters when selected)
    #if AM_TEST_MODE
      int8_t filt = 0;
      int8_t cw_tone = 0;
      int8_t volume = 5;
    #else
      int8_t filt = ui_get_filter();
      int8_t cw_tone = ui_get_cw_tone();
      int8_t volume = ui_get_volume();
    #endif
      audioSample = audio_filter_apply(audioSample, filt, cw_tone);

      // Simple AGC after filtering/volume to reduce overload distortion
      if (ENABLE_AGC && ui_get_agc()) {
        const bool isSSB = (demodMode == DEMOD_LSB || demodMode == DEMOD_USB);
        const bool isAM = (demodMode == DEMOD_AM);
        if (isSSB) {
          static float agc_env_ssb = 0.0f;
          static float agc_gain_ssb = 1.0f;

          float &agc_env = agc_env_ssb;
          float &agc_gain = agc_gain_ssb;

          const float x = fabsf((float)audioSample);
          const float attack = 0.02f;
          const float decay = 0.002f;
          if (x > agc_env) agc_env += (x - agc_env) * attack;
          else agc_env += (x - agc_env) * decay;

          const float target = 6000.0f;
          float g = target / (agc_env + 1.0f);
          // SSB AGC with moderate boost
          if (g > 2.5f) g = 2.5f;
          if (g < 0.1f) g = 0.1f;
          agc_gain += (g - agc_gain) * 0.01f;
          audioSample = (int16_t)(audioSample * agc_gain);
        } else if (isAM) {
          static float agc_env_am = 0.0f;
          static float agc_gain_am = 1.0f;

          float &agc_env = agc_env_am;
          float &agc_gain = agc_gain_am;

          const float x = fabsf((float)audioSample);
          const float attack = AM_AGC_ATTACK;
          const float decay = AM_AGC_DECAY;
          if (x > agc_env) agc_env += (x - agc_env) * attack;
          else agc_env += (x - agc_env) * decay;

          const float target = AM_AGC_TARGET;
          float g = target / (agc_env + 1.0f);
          if (g > AM_AGC_MAX_GAIN) g = AM_AGC_MAX_GAIN;
          if (g < AM_AGC_MIN_GAIN) g = AM_AGC_MIN_GAIN;
          agc_gain += (g - agc_gain) * 0.02f;
          audioSample = (int16_t)(audioSample * agc_gain);
        }
      }

      // Send to websocket audio stream before volume scaling
      wifi_config_audio_push(lastSynth.vfoHz, 4 * audioSample); // amplify 4x also

      // Apply volume after AGC to prevent pre-AGC overflow (volume 0-10, 2 = unity)
      int32_t volScaled = ((int32_t)audioSample * (int32_t)volume) / 2;
      if (volScaled > 32767) volScaled = 32767;
      if (volScaled < -32768) volScaled = -32768;
      audioSample = (int16_t)volScaled;

#if !AM_TEST_MODE
      // Soft limiter to avoid hard clipping distortion
      const int16_t limit = 29000;
      if (audioSample > limit) {
        audioSample = limit + (audioSample - limit) / 4;
      } else if (audioSample < -limit) {
        audioSample = -limit + (audioSample + limit) / 4;
      }
      
      // Scale to reasonable audio level
      // audioSample = audioSample >> 1;  // Divide by 2
#endif
      
      // Clip to prevent overflow
      if (audioSample > 32767) audioSample = 32767;
      if (audioSample < -32768) audioSample = -32768;
      
      // Output at 8kHz (4kHz bandwidth)
      if (audio_i2s_write_sample(audioSample)) {
        audioSampleCount++;
      }
      
    }
#if AM_TEST_MODE
  }
#else
  }
#endif
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

  // Start WiFi config (STA if credentials are valid, AP fallback if not)
  wifi_config_setup();

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

  // Initialize RX attenuation PWM control
  rxAttPwmInit();
  setRxAttFromUi(0); // Set initial attenuation to maximum 0
}

void loop() {

  uint32_t t0, dt;

  // ---------------- UI ----------------
  ui_loop();

  // ------------- Audio ---------------
  processAudioTest();

  // ----------- Synth control ----------
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

  if (synthStateChanged(now, lastSynth)) {
    si5351.fxtal = now.fxtalHz;
    programSi5351Rx(si5351, now);
    lastSynth = now;
  }

  // ------------- ADC ATT --------------
  // iq_adc_set_att_level((uint8_t)ui_get_att());
  iq_adc_set_att_level(0); // ADC attenuation always 0 dB
  setRxAttFromUi((uint8_t)ui_get_att_rf()); // Set RF attenuation from UI

    // Example trigger from serial
  // if (Serial.available()) {
  //     Serial.read();

  //     if (ft8tx.requestTransmission("IW5ALZ K1ABC JN89"))
  //         Serial.println("FT8 scheduled for next slot");
  //     else
  //         Serial.println("TX busy or encode error");
  // }

  // Your DSP / ADC processing can block here safely
  //delay(200);
   
}
