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

constexpr const char kPskReporterHost[] = "report.pskreporter.info";
constexpr uint16_t kPskReporterPort = 4739;
constexpr size_t kMaxPacketSize = 512;

static uint8_t g_receiver_id[4] = {0};
static bool g_receiver_id_initialized = false;
static uint32_t g_sequence = 0;

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

static size_t append_be16_signed(uint8_t* dst, size_t offset, size_t capacity, int16_t value)
{
    return append_be16(dst, offset, capacity, (uint16_t)value);
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

static size_t append_bytes(uint8_t* dst, size_t offset, size_t capacity, const uint8_t* bytes, size_t len)
{
    if (len > 255) {
        len = 255;
    }

    offset = append_u8(dst, offset, capacity, (uint8_t)len);
    if (offset + len > capacity) {
        return capacity;
    }
    if (len > 0 && bytes != nullptr) {
        memcpy(dst + offset, bytes, len);
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

} // namespace

void send_pskreporter_packet(const Ft8Spot& spot)
{
#if !defined(ARDUINO_ARCH_ESP32)
    (void)spot;
    return;
#else
    init_receiver_id();

    const char* receiver_callsign = is_non_empty(spot.receiver_callsign) ? spot.receiver_callsign : ui_get_mycall();
    const char* receiver_grid = is_non_empty(spot.receiver_grid) ? spot.receiver_grid : ui_get_mygrid();
    const char* decoding_software = is_non_empty(spot.decoding_software) ? spot.decoding_software : "usdx";
    const char* antenna_description = is_non_empty(spot.antenna_description) ? spot.antenna_description : "unknown";
    const char* callsign = spot.callsign;
    const char* locator = spot.grid;
    const char* mode = is_non_empty(spot.mode) ? spot.mode : "FT8";

    if (!is_non_empty(receiver_callsign) || !is_non_empty(callsign)) {
        Serial.println("[psk] skip spot: missing receiver or station callsign");
        return;
    }

    uint8_t receiver_body[128];
    size_t receiver_body_len = 0;
    receiver_body_len = append_string(receiver_body, receiver_body_len, sizeof(receiver_body), receiver_callsign);
    receiver_body_len = append_string(receiver_body, receiver_body_len, sizeof(receiver_body), receiver_grid);
    receiver_body_len = append_string(receiver_body, receiver_body_len, sizeof(receiver_body), decoding_software);
    receiver_body_len = append_string(receiver_body, receiver_body_len, sizeof(receiver_body), antenna_description);
    receiver_body_len = pad_to_4(receiver_body, receiver_body_len, sizeof(receiver_body));
    if (receiver_body_len == sizeof(receiver_body)) {
        Serial.println("[psk] skip spot: receiver block overflow");
        return;
    }

    uint8_t sender_body[160];
    size_t sender_body_len = 0;
    sender_body_len = append_string(sender_body, sender_body_len, sizeof(sender_body), callsign);
    sender_body_len = append_be32(sender_body, sender_body_len, sizeof(sender_body), spot.freq_hz);
    sender_body_len = append_u8(sender_body, sender_body_len, sizeof(sender_body), (uint8_t)spot.snr_db);
    sender_body_len = append_string(sender_body, sender_body_len, sizeof(sender_body), mode);
    sender_body_len = append_string(sender_body, sender_body_len, sizeof(sender_body), locator);
    sender_body_len = append_u8(sender_body, sender_body_len, sizeof(sender_body), 0x01);
    sender_body_len = append_be32(sender_body, sender_body_len, sizeof(sender_body), spot.timestamp);
    sender_body_len = append_bytes(sender_body, sender_body_len, sizeof(sender_body), nullptr, 0);
    sender_body_len = append_be16_signed(sender_body, sender_body_len, sizeof(sender_body), 0);
    sender_body_len = pad_to_4(sender_body, sender_body_len, sizeof(sender_body));
    if (sender_body_len == sizeof(sender_body)) {
        Serial.println("[psk] skip spot: sender block overflow");
        return;
    }

    static const uint8_t receiver_header[] = {
        0x00, 0x03, 0x00, 0x2C,
        0x99, 0x92,
        0x00, 0x04, 0x00, 0x01,
        0x80, 0x02, 0xFF, 0xFF, 0x00, 0x00, 0x76, 0x8F,
        0x80, 0x04, 0xFF, 0xFF, 0x00, 0x00, 0x76, 0x8F,
        0x80, 0x08, 0xFF, 0xFF, 0x00, 0x00, 0x76, 0x8F,
        0x80, 0x09, 0xFF, 0xFF, 0x00, 0x00, 0x76, 0x8F,
        0x00, 0x00
    };

    static const uint8_t sender_header[] = {
        0x00, 0x02, 0x00, 0x4C,
        0x99, 0x96,
        0x00, 0x09,
        0x80, 0x01, 0xFF, 0xFF, 0x00, 0x00, 0x76, 0x8F,
        0x80, 0x05, 0x00, 0x04, 0x00, 0x00, 0x76, 0x8F,
        0x80, 0x06, 0x00, 0x01, 0x00, 0x00, 0x76, 0x8F,
        0x80, 0x0A, 0xFF, 0xFF, 0x00, 0x00, 0x76, 0x8F,
        0x80, 0x03, 0xFF, 0xFF, 0x00, 0x00, 0x76, 0x8F,
        0x80, 0x0B, 0x00, 0x01, 0x00, 0x00, 0x76, 0x8F,
        0x00, 0x96, 0x00, 0x04,
        0x80, 0x0E, 0xFF, 0xFF, 0x00, 0x00, 0x76, 0x8F,
        0x80, 0x0F, 0x00, 0x02, 0x00, 0x00, 0x76, 0x8F
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

    packet[offset++] = 0x99;
    packet[offset++] = 0x92;
    offset = append_be16(packet, offset, sizeof(packet), (uint16_t)(receiver_body_len + 4));
    memcpy(packet + offset, receiver_body, receiver_body_len);
    offset += receiver_body_len;

    packet[offset++] = 0x99;
    packet[offset++] = 0x96;
    offset = append_be16(packet, offset, sizeof(packet), (uint16_t)(sender_body_len + 4));
    memcpy(packet + offset, sender_body, sender_body_len);
    offset += sender_body_len;

    WiFiUDP udp;
    if (!udp.beginPacket(kPskReporterHost, kPskReporterPort)) {
        Serial.println("[psk] beginPacket failed");
        return;
    }

    const size_t written = udp.write(packet, offset);
    const int rc = udp.endPacket();
    if (written != offset || rc == 0) {
        Serial.printf("[psk] send failed: wrote=%u/%u rc=%d\n", (unsigned)written, (unsigned)offset, rc);
        return;
    }

    // Serial.printf("[psk] sent spot %s @ %u Hz SNR %d\n", callsign, (unsigned)spot.freq_hz, (int)spot.snr_db);
#endif
}