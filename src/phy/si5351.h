#pragma once
#include <Arduino.h>
#include <Wire.h>

#include <stdbool.h>
#include <stdint.h>

// UI-agnostic RX synth programming helpers.
// Mode mapping is intentionally numeric to avoid coupling this low-level driver
// header to the UI layer.
enum : uint8_t {
  SI5351_RX_MODE_LSB = 0,
  SI5351_RX_MODE_USB = 1,
  SI5351_RX_MODE_CW = 2,
  SI5351_RX_MODE_FM = 3,
  SI5351_RX_MODE_AM = 4,
};

class SI5351;

struct Si5351RxSynthState {
  uint32_t fxtalHz;
  int32_t vfoHz;
  uint8_t mode;
  int16_t rit;
  bool ritActive;
  int16_t cwOffset;
};

static inline bool synthStateChanged(const Si5351RxSynthState& a, const Si5351RxSynthState& b) {
  if (a.fxtalHz != b.fxtalHz) return true;
  if (a.vfoHz != b.vfoHz) return true;
  if (a.mode != b.mode) return true;
  if (a.ritActive != b.ritActive) return true;
  if (a.rit != b.rit) return true;
  if (a.cwOffset != b.cwOffset) return true;
  return false;
}

static inline int32_t programSi5351Rx(SI5351& si5351, const Si5351RxSynthState& s);

class SI5351 {
public:
  volatile int32_t _fout;
  volatile uint8_t _div;  // note: uint8_t asserts fout > 3.5MHz with R_DIV=1
  volatile uint16_t _msa128min512;
  volatile uint32_t _msb128;
  //volatile uint32_t _mod;
  volatile uint8_t pll_regs[8];
  volatile uint8_t _i2c_error = 0;

  inline uint8_t i2cError() const { return _i2c_error; }
  inline void clearI2cError() { _i2c_error = 0; }

  #define BB0(x) ((uint8_t)(x))           // Bash byte x of int32_t
  #define BB1(x) ((uint8_t)((x)>>8))
  #define BB2(x) ((uint8_t)((x)>>16))

  #define FAST __attribute__((optimize("Ofast")))

  volatile uint32_t fxtal = F_XTAL;

  inline void FAST freq_calc_fast(int16_t df)  // note: relies on cached variables: _msb128, _msa128min512, _div, _fout, fxtal
  {
    #define _MSC  0x10000
    uint32_t msb128 = _msb128 + ((int64_t)(_div * (int32_t)df) * _MSC * 128) / fxtal;

    uint16_t msp1 = _msa128min512 + msb128 / _MSC; // = 128 * _msa + msb128 / _MSC - 512;
    uint16_t msp2 = msb128; // = msb128 % _MSC;  assuming MSC is covering exact uint16_t so the mod operation can dissapear (and the upper BB2 byte) // = msb128 - msb128/_MSC * _MSC;

    pll_regs[4] = BB0(msp1);
    pll_regs[5] = ((_MSC&0xF0000)>>(16-4))/*|BB2(msp2)*/; // top nibble MUST be same as top nibble of _MSC !  assuming that BB2(msp2) is always 0 -> so reg is constant
    pll_regs[6] = BB1(msp2);
    pll_regs[7] = BB0(msp2);
  }

  inline void SendPLLRegisterBulk(){
    if (_i2c_error) return;
    Wire.beginTransmission(SI5351_ADDR);
    Wire.write(26+0*8 + 4);  // Write to PLLA
    //Wire.write(26+1*8 + 4);  // Write to PLLB
    Wire.write(pll_regs[4]);
    Wire.write(pll_regs[5]);
    Wire.write(pll_regs[6]);
    Wire.write(pll_regs[7]);
    _i2c_error = Wire.endTransmission();
    delay(0);
  }
  
  void SendRegister(uint8_t reg, uint8_t* data, uint8_t n){
    if (_i2c_error) return;
    Wire.beginTransmission(SI5351_ADDR);
    Wire.write(reg);
    while (n--) Wire.write(*data++);
    _i2c_error = Wire.endTransmission();
    delay(0);
  }
  void SendRegister(uint8_t reg, uint8_t val){ SendRegister(reg, &val, 1); }
  int16_t iqmsa; // to detect a need for a PLL reset
///*
  enum ms_t { PLLA=0, PLLB=1, MSNA=-2, MSNB=-1, MS0=0, MS1=1, MS2=2, MS3=3, MS4=4, MS5=5 };
  
  void ms(int8_t n, uint32_t div_nom, uint32_t div_denom, uint8_t pll = PLLA, uint8_t _int = 0, uint16_t phase = 0, uint8_t rdiv = 0){
    if (_i2c_error) return;
    uint16_t msa; uint32_t msb, msc, msp1, msp2, msp3;
    msa = div_nom / div_denom;     // integer part: msa must be in range 15..90 for PLL, 8+1/1048575..900 for MS
    if(msa == 4) _int = 1;  // To satisfy the MSx_INT=1 requirement of AN619, section 4.1.3 which basically says that for MS divider a value of 4 and integer mode must be used
    msb = (_int) ? 0 : (((uint64_t)(div_nom % div_denom)*_MSC) / div_denom); // fractional part
    msc = (_int) ? 1 : _MSC;
    //lcd.setCursor(0, 0); lcd.print(n); lcd.print(":"); lcd.print(msa); lcd.print(" "); lcd.print(msb); lcd.print(" "); lcd.print(msc); lcd.print(F("    ")); delay(500);
    msp1 = 128*msa + 128*msb/msc - 512;
    msp2 = 128*msb - 128*msb/msc * msc;
    msp3 = msc;
    uint8_t ms_reg2 = BB2(msp1) | (rdiv<<4) | ((msa == 4)*0x0C);
    uint8_t ms_regs[8] = { BB1(msp3), BB0(msp3), ms_reg2, BB1(msp1), BB0(msp1), BB2(((msp3 & 0x0F0000)<<4) | msp2), BB1(msp2), BB0(msp2) };

    SendRegister(n*8+42, ms_regs, 8); // Write to MSx
    if (_i2c_error) return;
    if(n < 0){
      SendRegister(n+16+8, 0x80|(0x40*_int)); // MSNx PLLn: 0x40=FBA_INT; 0x80=CLKn_PDN
    } else {
      //SendRegister(n+16, ((pll)*0x20)|0x0C|0|(0x40*_int));  // MSx CLKn: 0x0C=PLLA,0x2C=PLLB local msynth; 0=2mA; 0x40=MSx_INT; 0x80=CLKx_PDN
      SendRegister(n+16, ((pll)*0x20)|0x0C|3|(0x40*_int));  // MSx CLKn: 0x0C=PLLA,0x2C=PLLB local msynth; 3=8mA; 0x40=MSx_INT; 0x80=CLKx_PDN
      SendRegister(n+165, (!_int) * phase * msa / 90);      // when using: make sure to configure MS in fractional-mode, perform reset afterwards
    }
  }

  void phase(int8_t n, uint32_t div_nom, uint32_t div_denom, uint16_t phase){ SendRegister(n+165, phase * (div_nom / div_denom) / 90); }  // when using: make sure to configure MS in fractional-mode!, perform reset afterwards

  void reset(){ SendRegister(177, 0xA0); } // 0x20 reset PLLA; 0x80 reset PLLB

  void oe(uint8_t mask){ SendRegister(3, ~mask); } // output-enable mask: CLK2=4; CLK1=2; CLK0=1

  void freq(int32_t fout, uint16_t i, uint16_t q){  // Set a CLK0,1,2 to fout Hz with phase i, q (on PLLA)
      if (_i2c_error) return;
      uint8_t rdiv = 0; // CLK pin sees fout/(2^rdiv)
      if(fout > 300000000){ i/=3; q/=3; fout/=3; }  // for higher freqs, use 3rd harmonic
      if(fout < 500000){ rdiv = 7; fout *= 128; } // Divide by 128 for fout 4..500kHz
      uint16_t d; if(fout < 30000000) d = (16 * fxtal) / fout; else d = (32 * fxtal) / fout;  // Integer part  .. maybe 44?
      if(fout < 3500000) d = (7 * fxtal) / fout;  // PLL at 189MHz to cover 160m (freq>1.48MHz) when using 27MHz crystal
      if(fout > 140000000) d = 4; // for f=140..300MHz; AN619; 4.1.3, this implies integer mode
      if(d % 2) d++;  // even numbers preferred for divider (AN619 p.4 and p.6)
      if( (d * (fout - 5000) / fxtal) != (d * (fout + 5000) / fxtal) ) d += 2; // Test if multiplier remains same for freq deviation +/- 5kHz, if not use different divider to make same
      uint32_t fvcoa = d * fout;  // Variable PLLA VCO frequency at integer multiple of fout at around 27MHz*16 = 432MHz
      // si5351 spectral purity considerations: https://groups.io/g/QRPLabs/message/42662

      ms(MSNA, fvcoa, fxtal);                   // PLLA in fractional mode
      //ms(MSNB, fvcoa, fxtal);
      ms(MS0,  fvcoa, fout, PLLA, 0, i, rdiv);  // Multisynth stage with integer divider but in frac mode due to phase setting
      ms(MS1,  fvcoa, fout, PLLA, 0, q, rdiv);
#ifdef F_CLK2
      freqb(F_CLK2);
#else
      ms(MS2,  fvcoa, fout, PLLA, 0, 0, rdiv);
#endif
      if(iqmsa != (((int8_t)i-(int8_t)q)*((int16_t)(fvcoa/fout))/90)){ iqmsa = ((int8_t)i-(int8_t)q)*((int16_t)(fvcoa/fout))/90; reset(); }
      oe(0b00000011);  // output enable CLK0, CLK1

      _fout = fout;  // cache
      _div = d;
      _msa128min512 = fvcoa / fxtal * 128 - 512;
      _msb128=((uint64_t)(fvcoa % fxtal)*_MSC*128) / fxtal;
      //_mod = fvcoa % fxtal;
  }

  void freqb(uint32_t fout){  // Set a CLK2 to fout Hz (on PLLB)
      if (_i2c_error) return;
      uint16_t d = (16 * fxtal) / fout;
      if(d % 2) d++;  // even numbers preferred for divider (AN619 p.4 and p.6)
      uint32_t fvcoa = d * fout;  // Variable PLLA VCO frequency at integer multiple of fout at around 27MHz*16 = 432MHz

      ms(MSNB, fvcoa, fxtal);
      ms(MS2,  fvcoa, fout, PLLB, 0, 0, 0);
  }
  
  uint8_t RecvRegister(uint8_t reg){
    Wire.beginTransmission(SI5351_ADDR);
    Wire.write(reg);
    Wire.endTransmission();
    Wire.requestFrom(SI5351_ADDR, (uint8_t)1);
    uint8_t data = Wire.read();
    return data;
  }
  void powerDown(){
    if (_i2c_error) return;
    SendRegister(3, 0b11111111); // Disable all CLK outputs
    SendRegister(24, 0b00010000); // Disable state: CLK2 HIGH state, CLK0 & CLK1 LOW state when disabled; CLK2 needs to be in HIGH state to make sure that cap to gate is already charged, preventing "exponential pulse is caused by CLK2, which had been at 0v whilst it was disabled, suddenly generating a 5vpp waveform, which is “added to” the 0v filtered PWM output and causing the output fets to be driven with the full 5v pp.", see: https://forum.dl2man.de/viewtopic.php?t=146&p=1307#p1307
    SendRegister(25, 0b00000000); // Disable state: LOW state when disabled
    for(int addr = 16; addr != 24; addr++) {
      SendRegister(addr, 0b10000000);  // Conserve power when output is disabled
      if (_i2c_error) break;
    }
    SendRegister(187, 0);        // Disable fanout (power-safe)
    // To initialise things as they should:
    SendRegister(149, 0);        // Disable spread spectrum enable
    SendRegister(183, 0b11010010);  // Internal CL = 10 pF (default)
  }
  #define SI_CLK_OE 3

};

static inline int32_t programSi5351Rx(SI5351& si5351, const Si5351RxSynthState& s) {
  // Clone the core logic from main.ori (RX programming):
  // - LSB: si5351.freq(freq, rx_ph_q, 0)
  // - USB: si5351.freq(freq, 0, rx_ph_q)
  // - CW : si5351.freq(freq + cw_offset, rx_ph_q, 0)
  // - RIT applied via freq_calc_fast() + SendPLLRegisterBulk()
  const uint16_t rx_ph_q = 90;

  const int32_t baseFreq = s.vfoHz;
  int32_t freq = baseFreq;

  if (s.mode == SI5351_RX_MODE_CW) {
    freq += s.cwOffset;
    si5351.freq(freq, rx_ph_q, 0);
  } else if (s.mode == SI5351_RX_MODE_LSB) {
    si5351.freq(freq, rx_ph_q, 0);
  } else {
    si5351.freq(freq, 0, rx_ph_q);
  }

  const int16_t rit = s.ritActive ? s.rit : 0;
  if (rit != 0) {
    si5351.freq_calc_fast(rit);
    si5351.SendPLLRegisterBulk();
  }

#if SYNTH_DEBUG
  const char* modeLabel = "?";
  switch (s.mode) {
    case SI5351_RX_MODE_LSB: modeLabel = "LSB"; break;
    case SI5351_RX_MODE_USB: modeLabel = "USB"; break;
    case SI5351_RX_MODE_CW:  modeLabel = "CW"; break;
    case SI5351_RX_MODE_FM:  modeLabel = "FM"; break;
    case SI5351_RX_MODE_AM:  modeLabel = "AM"; break;
    default: break;
  }
  Serial.printf(
    "[SYNTH] mode=%s vfo=%ldHz cwOff=%d rit=%d (%s) -> prog=%ldHz\n",
    modeLabel,
    (long)baseFreq,
    (int)s.cwOffset,
    (int)rit,
    s.ritActive ? "on" : "off",
    (long)freq
  );
#endif

  return freq;
}

