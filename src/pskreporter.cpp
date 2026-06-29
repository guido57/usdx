#include "pskreporter.h"

#include <cstring>
#include <ctime>

#include "ui.h"

#if defined(ARDUINO_ARCH_ESP32)
#include <WiFi.h>
#include <WiFiUdp.h>
#include <esp_system.h>
#endif

namespace {


constexpr size_t kMaxPacketSize = 512;

static uint8_t g_receiver_id[4] = {0};
static bool g_receiver_id_initialized = false;
static uint32_t g_sequence = 0;
static WiFiUDP g_psk_udp;
static bool g_psk_udp_initialized = false;

static size_t append_u8(uint8_t* dst, size_t offset, size_t capacity, uint8_t value)
{
    if (offset >= capacity) {
        return capacity;
    }
    dst[offset++] = value;
    return offset;
}

static size_t append_be16(uint8_t* dst, size_t offset, size_t capacity, uint16_t value)
{
    offset = append_u8(dst, offset, capacity, (uint8_t)((value >> 8) & 0xFF));
    offset = append_u8(dst, offset, capacity, (uint8_t)(value & 0xFF));
    return offset;
}

static size_t append_be32(uint8_t* dst, size_t offset, size_t capacity, uint32_t value)
{
    offset = append_u8(dst, offset, capacity, (uint8_t)((value >> 24) & 0xFF));
    offset = append_u8(dst, offset, capacity, (uint8_t)((value >> 16) & 0xFF));
    offset = append_u8(dst, offset, capacity, (uint8_t)((value >> 8) & 0xFF));
    offset = append_u8(dst, offset, capacity, (uint8_t)(value & 0xFF));
    return offset;
}

static size_t append_string(uint8_t* dst, size_t offset, size_t capacity, const char* text)
{
    if (text == nullptr) {
        text = "";
    }

    size_t len = strnlen(text, 255);
    offset = append_u8(dst, offset, capacity, (uint8_t)len);
    if (offset + len > capacity) {
        return capacity;
    }
    if (len > 0) {
        memcpy(dst + offset, text, len);
    }
    return offset + len;
}

static size_t pad_to_4(uint8_t* dst, size_t offset, size_t capacity)
{
    const size_t pad = (4 - (offset % 4)) % 4;
    if (offset + pad > capacity) {
        return capacity;
    }
    if (pad > 0) {
        memset(dst + offset, 0, pad);
    }
    return offset + pad;
}

static bool is_non_empty(const char* text)
{
    return text != nullptr && text[0] != '\0';
}

static bool is_plausible_amateur_freq_hz(uint32_t f)
{
    // Common amateur allocations where PSK/FT8 reports are expected.
    return
        (f >= 135700U && f <= 137800U) ||
        (f >= 472000U && f <= 479000U) ||
        (f >= 1800000U && f <= 2000000U) ||
        (f >= 3500000U && f <= 4000000U) ||
        (f >= 5000000U && f <= 5500000U) ||
        (f >= 7000000U && f <= 7300000U) ||
        (f >= 10100000U && f <= 10150000U) ||
        (f >= 14000000U && f <= 14350000U) ||
        (f >= 18068000U && f <= 18168000U) ||
        (f >= 21000000U && f <= 21450000U) ||
        (f >= 24890000U && f <= 24990000U) ||
        (f >= 28000000U && f <= 29700000U) ||
        (f >= 50000000U && f <= 54000000U) ||
        (f >= 70000000U && f <= 71000000U) ||
        (f >= 144000000U && f <= 148000000U) ||
        (f >= 222000000U && f <= 225000000U) ||
        (f >= 420000000U && f <= 450000000U);
}


static void init_receiver_id()
{
    if (g_receiver_id_initialized) {
        return;
    }

#if defined(ARDUINO_ARCH_ESP32)
    uint32_t r = esp_random();
#else
    uint32_t r = (uint32_t)std::time(nullptr);
#endif

    g_receiver_id[0] = (uint8_t)((r >> 24) & 0xFF);
    g_receiver_id[1] = (uint8_t)((r >> 16) & 0xFF);
    g_receiver_id[2] = (uint8_t)((r >> 8) & 0xFF);
    g_receiver_id[3] = (uint8_t)(r & 0xFF);
    g_receiver_id_initialized = true;
}

static bool send_udp_packet(const uint8_t* packet,
                            size_t packet_len,
                            const char* host,
                            uint16_t port,
                            uint32_t src_freq_hz)
{
    if (!g_psk_udp_initialized) {
        // Keep a stable UDP source port for the session as recommended by PSK Reporter.
        g_psk_udp_initialized = (g_psk_udp.begin(0) == 1);
        if (!g_psk_udp_initialized) {
            Serial.println("[psk] udp begin failed");
            return false;
        }
    }

    if (!g_psk_udp.beginPacket(host, port)) {
        Serial.printf("[psk] beginPacket failed host=%s port=%u\r\n", host, (unsigned)port);
        return false;
    }

    const size_t written = g_psk_udp.write(packet, packet_len);
    const int rc = g_psk_udp.endPacket();
    if (written != packet_len || rc == 0) {
        Serial.printf("[psk] send failed: host=%s port=%u wrote=%u/%u rc=%d\r\n",
                      host,
                      (unsigned)port,
                      (unsigned)written,
                      (unsigned)packet_len,
                      rc);
        return false;
    }

    // Serial.printf("[psk] send ok: host=%s port=%u wrote=%u src.freq=%u tx.freq=%u\r\n",
    //               host,
    //               (unsigned)port,
    //               (unsigned)written,
    //               (unsigned)src_freq_hz);
    return true;
}

} // namespace

void send_pskreporter_packet(const Ft8Spot& spot)
{
#if !defined(ARDUINO_ARCH_ESP32)
    (void)spot;
    return;
#else
    init_receiver_id();

    const char* receiver_callsign = ui_get_mycall();
    const char* receiver_grid = ui_get_mygrid();
    const char* decoding_software = ui_get_mysoftware();
    const char* antenna_description = ui_get_myantenna();
    const char* callsign = spot.callsign;
    const char* locator = spot.grid;
    const char* mode = is_non_empty(spot.mode) ? spot.mode : "FT8";

    if (!is_non_empty(receiver_callsign) || !is_non_empty(callsign)) {
        Serial.println("[psk] skip spot: missing receiver or station callsign");
        return;
    }

    // Use fresh template IDs to prevent any stale server-side template cache
    // from decoding this packet with an older field layout.
    const uint16_t sender_set_id = (uint16_t)(0xA000U | (g_sequence & 0x0FFFU));
    const uint16_t receiver_set_id = (uint16_t)(sender_set_id - 1U);

    uint8_t receiver_body[128];
    size_t receiver_body_len = 0;
    receiver_body_len = append_string(receiver_body, receiver_body_len, sizeof(receiver_body), receiver_callsign);
    receiver_body_len = append_string(receiver_body, receiver_body_len, sizeof(receiver_body), receiver_grid);
    receiver_body_len = append_string(receiver_body, receiver_body_len, sizeof(receiver_body), decoding_software);
    receiver_body_len = append_string(receiver_body, receiver_body_len, sizeof(receiver_body), antenna_description);
    //receiver_body_len = append_string(receiver_body, receiver_body_len, sizeof(receiver_body), rig_description);
    receiver_body_len = pad_to_4(receiver_body, receiver_body_len, sizeof(receiver_body));
    if (receiver_body_len == sizeof(receiver_body)) {
        Serial.println("[psk] skip spot: receiver block overflow");
        return;
    }
    // else
    //     Serial.printf("[psk] receiver block: %s\r\n", (char *) receiver_body);

    uint8_t sender_body[160];
    size_t sender_body_len = 0;
    sender_body_len = append_string(sender_body, sender_body_len, sizeof(sender_body), callsign);
    sender_body_len = append_be32(sender_body, sender_body_len, sizeof(sender_body), spot.freq_hz);
    sender_body_len = append_u8(sender_body, sender_body_len, sizeof(sender_body), (uint8_t)spot.snr_db);
    sender_body_len = append_u8(sender_body, sender_body_len, sizeof(sender_body), 0x80); // iMD unknown/null
    sender_body_len = append_string(sender_body, sender_body_len, sizeof(sender_body), mode);
    sender_body_len = append_u8(sender_body, sender_body_len, sizeof(sender_body), 0x01);
    sender_body_len = append_string(sender_body, sender_body_len, sizeof(sender_body), locator);
    sender_body_len = append_be32(sender_body, sender_body_len, sizeof(sender_body), spot.timestamp);
    sender_body_len = pad_to_4(sender_body, sender_body_len, sizeof(sender_body));
    if (sender_body_len == sizeof(sender_body)) {
        Serial.println("[psk] skip spot: sender block overflow");
        return;
    }

    const uint8_t receiver_header[] = {
        0x00, 0x03, 0x00, 0x2C,
        (uint8_t)((receiver_set_id >> 8) & 0xFF), (uint8_t)(receiver_set_id & 0xFF),
        0x00, 0x04, 0x00, 0x01,
        0x80, 0x02, 0xFF, 0xFF, 0x00, 0x00, 0x76, 0x8F,
        0x80, 0x04, 0xFF, 0xFF, 0x00, 0x00, 0x76, 0x8F,
        0x80, 0x08, 0xFF, 0xFF, 0x00, 0x00, 0x76, 0x8F,
        0x80, 0x09, 0xFF, 0xFF, 0x00, 0x00, 0x76, 0x8F,
        0x00, 0x00
    };

    const uint8_t sender_header[] = {
        0x00, 0x02, 0x00, 0x44,
        (uint8_t)((sender_set_id >> 8) & 0xFF), (uint8_t)(sender_set_id & 0xFF),
        0x00, 0x08,
        0x80, 0x01, 0xFF, 0xFF, 0x00, 0x00, 0x76, 0x8F,
        0x80, 0x05, 0x00, 0x04, 0x00, 0x00, 0x76, 0x8F,
        0x80, 0x06, 0x00, 0x01, 0x00, 0x00, 0x76, 0x8F,
        0x80, 0x07, 0x00, 0x01, 0x00, 0x00, 0x76, 0x8F,
        0x80, 0x0A, 0xFF, 0xFF, 0x00, 0x00, 0x76, 0x8F,
        0x80, 0x0B, 0x00, 0x01, 0x00, 0x00, 0x76, 0x8F,
        0x80, 0x03, 0xFF, 0xFF, 0x00, 0x00, 0x76, 0x8F,
        0x00, 0x96, 0x00, 0x04
    };

    const size_t receiver_info_len = 4 + receiver_body_len;
    const size_t sender_info_len = 4 + sender_body_len;
    const size_t total_length = 16 + sizeof(receiver_header) + sizeof(sender_header) + receiver_info_len + sender_info_len;

    if (total_length > kMaxPacketSize) {
        Serial.println("[psk] skip spot: packet too large");
        return;
    }

    uint8_t packet[kMaxPacketSize];
    size_t offset = 0;

    offset = append_u8(packet, offset, sizeof(packet), 0x00);
    offset = append_u8(packet, offset, sizeof(packet), 0x0A);
    offset = append_be16(packet, offset, sizeof(packet), (uint16_t)total_length);
    offset = append_be32(packet, offset, sizeof(packet), (uint32_t)time(nullptr));
    offset = append_be32(packet, offset, sizeof(packet), g_sequence++);
    if (offset + sizeof(g_receiver_id) > sizeof(packet)) {
        Serial.println("[psk] skip spot: header overflow");
        return;
    }
    memcpy(packet + offset, g_receiver_id, sizeof(g_receiver_id));
    offset += sizeof(g_receiver_id);

    if (offset + sizeof(receiver_header) + sizeof(sender_header) + receiver_info_len + sender_info_len > sizeof(packet)) {
        Serial.println("[psk] skip spot: packet assembly overflow");
        return;
    }

    memcpy(packet + offset, receiver_header, sizeof(receiver_header));
    offset += sizeof(receiver_header);

    memcpy(packet + offset, sender_header, sizeof(sender_header));
    offset += sizeof(sender_header);

    packet[offset++] = (uint8_t)((receiver_set_id >> 8) & 0xFF);
    packet[offset++] = (uint8_t)(receiver_set_id & 0xFF);
    offset = append_be16(packet, offset, sizeof(packet), (uint16_t)(receiver_body_len + 4));
    memcpy(packet + offset, receiver_body, receiver_body_len);
    offset += receiver_body_len;

    packet[offset++] = (uint8_t)((sender_set_id >> 8) & 0xFF);
    packet[offset++] = (uint8_t)(sender_set_id & 0xFF);
    offset = append_be16(packet, offset, sizeof(packet), (uint16_t)(sender_body_len + 4));
    memcpy(packet + offset, sender_body, sender_body_len);
    offset += sender_body_len;

    const bool sent_prod = send_udp_packet(packet,
                                           offset,
                                           "report.pskreporter.info",
                                           4739,
                                           spot.freq_hz);

    // const bool sent_analyzer = send_udp_packet(packet,
    //                                            offset,
    //                                            "pskreporter.info",
    //                                            14739,
    //                                            spot.freq_hz);

    // Serial.printf("[psk] tx summary: prod=%s analyzer=%s\r\n",
    //               sent_prod ? "ok" : "fail",
    //               sent_analyzer ? "ok" : "fail");

    // Serial.printf("tx.freq bytes %02X %02X %02X %02X\r\n",
    //               spot.freq_hz & 0xFF,
    //               (spot.freq_hz >> 8) & 0xFF,
    //               (spot.freq_hz >> 16) & 0xFF,
    //               (spot.freq_hz >> 24) & 0xFF);
    // for(size_t i = 0; i < offset; ++i) {
    //     Serial.printf("%02X %c ", packet[i], (packet[i] >= 32 && packet[i] <= 126) ? packet[i] : '.');
    // }
    // Serial.println();

#endif
}