#pragma once
#include <Arduino.h>

class Encoder {
public:
  // Callback function types
  typedef void (*EncoderCallback)(int8_t direction);
  typedef void (*ButtonCallback)(uint8_t buttonId, bool pressed);
  
  Encoder();
  
  // Initialize encoder_freq with pin assignments
  void begin(uint8_t rotA, uint8_t rotB, uint8_t rotSW, 
             uint8_t leftSW, uint8_t rightSW);
  
  // Set callbacks
  void onRotate(EncoderCallback callback);
  void onButton(ButtonCallback callback);
  
  // Call this in loop()
  void update();
  
  // Get current encoder position
  int16_t getPosition() const { return _position; }
  void setPosition(int16_t pos) { _position = pos; }
  
  // Button IDs
  enum Button { BTN_ENCODER = 0, BTN_LEFT = 1, BTN_RIGHT = 2 };
  
private:
  // Pin assignments
  uint8_t _pinRotA, _pinRotB, _pinRotSW, _pinLeftSW, _pinRightSW;
  
  // Encoder state
  int16_t _position;
  uint8_t _lastEncoderState;
  int8_t _encoderAccum;
  
  // Button state tracking
  struct ButtonState {
    bool lastState;
    unsigned long lastChange;
  };
  ButtonState _buttons[3];
  
  // Debounce timing
  static const unsigned long DEBOUNCE_MS = 50;
  static const unsigned long SWITCH_DEBOUNCE_MS = 200;
  
  // Callbacks
  EncoderCallback _rotateCallback;
  ButtonCallback _buttonCallback;
  
  // Internal methods
  void checkEncoder();
  void checkButtons();
};
