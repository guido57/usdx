#!/usr/bin/env python3
"""Analyze an I/Q CSV dump produced by the firmware.

Usage:
  python3 tools/iq_analyze.py capture.csv

Input format:
  Lines like: i,q
  Header lines starting with '#' are ignored.

Requires:
  numpy (mandatory)

Optional:
  matplotlib (for plotting)
"""

from __future__ import annotations

import sys
from pathlib import Path


def _require_numpy():
    try:
        import numpy as np  # type: ignore

        return np
    except Exception as e:
        print("ERROR: numpy is required (pip install numpy)")
        raise


def read_iq_csv(path: Path):
    np = _require_numpy()

    i_list = []
    q_list = []
    fs_hz = None

    bad_lines = 0

    for line in path.read_text().splitlines():
        line = line.strip()
        if not line:
            continue

        # PlatformIO device monitor often prefixes each line with a timestamp, e.g.
        #   21:09:20.083 > 1506,1504
        # Strip everything up to and including the first '>' if present.
        if ">" in line:
            line = line.split(">", 1)[1].strip()
            if not line:
                continue

        if line.startswith("#"):
            # Try to parse fs_hz from header.
            if "fs_hz=" in line:
                try:
                    part = line.split("fs_hz=")[1]
                    fs_hz = int(part.split()[0].strip())
                except Exception:
                    pass
            continue

        if "," not in line:
            bad_lines += 1
            continue

        try:
            a, b = line.split(",", 1)
            i_list.append(int(a.strip()))
            q_list.append(int(b.strip()))
        except Exception:
            bad_lines += 1
            continue

    if bad_lines:
        print(f"Note: ignored {bad_lines} malformed line(s)")

    i = np.array(i_list, dtype=np.float64)
    q = np.array(q_list, dtype=np.float64)
    return i, q, fs_hz


def hann(n: int):
    np = _require_numpy()
    return 0.5 - 0.5 * np.cos(2.0 * np.pi * np.arange(n) / n)


def analyze_one(x, fs_hz: float | None, label: str):
    np = _require_numpy()

    n = len(x)
    if n < 256:
        raise SystemExit("Need at least ~256 samples")

    x = x - np.mean(x)

    w = hann(n)
    xw = x * w

    # rFFT
    X = np.fft.rfft(xw)
    mag = np.abs(X)

    # Ignore DC bin (0)
    k1 = int(np.argmax(mag[1:]) + 1)

    # Basic single-bin frequency estimate
    f1 = None
    if fs_hz is not None:
        f1 = fs_hz * k1 / n

    # THD estimate from harmonics 2..5 (bin-picking)
    fund = mag[k1]
    harm_pow = 0.0
    for h in range(2, 6):
        kh = k1 * h
        if kh >= len(mag):
            break
        harm_pow += mag[kh] ** 2

    thd = (harm_pow ** 0.5) / fund if fund > 0 else float("nan")

    rms = (np.mean(x**2) ** 0.5)
    peak = np.max(np.abs(x))

    return {
        "label": label,
        "n": n,
        "mean": float(np.mean(x)),
        "rms": float(rms),
        "peak": float(peak),
        "k1": int(k1),
        "f1_hz": float(f1) if f1 is not None else None,
        "thd": float(thd),
    }


def main(argv: list[str]) -> int:
    if len(argv) != 2:
        print("Usage: python3 tools/iq_analyze.py capture.csv")
        return 2

    path = Path(argv[1])
    i, q, fs_hz = read_iq_csv(path)

    print(f"Samples: {len(i)} pairs")
    if fs_hz is not None:
        print(f"Fs: {fs_hz} Hz (per channel)")
    else:
        print("Fs: unknown (header missing)")

    res_i = analyze_one(i, fs_hz, "I")
    res_q = analyze_one(q, fs_hz, "Q")

    for r in (res_i, res_q):
        ftxt = f"{r['f1_hz']:.2f} Hz" if r["f1_hz"] is not None else "(unknown)"
        print(
            f"{r['label']}: rms={r['rms']:.2f} peak={r['peak']:.2f} "
            f"fund={ftxt} THD(2..5)={100.0*r['thd']:.2f}%"
        )

    # Optional plot
    try:
        import matplotlib.pyplot as plt  # type: ignore

        nshow = min(400, len(i))
        t = None
        if fs_hz is not None:
            import numpy as np  # type: ignore

            t = np.arange(nshow) / fs_hz
            plt.plot(t, i[:nshow], label="I")
            plt.plot(t, q[:nshow], label="Q")
            plt.xlabel("Time (s)")
        else:
            plt.plot(i[:nshow], label="I")
            plt.plot(q[:nshow], label="Q")
            plt.xlabel("Sample")

        plt.ylabel("Counts")
        plt.title("I/Q capture (first samples)")
        plt.legend()
        plt.grid(True)
        plt.show()
    except Exception:
        pass

    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
