#include "ft8_decoder.h"
#include "decoder_api.h"
#include <string.h>
#include <time.h>
#include <esp_timer.h>

// Configuration
#define DECODER_SAMPLE_RATE 8000  // 8 kHz audio
#define SLOT_DURATION_SEC   13    // FT8 slot duration
#define MAX_MESSAGES        10    // Max messages per slot

// Decoder state
typedef struct {
    ft8_stream_decoder_t* stream;
    ft8_decoded_msg_t messages[MAX_MESSAGES];
    int message_count;
    uint32_t sample_count;
    uint64_t slot_start_time_us;
    bool slot_active;
    bool decoder_initialized;
} decoder_state_t;

static decoder_state_t decoder_state = {0};

// Get current UTC time
static void get_utc_time(struct tm* utc, double* frac_sec) {
    time_t now = time(NULL);
    struct tm* tm_info = gmtime(&now);
    *utc = *tm_info;
    
    // Get microsecond precision
    uint64_t now_us = esp_timer_get_time();
    *frac_sec = (now_us % 1000000) / 1000000.0;
}

// Initialize decoder
void ft8_decoder_init() {
    if (decoder_state.decoder_initialized) {
        return;
    }
    
    memset(&decoder_state, 0, sizeof(decoder_state));
    decoder_state.slot_start_time_us = esp_timer_get_time();
    decoder_state.decoder_initialized = true;
    
    // Create first stream decoder instance
    struct tm utc;
    double frac_sec;
    get_utc_time(&utc, &frac_sec);
    
    ft8_decode_context_t ctx = {
        .is_ft8 = true,
        .base_freq_mhz = 14.074f,  // Default to 20m band
        .utc = utc,
        .utc_frac_sec = frac_sec
    };
    
    decoder_state.stream = ft8_stream_open(DECODER_SAMPLE_RATE, &ctx);
}

// Add samples to decoder
void ft8_decoder_add_samples(const int16_t* samples, int num_samples) {
    if (!decoder_state.decoder_initialized || !decoder_state.stream) {
        return;
    }
    
    int result = ft8_stream_append_i16(decoder_state.stream, samples, num_samples);
    decoder_state.sample_count += num_samples;
    
    // Track if we have enough samples for a slot
    if (decoder_state.sample_count >= DECODER_SAMPLE_RATE * SLOT_DURATION_SEC) {
        decoder_state.slot_active = true;
    }
}

// Process decoder and extract results
bool ft8_decoder_process() {
    if (!decoder_state.decoder_initialized || !decoder_state.stream) {
        return false;
    }
    
    uint64_t now_us = esp_timer_get_time();
    uint64_t slot_elapsed_us = now_us - decoder_state.slot_start_time_us;
    
    // Check if we should finalize this slot
    if (slot_elapsed_us >= (SLOT_DURATION_SEC * 1000000)) {
        // Finalize the current slot
        int num_candidates = ft8_stream_finalize(decoder_state.stream);
        
        // For now, just count that we processed
        // Message extraction would happen via additional API calls
        decoder_state.message_count = (num_candidates > 0) ? 1 : 0;
        
        // Close current stream and prepare next one
        ft8_stream_close(decoder_state.stream);
        
        // Reset for next slot
        struct tm utc;
        double frac_sec;
        get_utc_time(&utc, &frac_sec);
        
        ft8_decode_context_t ctx = {
            .is_ft8 = true,
            .base_freq_mhz = 14.074f,
            .utc = utc,
            .utc_frac_sec = frac_sec
        };
        
        decoder_state.stream = ft8_stream_open(DECODER_SAMPLE_RATE, &ctx);
        decoder_state.sample_count = 0;
        decoder_state.slot_start_time_us = now_us;
        decoder_state.slot_active = false;
        
        return decoder_state.message_count > 0;
    }
    
    return false;
}

// Get last decoded message
const ft8_decoded_msg_t* ft8_decoder_get_last_message() {
    if (decoder_state.message_count > 0) {
        return &decoder_state.messages[0];
    }
    return NULL;
}

// Get message count
int ft8_decoder_get_message_count() {
    return decoder_state.message_count;
}

// Reset slot
void ft8_decoder_reset_slot() {
    if (decoder_state.stream) {
        ft8_stream_close(decoder_state.stream);
    }
    
    struct tm utc;
    double frac_sec;
    get_utc_time(&utc, &frac_sec);
    
    ft8_decode_context_t ctx = {
        .is_ft8 = true,
        .base_freq_mhz = 14.074f,
        .utc = utc,
        .utc_frac_sec = frac_sec
    };
    
    decoder_state.stream = ft8_stream_open(DECODER_SAMPLE_RATE, &ctx);
    decoder_state.sample_count = 0;
    decoder_state.slot_start_time_us = esp_timer_get_time();
    decoder_state.message_count = 0;
}

// Get UTC fraction
double ft8_decoder_get_utc_frac() {
    struct tm utc;
    double frac_sec;
    get_utc_time(&utc, &frac_sec);
    return frac_sec;
}
