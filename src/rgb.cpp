#include "rgb.h"
#include <Adafruit_NeoPixel.h>

#define RGB_PIN 48

static Adafruit_NeoPixel pixel(1, RGB_PIN, NEO_GRB + NEO_KHZ800);

#undef RGB_BRIGHTNESS
#define RGB_BRIGHTNESS 50

namespace
{
    // ---------------- system state ----------------
    bool ethOK = false;
    bool ipOK = false;
    bool wsOK = false;
    bool siOK = false;
    bool mcpOK = false;
    bool tx = false;

    // ---------------- FT8 flash ----------------
    bool decodeFlash = false;
    uint32_t decodeStart = 0;

    // ---------------- blink engine ----------------
    bool blinkState = true;
    uint32_t blinkTimer = 0;

    // ---------------- helper ----------------
    void show(uint8_t r, uint8_t g, uint8_t b)
    {
        pixel.setPixelColor(0, pixel.Color(r, g, b));
        pixel.show();
    }

    void showBlink(uint8_t r, uint8_t g, uint8_t b)
    {
        if (!blinkState)
        {
            show(0, 0, 0);
            return;
        }
        show(r, g, b);
    }
}

// ============================================================
// PUBLIC API
// ============================================================

namespace RGB
{

void begin()
{
    pixel.begin();
    pixel.clear();
    pixel.show();
}

void ethernetLink(bool up) { ethOK = up; }
void ipAssigned(bool assigned) { ipOK = assigned; }
void websocketConnected(bool connected) { wsOK = connected; }
void si5351(bool ok) { siOK = ok; }
void mcp23017(bool ok) { mcpOK = ok; }
void transmitting(bool on) { tx = on; }

void ft8Decoded()
{
    decodeFlash = true;
    decodeStart = millis();
}

void startupTest()
{
    show(RGB_BRIGHTNESS, 0, 0); delay(120);
    show(RGB_BRIGHTNESS, RGB_BRIGHTNESS, 0); delay(120);
    show(RGB_BRIGHTNESS / 2, 0, RGB_BRIGHTNESS); delay(120);
    show(RGB_BRIGHTNESS, 0, RGB_BRIGHTNESS); delay(120);
    show(0, 0, RGB_BRIGHTNESS); delay(120);
    show(0, RGB_BRIGHTNESS, 0); delay(120);
    show(0, 0, 0);
}

void update()
{
    uint32_t now = millis();

    // ---------------- blink engine ----------------
    if (now - blinkTimer > 500)
    {
        blinkTimer = now;
        blinkState = !blinkState;
    }

    // ---------------- FT8 decode flash ----------------
    if (decodeFlash && (now - decodeStart < 2000))
    {
        show(0, RGB_BRIGHTNESS, 0);
        return;
    }
    decodeFlash = false;

    // ---------------- TX override ----------------
    if (tx)
    {
        show(RGB_BRIGHTNESS, RGB_BRIGHTNESS / 3, 0); // orange
        return;
    }

    // ---------------- fault chain (priority order) ----------------
    if (!ethOK) { showBlink(RGB_BRIGHTNESS, 0, 0); return; }        // red
    if (!ipOK)  { showBlink(RGB_BRIGHTNESS, RGB_BRIGHTNESS, 0); return; }      // yellow
    // if (!mcpOK) { showBlink(RGB_BRIGHTNESS / 2, 0, RGB_BRIGHTNESS); return; }      // purple
    if (!siOK)  { showBlink(RGB_BRIGHTNESS, 0, RGB_BRIGHTNESS); return; }      // magenta
    if (!wsOK)  { showBlink(0, 0, RGB_BRIGHTNESS); return; }       // blue
    show(0, 0, RGB_BRIGHTNESS);                                    // blue

}      

}
