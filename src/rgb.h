#ifndef RGB_H
#define RGB_H

#include <Arduino.h>

namespace RGB
{
    void begin();
    void update();

    void ethernetLink(bool up);
    void ipAssigned(bool assigned);
    void websocketConnected(bool connected);
    void si5351(bool ok);
    void mcp23017(bool ok);
    void transmitting(bool on);

    void ft8Decoded();
    void startupTest();
}

#endif