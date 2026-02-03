#pragma once

#include <cstdint>

// Initialize WiFi in STA mode (using saved credentials) or fallback to AP mode.
void wifi_config_setup();

// Handle web server clients (call in loop).
void wifi_config_loop();

// Stream audio sample (16-bit PCM @ 8kHz) to WebSocket clients.
void wifi_config_audio_push(int32_t freq_hz, int16_t sample);


// ===== FT8 =====
static bool ft8_running = false;
struct Ft8Spot {
    char      decoded_line[64];          // Full decoded message line
    char      receiver_callsign[12];     // "SO3WWA"
    char      receiver_grid[8];          // "JO01"
    char      decoding_software[16];     // "WSJT-X 2.5.4"
    char      antenna_description[24];   // "Vertical"
    char      callsign[12];              // "K1ABC"
    char      grid[8];                   // "FN31"
    char      report[12];                 // "-10"
    uint32_t  freq_hz;                   // 7074000
    int8_t    snr_db;                    // -24 … +10 dB fits in int8
    char      mode[4];                   // "FT8"
    uint32_t  timestamp;                 // Unix UTC seconds (FT8 slot time)
    bool      cq;                     // true if message was CQ
    bool      directed_to_me;            // true if MYCALL present
};

// Global WiFi status variables
extern uint8_t g_wifiBars;
extern int     g_wifiRssi;
extern bool    g_wifiConnected;
extern bool    g_wifiReconnecting;
extern bool    g_wifiWarning;

// ===== FT8 server status variables =====
extern bool     g_ft8ServerConnected;
extern bool     g_ft8ServerActive;     // data flowing
extern uint32_t g_ft8LastRxMs;         // last FT8 message time
