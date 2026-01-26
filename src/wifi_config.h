#pragma once

#include <cstdint>

// Initialize WiFi in STA mode (using saved credentials) or fallback to AP mode.
void wifi_config_setup();

// Handle web server clients (call in loop).
void wifi_config_loop();

// Stream audio sample (16-bit PCM @ 8kHz) to WebSocket clients.
void wifi_config_audio_push(int32_t freq_hz, int16_t sample);
