# IQ Mux and Demux in uSDX: Technical Explanation

## Overview

The IQ multiplexer (mux) and demultiplexer (demux) are essential components in the uSDX's Software Defined Radio (SDR) receiver architecture, enabling the microcontroller to process both in-phase (I) and quadrature (Q) signals using a single ADC (Analog-to-Digital Converter).

## Background: Why I/Q Processing?

In SDR receivers, I/Q (In-phase/Quadrature) signal processing is fundamental for:

1. **Sideband Rejection**: Distinguishing between USB (Upper Sideband) and LSB (Lower Sideband) signals
2. **Complex Signal Representation**: Capturing both amplitude and phase information
3. **Digital Signal Processing**: Enabling sophisticated filtering and demodulation in software

Traditional analog receivers use separate hardware for each sideband. The uSDX uses digital I/Q processing to achieve sideband rejection in software, making the design simpler and more versatile.

## The Challenge: Two Signals, One ADC

The ATMEGA328P microcontroller in the uSDX has multiple ADC inputs but can only sample **one input at a time**. However, the Quadrature Sampling Detector (QSD) provides **two separate analog outputs**:

- **I (In-phase)**: Connected to ADC channel 0 (AUDIO1/PC0)
- **Q (Quadrature)**: Connected to ADC channel 1 (AUDIO2/PC1)

These two signals must be sampled at the same rate and processed together to enable proper I/Q demodulation.

## The Solution: ADC Multiplexing (Mux/Demux)

### What is the IQ Mux?

The **IQ Mux** (multiplexer) is implemented in software using the ATMEGA328P's `ADMUX` register, which controls which ADC input is being sampled. The mux rapidly alternates between:

1. Sampling the **I channel** (ADC0/AUDIO1)
2. Sampling the **Q channel** (ADC1/AUDIO2)

This happens at twice the desired sample rate. For example, if the target audio sample rate is 15.625 kHz, the ADC samples at 31.25 kHz, alternating between I and Q.

### What is the IQ Demux?

The **IQ Demux** (demultiplexer) is the receiving end of this process. After the ADC samples are captured by interrupt service routines, the demux separates them back into two streams:

- Samples from ADC0 → I signal stream
- Samples from ADC1 → Q signal stream

These separate streams are then processed by the DSP algorithms.

## Implementation in uSDX Code

### Mux Configuration (Start of Reception)

In the `start_rx()` function (around line 3748), the code configures the ADC channels:

```c
adc_start(0, !(att == 1), F_ADC_CONV); admux[0] = ADMUX;  // Configure I channel
adc_start(1, !(att == 1), F_ADC_CONV); admux[1] = ADMUX;  // Configure Q channel
```

The `admux[]` array stores the ADMUX register configurations for each channel, allowing rapid switching between I and Q inputs.

### Mux Switching (During Reception)

In the interrupt service routines `sdr_rx()` and `sdr_rx_q()` (around lines 3227 and 3167), the code alternates between channels:

```c
// In sdr_rx() - samples I channel
ADMUX = admux[1];  // set MUX for next conversion (Q channel)
ADCSRA |= (1 << ADSC);  // start next ADC conversion
int16_t adc = ADC - 511;  // read current I sample

// In sdr_rx_q() - samples Q channel  
ADMUX = admux[0];  // set MUX for next conversion (I channel)
ADCSRA |= (1 << ADSC);  // start next ADC conversion
int16_t adc = ADC - 511;  // read current Q sample
```

### Demux and Processing

Each function processes its respective sample:
- `sdr_rx()` processes **I samples** through the I-channel filter chain
- `sdr_rx_q()` processes **Q samples** through the Q-channel filter chain

After decimation (downsampling), both processed I and Q signals are combined in the `process()` function using a Hilbert transform to achieve sideband rejection.

## Timing Diagram

```
ADC Samples:  I₀  Q₀  I₁  Q₁  I₂  Q₂  I₃  Q₃  ...
              ↓   ↓   ↓   ↓   ↓   ↓   ↓   ↓
ADMUX:       [0] [1] [0] [1] [0] [1] [0] [1] ...
              ↓   ↓   ↓   ↓   ↓   ↓   ↓   ↓
Functions:   sdr  sdr sdr sdr sdr sdr sdr sdr ...
             _rx  _rx _rx _rx _rx _rx _rx _rx
                  _q      _q      _q      _q
              ↓   ↓   ↓   ↓   ↓   ↓   ↓   ↓
Demux:       I₀  Q₀  I₁  Q₁  I₂  Q₂  I₃  Q₃
             → I stream  → Q stream
```

## Benefits of This Approach

1. **Hardware Simplicity**: Only one ADC required instead of two
2. **Cost Reduction**: No need for expensive dual ADC chips
3. **Synchronization**: I and Q samples are naturally synchronized since they're captured by the same ADC
4. **Flexibility**: Easy to swap I/Q channels in software if needed (see `SWAP_RX_IQ` option)

## Trade-offs

1. **Sample Rate**: Effective sample rate per channel is half the ADC sample rate
2. **Processing Overhead**: CPU must handle rapid ADC interrupts and channel switching
3. **Timing Critical**: Requires precise timing to maintain I/Q phase relationship

## Sample Rate Correction for I Channel

An important detail: Because the ADC alternates between I and Q, there's a time delay between consecutive I samples (and similarly for Q samples). The code compensates for this in the I channel using **linear interpolation**:

```c
static int16_t prev_adc;
int16_t corr_adc = (prev_adc + adc) / 2;  // Linear interpolation
prev_adc = adc;
```

This correction ensures that I and Q samples represent the same point in time, which is essential for proper I/Q processing and sideband rejection.

## Optional: Swapping I and Q

The code includes a `SWAP_RX_IQ` option (line 3750) that allows reversing which ADC channel is used for I vs Q. This is useful if the hardware is wired differently or to flip the sideband:

```c
#ifdef SWAP_RX_IQ
    adc_start(1, !(att == 1), F_ADC_CONV); admux[0] = ADMUX;  // Q→I
    adc_start(0, !(att == 1), F_ADC_CONV); admux[1] = ADMUX;  // I→Q
#else
    adc_start(0, !(att == 1), F_ADC_CONV); admux[0] = ADMUX;  // I→I
    adc_start(1, !(att == 1), F_ADC_CONV); admux[1] = ADMUX;  // Q→Q
#endif
```

## Conclusion

The IQ mux/demux system in uSDX is an elegant solution that enables full SDR I/Q reception using a single ADC on a simple 8-bit microcontroller. By rapidly multiplexing between the I and Q inputs and demultiplexing the samples in software, the uSDX achieves sophisticated radio reception that would traditionally require much more complex and expensive hardware.

This design philosophy—moving complexity from hardware to software—is central to the uSDX's mission of creating a simple, low-cost, yet capable QRP SSB transceiver.

## References

- uSDX Main Repository: https://github.com/threeme3/usdx
- README.md Section "Technical Description" (lines 137-150)
- Source Code: `usdx.ino` - ADC and DSP functions (lines 3050-3400)
