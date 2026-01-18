#pragma once

#define VERSION   "0.01"

// rotary encoder
#define GPIO_ROT_A     47  // GPIO pin for rotary encoder A
#define GPIO_ROT_B     21 // GPIO pin for rotary encoder B   
#define GPIO_ROT_SW    45  // GPIO pin for rotary encoder switch  
#define GPIO_L_SW      13  // GPIO pin for left pushbutton switch   
#define GPIO_R_SW      14  // GPIO pin for right pushbutton switch

// I2C pins (shared bus for all devices)
#define I2C_SDA 41
#define I2C_SCL 42

// SI5351 configuration (used by src/phy/si5351.h)
#ifndef F_XTAL
#define F_XTAL 25000000UL
#endif

#ifndef SI5351_ADDR
#define SI5351_ADDR 0x60
#endif

// Set to 1 to print SI5351 programming details over Serial.
#ifndef SYNTH_DEBUG
#define SYNTH_DEBUG 0
#endif


// Default test wiring for ESP32-S3 DevKitC: use GPIO1/GPIO2 (ADC-capable and typically exposed).
// Some modules/board variants reserve GPIO9/10 for flash signals; IDF may reject them.
#define GPIO_ADC_I 6
#define GPIO_ADC_Q 7

// IQ ADC sampling configuration (ESP32)
// main.ori uses a 62.5kHz timer ISR and alternates I/Q each tick, i.e. ~31.25kS/s per channel.

// This project uses the ESP-IDF ADC continuous driver for I/Q sampling.
// If you ever need a non-IDF fallback again, reintroduce a feature flag and
// implement a separate ADC backend.
#if !defined(ARDUINO_ARCH_ESP32)
	#error "IQ ADC requires ESP32 + ESP-IDF ADC continuous driver"
#endif

#ifndef IQ_ADC_CONV_RATE_HZ
	#define IQ_ADC_CONV_RATE_HZ 64000U
#endif

// DC removal ("DC decoupling") for raw I/Q samples.
// Implemented as a slow running average (IIR) in fixed-point:
//   dcQ8 += ((x<<8) - dcQ8) >> IQ_ADC_DC_SHIFT;
//   y = x - (dcQ8>>8)
// With IQ_ADC_DC_SHIFT=8 and Fs≈31250S/s per channel, the high-pass cutoff is ~20Hz.
#ifndef IQ_ADC_DC_SHIFT
	#define IQ_ADC_DC_SHIFT 8
#endif

// How many I/Q pairs to buffer in RAM (consumer can pop later).
// 8192 pairs ~= 8192 / 31250 = 0.262s of headroom.
#ifndef IQ_ADC_RING_SIZE
	#define IQ_ADC_RING_SIZE 8192U
#endif

// ADC DMA buffering (continuous mode).
// Data rate is IQ_ADC_CONV_RATE_HZ * 4 bytes/result.
#ifndef IQ_ADC_DMA_MAX_STORE_BYTES
	#define IQ_ADC_DMA_MAX_STORE_BYTES 65536U
#endif

#ifndef IQ_ADC_DMA_FRAME_BYTES
	#define IQ_ADC_DMA_FRAME_BYTES 2048U
#endif

// Background ADC reader task tuning.
#ifndef IQ_ADC_TASK_PRIORITY
	#define IQ_ADC_TASK_PRIORITY 20
#endif

#ifndef IQ_ADC_TASK_STACK
	#define IQ_ADC_TASK_STACK 4096
#endif

#ifndef IQ_ADC_TASK_CORE
	// -1 = let scheduler choose; 0/1 pins the task to a core.
	#define IQ_ADC_TASK_CORE -1
#endif

// Set to 1 to print ADC noise stats every 5 seconds.
#ifndef IQ_ADC_NOISE_LOG
  #define IQ_ADC_NOISE_LOG 0
#endif

// Set to 1 to enable image rejection / IQ debug logs in main loop.
#ifndef IQ_MEASURE_LOG
	#define IQ_MEASURE_LOG 1
#endif

// I2S Audio Output (MAX98357 amplifier)
// GPIO pins for I2S (MAX98357)
#define GPIO_I2S_LRC   15  // Left/Right clock (word select)
#define GPIO_I2S_BCLK  16  // Bit clock
#define GPIO_I2S_DOUT  17  // Data out

// Audio sample rate (8kHz for 4kHz bandwidth)
#define AUDIO_I2S_SAMPLE_RATE 8000
