#pragma once

#include <stddef.h>
#include <stdint.h>

struct IqSample {
  int16_t i;
  int16_t q;
};

// Initializes ADC pins for I/Q sampling.
void iq_adc_setup();

// Returns true if ADC pins were successfully attached.
bool iq_adc_ready();

// Sets ADC attenuation/profile. Range is intentionally simple:
// 0 = most sensitive, 1 = medium, 2 = least sensitive.
void iq_adc_set_att_level(uint8_t level);

// Reads one I/Q sample pair.
IqSample iq_adc_read_iq();

// Returns the number of buffered I/Q pairs currently available to read.
size_t iq_adc_available();

// Returns how many I/Q pairs were dropped due to buffer overflow (consumer too slow).
uint32_t iq_adc_dropped_pairs();

// Returns how many times the ADC continuous driver reported pool overflow.
uint32_t iq_adc_pool_overflows();

// Captures N samples and prints basic stats to Serial.
void iq_adc_print_stats(size_t nSamples);
