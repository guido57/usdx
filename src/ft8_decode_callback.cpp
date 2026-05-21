#include <Arduino.h>
#include <math.h>
#include <string.h>
#include <time.h>

#include "ft8_decode/decode.h"
#include "qso_manager.h"
#include "ui.h"

extern QSOManager qsoManager;

static int8_t clamp_snr_to_i8(float snr_db)
{
    int snr = (int)lroundf(snr_db);
    if (snr > 127) {
        snr = 127;
    }
    if (snr < -128) {
        snr = -128;
    }
    return (int8_t)snr;
}

extern "C" void ft8_on_message_decoded(const char* phase,
                                        const struct tm* utc,
                                        double tbase_sec,
                                        float base_freq_mhz,
                                        const message_t* msg)
{
    if (phase == nullptr || utc == nullptr || msg == nullptr) {
        return;
    }

    // if(ui_get_ws_server_enabled()) {
    //     return;
    // }

    // Final pass only; checkpoint pass is intentionally ignored.
    // if (strcmp(phase, "final") != 0) {
    //     return;
    // }

    Ft8Spot spot{};

    strlcpy(spot.decoded_line, msg->text, sizeof(spot.decoded_line));
    strlcpy(spot.mode, "FT8", sizeof(spot.mode));

    const char* mycall = ui_get_mycall();
    const char* mygrid = ui_get_mygrid();
    strlcpy(spot.receiver_callsign, (mycall != nullptr) ? mycall : "", sizeof(spot.receiver_callsign));
    strlcpy(spot.receiver_grid, (mygrid != nullptr) ? mygrid : "", sizeof(spot.receiver_grid));

    float full_freq_hz = 1.0e6f * base_freq_mhz + msg->freq_hz;
    if (full_freq_hz < 0.0f) {
        full_freq_hz = 0.0f;
    }
    spot.freq_hz = (uint32_t)lroundf(full_freq_hz);
    spot.snr_db = clamp_snr_to_i8(msg->snr_db);

    // Keep timestamp aligned with current system UTC and FT8 slot offset when available.
    (void)utc;
    (void)tbase_sec;
    spot.timestamp = (uint32_t)time(nullptr);

    spot.cq = (strncmp(msg->text, "CQ ", 3) == 0) || (strcmp(msg->text, "CQ") == 0);
    spot.directed_to_me = (mycall != nullptr && mycall[0] != '\0' && strstr(msg->text, mycall) != nullptr);

    qsoManager.processFt8Spot(spot);
}
