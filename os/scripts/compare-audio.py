#!/usr/bin/env python3
"""Compares what a device played with an offline render of the same thing.

Finds where the recording's sound starts, lines it up with the reference, then matches
each half-second of the reference against the recording, allowing up to 50 ms either way.
With correct timing, every piece matches at the same offset with a near-perfect
correlation. A changing offset means the playback drifted or skipped (a dropout, or an
audio buffer played late); a low correlation means the audio itself differs.

    compare-audio.py RECORDING.wav REFERENCE.wav

Reads 16-bit PCM and 32-bit float WAV files; compares the left channels.
"""
import struct
import sys

import numpy as np


def read_wav(path):
    data = open(path, "rb").read()
    if data[:4] != b"RIFF" or data[8:12] != b"WAVE":
        sys.exit(f"{path}: not a WAV file")
    pos, fmt, samples = 12, None, None
    while pos + 8 <= len(data):
        tag, size = data[pos:pos + 4], struct.unpack("<I", data[pos + 4:pos + 8])[0]
        body = data[pos + 8:pos + 8 + size]
        if tag == b"fmt ":
            fmt = struct.unpack("<HHIIHH", body[:16])
        elif tag == b"data":
            samples = body
        pos += 8 + size + (size & 1)
    format_tag, channels, rate, _, _, bits = fmt
    if format_tag == 1 and bits == 16:
        x = np.frombuffer(samples[:len(samples) // 2 * 2], "<i2").astype(np.float64) / 32768.0
    elif format_tag == 3 and bits == 32:
        x = np.frombuffer(samples[:len(samples) // 4 * 4], "<f4").astype(np.float64)
    else:
        sys.exit(f"{path}: unsupported format {format_tag}/{bits}-bit")
    x = x[:len(x) // channels * channels].reshape(-1, channels)
    return rate, x[:, 0]


def first_sound(x, threshold=1e-3):
    loud = np.nonzero(np.abs(x) > threshold)[0]
    return int(loud[0]) if len(loud) else None


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    rate, rec = read_wav(sys.argv[1])
    ref_rate, ref = read_wav(sys.argv[2])
    if rate != ref_rate:
        sys.exit(f"sample rates differ: {rate} and {ref_rate}")

    r0, f0 = first_sound(rec), first_sound(ref)
    if r0 is None:
        sys.exit("the recording is silent")
    offset = r0 - f0  # recording index = reference index + offset
    piece, search = rate // 2, int(rate * 0.05)
    print(f"{rate} Hz; sound starts {r0 / rate:.3f} s into the recording; "
          f"{(len(rec) - r0) / rate:.1f} s of playing compared in {piece / rate:.1f} s pieces")

    lags, worst = [], (1.0, 0)
    for start in range(f0, len(ref) - piece, piece):
        a = start + offset - search
        if a < 0 or a + piece + 2 * search > len(rec):
            break
        want = ref[start:start + piece]
        if np.sqrt(np.mean(want ** 2)) < 1e-3:
            continue  # a quiet piece says nothing about timing
        window = rec[a:a + piece + 2 * search]
        corr = np.correlate(window, want, mode="valid")
        energy = np.sqrt(np.convolve(window ** 2, np.ones(piece), mode="valid") * np.sum(want ** 2))
        score = corr / np.maximum(energy, 1e-12)
        best = int(np.argmax(score))
        lags.append(best - search)
        if score[best] < worst[0]:
            worst = (float(score[best]), start)

    if not lags:
        sys.exit("nothing to compare")
    lags = np.array(lags)
    changes = int(np.count_nonzero(np.diff(lags)))
    print(f"{len(lags)} pieces; offset from the first sound: min {lags.min()}, max {lags.max()} samples "
          f"({(lags.max() - lags.min()) / rate * 1000:.2f} ms spread), changes {changes} times")
    print(f"lowest match {worst[0]:.4f} (1.0 is identical), at {(worst[1] - f0) / rate:.1f} s")
    print("offset per piece:", " ".join(str(int(v)) for v in lags))
    ok = lags.max() - lags.min() <= 1 and worst[0] > 0.99
    print("TIMING OK" if ok else "TIMING DIFFERS")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
