#ifndef IQ_ADC_H
#define IQ_ADC_H

#include <Arduino.h>
#include <stdint.h>
#include <stddef.h>

// --- Configuration Constants ---
// Note: These can also be defined in a global "configuration.h"
#ifndef IQ_ADC_RING_SIZE
#define IQ_ADC_RING_SIZE 2048      // Number of I/Q pairs the buffer can hold
#endif

#ifndef IQ_ADC_DMA_FRAME_BYTES
#define IQ_ADC_DMA_FRAME_BYTES 1024 // Size of each DMA data chunk
#endif

#ifndef IQ_ADC_DMA_MAX_STORE_BYTES
#define IQ_ADC_DMA_MAX_STORE_BYTES 4096 // Total DMA internal storage
#endif

#ifndef IQ_ADC_CONV_RATE_HZ
#define IQ_ADC_CONV_RATE_HZ 64000   // Aggregate sample rate (e.g., 32kHz per channel)
#endif

#ifndef IQ_ADC_DC_SHIFT
#define IQ_ADC_DC_SHIFT 8           // Filter coefficient for DC decoupling
#endif

// --- Data Structures ---

/**
 * @brief Structure to hold a synchronous In-phase (I) and Quadrature (Q) sample.
 */
struct IqSample {
    int16_t i;
    int16_t q;
};

// --- Public API ---

/**
 * @brief Initializes the ADC hardware, DMA controller, and starts the reader task.
 * Maps GPIO_ADC_I and GPIO_ADC_Q to their respective hardware channels.
 */
void iq_adc_setup();


/**
 * @brief Restarts the ADC hardware, DMA controller, and reader task.
 * This function can be used to apply new settings without fully reinitializing the system  .
 */
void iq_adc_restart(); // Added function declaration for restarting the ADC with new settings

/**
 * @brief Checks if the ADC system is initialized and running.
 * @return true if ready, false otherwise.
 */
bool iq_adc_ready();


esp_err_t iq_adc_error(); 

/**
 * @brief Reads one I/Q pair from the ring buffer.
 * This function blocks for up to 50ms if the buffer is empty.
 * @return IqSample containing the centered/decoupled I and Q values.
 */
IqSample iq_adc_read_iq();

/**
 * @brief Updates the ADC attenuation level (gain).
 * Levels: 0 (0dB), 1 (6dB), 2+ (12dB).
 * This will briefly stop and restart the ADC conversion.
 */
void iq_adc_set_att_level(uint8_t level);

/**
 * @brief Returns the number of I/Q pairs currently waiting in the ring buffer.
 */
size_t iq_adc_available();

/**
 * @brief Returns the total number of sample pairs dropped due to buffer overflow.
 */
uint32_t iq_adc_dropped_pairs();

/**
 * @brief Prints diagnostic information to the Serial console.
 * @param nSamples Number of samples to include in the diagnostic report.
 */
void iq_adc_print_stats(size_t nSamples);

#endif // IQ_ADC_H