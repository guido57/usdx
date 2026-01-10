#include "encoder.h"

Encoder::Encoder() 
  : _position(0)
  , _lastEncoderState(0)
  , _encoderAccum(0)
  , _rotateCallback(nullptr)
  , _buttonCallback(nullptr)
{
  for (int i = 0; i < 3; i++) {
    _buttons[i].lastState = HIGH;
    _buttons[i].lastChange = 0;
  }
}

void Encoder::begin(uint8_t rotA, uint8_t rotB, uint8_t rotSW, 
                     uint8_t leftSW, uint8_t rightSW) {
  _pinRotA = rotA;
  _pinRotB = rotB;
  _pinRotSW = rotSW;
  _pinLeftSW = leftSW;
  _pinRightSW = rightSW;
  
  // Initialize all pins with pull-ups
  pinMode(_pinRotA, INPUT_PULLUP);
  pinMode(_pinRotB, INPUT_PULLUP);
  pinMode(_pinRotSW, INPUT_PULLUP);
  pinMode(_pinLeftSW, INPUT_PULLUP);
  pinMode(_pinRightSW, INPUT_PULLUP);
  
  // Read initial encoder state
  _lastEncoderState = (digitalRead(_pinRotB) << 1) | digitalRead(_pinRotA);
}

void Encoder::onRotate(EncoderCallback callback) {
  _rotateCallback = callback;
}

void Encoder::onButton(ButtonCallback callback) {
  _buttonCallback = callback;
}

void Encoder::update() {
  checkEncoder();
  checkButtons();
}

void Encoder::checkEncoder() {
  static const int8_t enc_states[] = {0, -1, 1, 0, 1, 0, 0, -1, -1, 0, 0, 1, 0, 1, -1, 0};
  
  uint8_t currentState = (digitalRead(_pinRotB) << 1) | digitalRead(_pinRotA);
  uint8_t transition = (_lastEncoderState << 2) | currentState;
  
  int8_t delta = enc_states[transition & 0x0F];
  if (delta != 0) {
    _encoderAccum += delta;
    
    // Only report position change every 4 transitions (1 detent)
    if (abs(_encoderAccum) >= 4) {
      int8_t direction = (_encoderAccum > 0) ? 1 : -1;
      _position += direction;
      _encoderAccum = 0;
      
      if (_rotateCallback) {
        _rotateCallback(direction);
      }
    }
  }
  
  _lastEncoderState = currentState;
}

void Encoder::checkButtons() {
  unsigned long now = millis();
  
  uint8_t pins[3] = {_pinRotSW, _pinLeftSW, _pinRightSW};
  
  for (int i = 0; i < 3; i++) {
    bool currentState = digitalRead(pins[i]);
    
    // Use longer debounce for all switch pins
    unsigned long debounce = SWITCH_DEBOUNCE_MS;
    
    // Detect state change with debounce
    if (currentState != _buttons[i].lastState && 
        (now - _buttons[i].lastChange) > debounce) {
      _buttons[i].lastState = currentState;
      _buttons[i].lastChange = now;
      
      // Report to callback (active LOW)
      if (_buttonCallback) {
        _buttonCallback(i, currentState == LOW);
      }
    }
  }
}
