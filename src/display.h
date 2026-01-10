#pragma once

#include <Arduino.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

class Display {
public:
  // Construct with I2C pins and address (7-bit)
  Display(uint8_t sdaPin, uint8_t sclPin, uint8_t address = 0x3C);

  // Initialize I2C and OLED. Returns true on success.
  bool begin();

  // Show static test info
  void showTestPattern();

  // Update a counter line (bottom row)
  void showCounter(uint32_t counter);

  // Minimal compatibility helpers for the legacy LCD-style calls used in main.ori
  void clear();
  void flush();
  void setCursor(int16_t x, int16_t y);
  void setTextSize(uint8_t size);
  void setTextColor(uint16_t color);
  void noCursor() {}   // Not supported on SSD1306; kept for compatibility
  void cursor()  {}   // Not supported on SSD1306; kept for compatibility

  // Provide access to the underlying display for direct printing/drawing
  Adafruit_SSD1306& raw();

private:
  static constexpr uint8_t kWidth = 128;
  static constexpr uint8_t kHeight = 64;
  static constexpr int8_t kResetPin = -1; // no reset pin

  uint8_t _sda;
  uint8_t _scl;
  uint8_t _address;

  Adafruit_SSD1306 _oled;
};
