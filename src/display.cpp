#include "display.h"
#include <Wire.h>

Display::Display(uint8_t sdaPin, uint8_t sclPin, uint8_t address)
  : _sda(sdaPin), _scl(sclPin), _address(address), _oled(kWidth, kHeight, &Wire, kResetPin) {}

bool Display::begin() {
  if (!_oled.begin(SSD1306_SWITCHCAPVCC, _address)) {
    return false;
  }

  _oled.clearDisplay();
  _oled.display();
  return true;
}

void Display::clear() {
  _oled.clearDisplay();
}

void Display::flush() {
  _oled.display();
}

void Display::setCursor(int16_t x, int16_t y) {
  _oled.setCursor(x, y);
}

void Display::setTextSize(uint8_t size) {
  _oled.setTextSize(size);
}

void Display::setTextColor(uint16_t color) {
  _oled.setTextColor(color);
}

Adafruit_SSD1306& Display::raw() {
  return _oled;
}

void Display::showTestPattern() {
  _oled.clearDisplay();
  _oled.setTextSize(1);
  _oled.setTextColor(SSD1306_WHITE);
  _oled.setCursor(0, 0);
  _oled.println(F("SSD1309 OLED Test"));
  _oled.println();
  _oled.println(F("ESP32-S3"));
  _oled.println(F("I2C: custom pins"));
  _oled.print(F("Addr: 0x"));
  _oled.println(_address, HEX);
  _oled.display();
}

void Display::showCounter(uint32_t counter) {
  _oled.fillRect(0, 56, 128, 8, SSD1306_BLACK);
  _oled.setCursor(0, 56);
  _oled.print(F("Count: "));
  _oled.print(counter);
  _oled.display();
}
