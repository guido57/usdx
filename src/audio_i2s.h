// audio_i2s.h - I2S audio output for ESP32
// Supports MAX98357 I2S amplifier

#pragma once

#include <stdint.h>
#include <stddef.h>

// Initialize I2S audio output
// Returns true on success
bool audio_i2s_setup();

// Write a single audio sample (blocking with timeout)
// sample: int16_t audio sample (±32767 range)
// Returns true if written successfully
bool audio_i2s_write_sample(int16_t sample);

// Write multiple audio samples (blocking with timeout)
// samples: array of int16_t audio samples
// count: number of samples
// Returns number of samples actually written
size_t audio_i2s_write_samples(const int16_t* samples, size_t count);

// Get number of samples that can be written without blocking
size_t audio_i2s_available();

// Stop I2S output and release resources
void audio_i2s_stop();

// Call this regularly from loop() to keep the audio library running
void audio_i2s_loop();
