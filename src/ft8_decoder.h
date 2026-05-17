#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Initialize FT8 decoder
void ft8_decoder_init();

// Process incoming audio samples (typically 8kHz, 16-bit)
// Called from the audio pipeline with samples at regular intervals
void ft8_decoder_add_samples(const int16_t* samples, int num_samples);

// Check if a decode is ready (called periodically)
// Returns true if new messages were decoded
bool ft8_decoder_process();

// Structure for decoded FT8 message
typedef struct {
    float snr;              // Signal-to-noise ratio (dB)
    float freq;             // Frequency offset from expected (Hz)
    float time_offset_sec;  // Timing offset (seconds)
    char message[30];       // Decoded message text
    bool valid;             // Message validity flag
} ft8_decoded_msg_t;

// Get the last decoded message
// Returns pointer to message, or NULL if no valid message
const ft8_decoded_msg_t* ft8_decoder_get_last_message();

// Get number of messages decoded in current slot
int ft8_decoder_get_message_count();

// Reset decoder for next slot
void ft8_decoder_reset_slot();

// Get current UTC time fraction for timing
double ft8_decoder_get_utc_frac();

#ifdef __cplusplus
}
#endif
