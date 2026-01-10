#!/usr/bin/env python3
"""
Test CIC filter implementation to verify it matches main.ori behavior
and doesn't produce artifacts like chirp or harmonic distortion.
"""

import numpy as np
import matplotlib.pyplot as plt

class CicFilter:
    """CIC filter matching cic_filter.cpp implementation"""
    
    def __init__(self):
        self.reset()
    
    def reset(self):
        self.i_s0za1 = 0
        self.i_s0zb0 = 0
        self.i_s0zb1 = 0
        self.i_s1za1 = 0
        self.i_s1zb0 = 0
        self.i_s1zb1 = 0
        self.q_s0za1 = 0
        self.q_s0zb0 = 0
        self.q_s0zb1 = 0
        self.q_s1za1 = 0
        self.q_s1zb0 = 0
        self.q_s1zb1 = 0
        self.i_prev = 0
        self.q_prev = 0
        self.phase = 0
        self.i_output = 0
        self.q_output = 0
        
    def process_sample(self, i_sample, q_sample):
        """Process one I/Q sample, returns (ready, i_out, q_out)"""
        M_SR = 1  # Shift right amount
        output_ready = False
        
        # Input interpolation/smoothing
        i_smooth = (self.i_prev + i_sample) // 2
        q_smooth = (self.q_prev + q_sample) // 2
        self.i_prev = i_sample
        self.q_prev = q_sample
        
        if self.phase == 0:
            # Process I, output result
            i_s1za0 = (i_smooth + (self.i_s0za1 + self.i_s0zb0) * 3 + self.i_s0zb1) >> M_SR
            self.i_s0za1 = i_smooth
            self.i_output = (i_s1za0 + (self.i_s1za1 + self.i_s1zb0) * 3 + self.i_s1zb1)
            self.i_s1za1 = i_s1za0
            output_ready = True
            
        elif self.phase == 1:
            self.q_s0zb1 = self.q_s0zb0
            self.q_s0zb0 = q_smooth
            
        elif self.phase == 2:
            self.i_s0zb1 = self.i_s0zb0
            self.i_s0zb0 = i_smooth
            
        elif self.phase == 3:
            self.q_s1zb1 = self.q_s1zb0
            self.q_s1zb0 = (q_smooth + (self.q_s0za1 + self.q_s0zb0) * 3 + self.q_s0zb1) >> M_SR
            self.q_s0za1 = q_smooth
            
        elif self.phase == 4:
            # Modified to output at phase 4 too (for decimation by 4)
            i_s1za0 = (i_smooth + (self.i_s0za1 + self.i_s0zb0) * 3 + self.i_s0zb1) >> M_SR
            self.i_s0za1 = i_smooth
            self.i_output = (i_s1za0 + (self.i_s1za1 + self.i_s1zb0) * 3 + self.i_s1zb1)
            self.i_s1za1 = i_s1za0
            output_ready = True
            
        elif self.phase == 5:
            self.q_s0zb1 = self.q_s0zb0
            self.q_s0zb0 = q_smooth
            
        elif self.phase == 6:
            self.i_s0zb1 = self.i_s0zb0
            self.i_s0zb0 = i_smooth
            
        elif self.phase == 7:
            q_s1za0 = (q_smooth + (self.q_s0za1 + self.q_s0zb0) * 3 + self.q_s0zb1) >> M_SR
            self.q_s0za1 = q_smooth
            self.q_output = (q_s1za0 + (self.q_s1za1 + self.q_s1zb0) * 3 + self.q_s1zb1)
            self.q_s1za1 = q_s1za0
        
        self.phase = (self.phase + 1) & 0x07
        
        return output_ready, self.i_output, self.q_output


def simple_decimator(i_samples, q_samples, decim_factor=4):
    """Simple averaging decimator"""
    n = len(i_samples)
    i_out = []
    q_out = []
    
    for start in range(0, n - decim_factor + 1, decim_factor):
        i_avg = np.mean(i_samples[start:start+decim_factor])
        q_avg = np.mean(q_samples[start:start+decim_factor])
        i_out.append(i_avg)
        q_out.append(q_avg)
    
    return np.array(i_out), np.array(q_out)


def test_tone(freq_hz=500, fs_in=32000, duration=0.5, add_dc=0):
    """Test with a pure tone"""
    
    # Generate input signal
    t = np.arange(0, duration, 1/fs_in)
    i_in = np.sin(2 * np.pi * freq_hz * t) * 1000 + add_dc  # Scale to ~1000 amplitude
    q_in = np.cos(2 * np.pi * freq_hz * t) * 1000 + add_dc
    
    # Convert to int16
    i_in = i_in.astype(np.int16)
    q_in = q_in.astype(np.int16)
    
    # Test CIC filter
    cic = CicFilter()
    i_cic_out = []
    q_cic_out = []
    
    for i_samp, q_samp in zip(i_in, q_in):
        ready, i_out, q_out = cic.process_sample(int(i_samp), int(q_samp))
        if ready:
            i_cic_out.append(i_out)
            q_cic_out.append(q_out)
    
    i_cic_out = np.array(i_cic_out)
    q_cic_out = np.array(q_cic_out)
    
    # Apply same scaling as in main.cpp
    i_cic_scaled = i_cic_out // 256
    q_cic_scaled = q_cic_out // 256
    
    # Test simple decimator
    i_simple, q_simple = simple_decimator(i_in, q_in, 4)
    
    # Output sample rate
    fs_out = fs_in / 4
    t_out = np.arange(len(i_simple)) / fs_out
    
    return {
        'time': t_out,
        'i_cic': i_cic_scaled,
        'q_cic': q_cic_scaled,
        'i_simple': i_simple,
        'q_simple': q_simple,
        'fs_out': fs_out
    }


def plot_results(result):
    """Plot comparison of CIC vs simple decimator"""
    
    fig, axes = plt.subplots(3, 2, figsize=(14, 10))
    
    # Time domain - I channel
    axes[0, 0].plot(result['time'][:500], result['i_simple'][:500], 'b-', label='Simple', alpha=0.7)
    axes[0, 0].plot(result['time'][:500], result['i_cic'][:500], 'r--', label='CIC', alpha=0.7)
    axes[0, 0].set_xlabel('Time (s)')
    axes[0, 0].set_ylabel('Amplitude')
    axes[0, 0].set_title('I Channel - Time Domain (first 500 samples)')
    axes[0, 0].legend()
    axes[0, 0].grid(True)
    
    # Time domain - Q channel
    axes[0, 1].plot(result['time'][:500], result['q_simple'][:500], 'b-', label='Simple', alpha=0.7)
    axes[0, 1].plot(result['time'][:500], result['q_cic'][:500], 'r--', label='CIC', alpha=0.7)
    axes[0, 1].set_xlabel('Time (s)')
    axes[0, 1].set_ylabel('Amplitude')
    axes[0, 1].set_title('Q Channel - Time Domain (first 500 samples)')
    axes[0, 1].legend()
    axes[0, 1].grid(True)
    
    # FFT - I channel
    fft_simple = np.fft.fft(result['i_simple'])
    fft_cic = np.fft.fft(result['i_cic'])
    freqs = np.fft.fftfreq(len(result['i_simple']), 1/result['fs_out'])
    
    axes[1, 0].plot(freqs[:len(freqs)//2], 20*np.log10(np.abs(fft_simple[:len(freqs)//2])+1e-10), 'b-', label='Simple', alpha=0.7)
    axes[1, 0].plot(freqs[:len(freqs)//2], 20*np.log10(np.abs(fft_cic[:len(freqs)//2])+1e-10), 'r--', label='CIC', alpha=0.7)
    axes[1, 0].set_xlabel('Frequency (Hz)')
    axes[1, 0].set_ylabel('Magnitude (dB)')
    axes[1, 0].set_title('I Channel - Frequency Spectrum')
    axes[1, 0].legend()
    axes[1, 0].grid(True)
    axes[1, 0].set_xlim([0, result['fs_out']/2])
    
    # FFT - Q channel
    fft_simple_q = np.fft.fft(result['q_simple'])
    fft_cic_q = np.fft.fft(result['q_cic'])
    
    axes[1, 1].plot(freqs[:len(freqs)//2], 20*np.log10(np.abs(fft_simple_q[:len(freqs)//2])+1e-10), 'b-', label='Simple', alpha=0.7)
    axes[1, 1].plot(freqs[:len(freqs)//2], 20*np.log10(np.abs(fft_cic_q[:len(freqs)//2])+1e-10), 'r--', label='CIC', alpha=0.7)
    axes[1, 1].set_xlabel('Frequency (Hz)')
    axes[1, 1].set_ylabel('Magnitude (dB)')
    axes[1, 1].set_title('Q Channel - Frequency Spectrum')
    axes[1, 1].legend()
    axes[1, 1].grid(True)
    axes[1, 1].set_xlim([0, result['fs_out']/2])
    
    # Difference plots
    diff_i = result['i_cic'] - result['i_simple']
    diff_q = result['q_cic'] - result['q_simple']
    
    axes[2, 0].plot(result['time'][:500], diff_i[:500], 'g-')
    axes[2, 0].set_xlabel('Time (s)')
    axes[2, 0].set_ylabel('Difference')
    axes[2, 0].set_title('I Channel - Difference (CIC - Simple)')
    axes[2, 0].grid(True)
    
    axes[2, 1].plot(result['time'][:500], diff_q[:500], 'g-')
    axes[2, 1].set_xlabel('Time (s)')
    axes[2, 1].set_ylabel('Difference')
    axes[2, 1].set_title('Q Channel - Difference (CIC - Simple)')
    axes[2, 1].grid(True)
    
    plt.tight_layout()
    plt.savefig('/home/guido/Documents/PlatformIO/Projects/usdx/tools/cic_test_results.png', dpi=150)
    print(f"Plot saved to: tools/cic_test_results.png")
    plt.show()


def analyze_results(result):
    """Print analysis of the results"""
    
    print("\n=== CIC Filter Test Results ===\n")
    
    # Output counts
    print(f"Output samples: {len(result['i_cic'])} (CIC), {len(result['i_simple'])} (Simple)")
    print(f"Output sample rate: {result['fs_out']} Hz")
    
    # Amplitude comparison
    i_cic_rms = np.sqrt(np.mean(result['i_cic']**2))
    i_simple_rms = np.sqrt(np.mean(result['i_simple']**2))
    q_cic_rms = np.sqrt(np.mean(result['q_cic']**2))
    q_simple_rms = np.sqrt(np.mean(result['q_simple']**2))
    
    print(f"\nRMS Amplitude:")
    print(f"  I channel - CIC: {i_cic_rms:.2f}, Simple: {i_simple_rms:.2f}")
    print(f"  Q channel - CIC: {q_cic_rms:.2f}, Simple: {q_simple_rms:.2f}")
    print(f"  Ratio (CIC/Simple): I={i_cic_rms/i_simple_rms:.3f}, Q={q_cic_rms/q_simple_rms:.3f}")
    
    # Peak values
    print(f"\nPeak values:")
    print(f"  I channel - CIC: {np.max(np.abs(result['i_cic']))}, Simple: {np.max(np.abs(result['i_simple'])):.0f}")
    print(f"  Q channel - CIC: {np.max(np.abs(result['q_cic']))}, Simple: {np.max(np.abs(result['q_simple'])):.0f}")
    
    # DC offset
    i_cic_dc = np.mean(result['i_cic'])
    i_simple_dc = np.mean(result['i_simple'])
    q_cic_dc = np.mean(result['q_cic'])
    q_simple_dc = np.mean(result['q_simple'])
    
    print(f"\nDC offset:")
    print(f"  I channel - CIC: {i_cic_dc:.2f}, Simple: {i_simple_dc:.2f}")
    print(f"  Q channel - CIC: {q_cic_dc:.2f}, Simple: {q_simple_dc:.2f}")
    
    # Harmonic distortion check (look for peaks at multiples of fundamental)
    fft_cic = np.fft.fft(result['i_cic'])
    freqs = np.fft.fftfreq(len(result['i_cic']), 1/result['fs_out'])
    
    # Find fundamental peak
    pos_freqs = freqs[:len(freqs)//2]
    pos_fft = np.abs(fft_cic[:len(freqs)//2])
    fund_idx = np.argmax(pos_fft[10:]) + 10  # Skip DC
    fund_freq = pos_freqs[fund_idx]
    fund_mag = pos_fft[fund_idx]
    
    print(f"\nFundamental frequency: {fund_freq:.1f} Hz, magnitude: {fund_mag:.0f}")
    
    # Check for harmonics
    for harmonic in [2, 3, 4]:
        harmonic_freq = fund_freq * harmonic
        if harmonic_freq < result['fs_out']/2:
            harmonic_idx = np.argmin(np.abs(pos_freqs - harmonic_freq))
            harmonic_mag = pos_fft[harmonic_idx]
            harmonic_db = 20 * np.log10(harmonic_mag / fund_mag) if harmonic_mag > 0 else -np.inf
            print(f"  {harmonic}nd harmonic: {harmonic_db:.1f} dB relative to fundamental")


if __name__ == "__main__":
    print("Testing CIC filter implementation...")
    print("Generating 500 Hz tone at 32 kHz sample rate")
    
    # Test with clean tone (no DC offset)
    result = test_tone(freq_hz=500, fs_in=32000, duration=0.5, add_dc=0)
    
    analyze_results(result)
    plot_results(result)
    
    print("\nTest complete!")
