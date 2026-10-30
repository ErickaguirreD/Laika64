#!/usr/bin/env python3
"""LAIKA 64 - the setup assistant's music (assistant/laika_orbit.mp3), synthesised here.

An original ambient piece in D minor, ~2 minutes, made to loop: slow detuned
pads over a deep drone, a soft arpeggio with a ping-pong echo, star-like
shimmers - and, for Laika (Sputnik 2, 1957), the beep-beep of a satellite's
signal drifting in and out with a little radio hiss.

Needs NumPy and lameenc (pip install lameenc).
    python platform/xbox360/tools/gen_wizard_music.py
"""
import os

import numpy as np
import lameenc

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))
OUT_DIR = os.path.join(ROOT, "assets", "assistant")
OUT = os.path.join(OUT_DIR, "laika_orbit.mp3")
SR = 44100
BPM = 70
BAR = 4 * 60.0 / BPM
BARS = 32
N = int(BAR * BARS * SR)
rng = np.random.RandomState(1957)


def hz(midi):
    return 440.0 * 2 ** ((midi - 69) / 12.0)


def env_adsr(n, a, r):
    e = np.ones(n)
    na, nr = int(a * SR), int(r * SR)
    if na: e[:na] = np.linspace(0, 1, na)
    if nr: e[-nr:] *= np.linspace(1, 0, nr)
    return e


def lowpass(x, cutoff):
    # one pole, run on whole arrays by blocks (cheap enough here)
    a = np.exp(-2 * np.pi * cutoff / SR)
    y = np.empty_like(x)
    acc = 0.0
    for i in range(0, len(x), 4096):
        blk = x[i:i + 4096]
        out = np.empty_like(blk)
        for j, v in enumerate(blk):
            acc = (1 - a) * v + a * acc
            out[j] = acc
        y[i:i + 4096] = out
    return y


L = np.zeros(N)
R = np.zeros(N)
t_all = np.arange(N) / SR

# The chords (MIDI notes): Dm9, Bbmaj7, Fmaj7/A, Cadd9 - two bars each.
CHORDS = [[50, 57, 60, 64, 65], [46, 53, 57, 60, 62], [45, 53, 57, 60, 64], [48, 55, 60, 62, 67]]

# Pads: three detuned voices per note, soft attack and release, spread left/right.
for bar in range(BARS):
    chord = CHORDS[(bar // 2) % 4]
    if bar % 2: continue
    start, length = int(bar * BAR * SR), int(2 * BAR * SR * 1.12)
    length = min(length, N - start)
    t = np.arange(length) / SR
    e = env_adsr(length, 2.2, 2.6)
    for k, note in enumerate(chord):
        f = hz(note + 12)
        for dt, pan in ((-0.07, 0.25), (0.0, 0.5), (0.08, 0.75)):
            ff = f * (1 + dt / 100.0 * 12)
            wave = np.sin(2 * np.pi * ff * t) + 0.25 * np.sin(2 * np.pi * ff * 2 * t + 0.4) + 0.08 * np.sin(2 * np.pi * ff * 3 * t)
            v = wave * e * 0.035
            L[start:start + length] += v * (1 - pan)
            R[start:start + length] += v * pan

# The drone: the root an octave down, breathing slowly.
root = np.array([CHORDS[(int(x / BAR) // 2) % 4][0] for x in t_all])
f_drone = 440.0 * 2 ** ((root - 12 - 69) / 12.0)
phase = 2 * np.pi * np.cumsum(f_drone) / SR
drone = (np.sin(phase) + 0.3 * np.sin(phase * 2)) * (0.07 + 0.03 * np.sin(2 * np.pi * t_all / 13.0))
L += drone; R += drone

# The arpeggio: chord tones two octaves up, eighth notes, every other bar from bar 4 on.
step = BAR / 8
dl = np.zeros(N); dr = np.zeros(N)
for bar in range(4, BARS):
    if bar % 8 in (6, 7): continue   # a breath now and then
    chord = CHORDS[(bar // 2) % 4]
    for s in range(8):
        note = chord[[0, 2, 3, 4, 3, 2, 1, 2][s]] + 12
        start = int((bar * BAR + s * step) * SR)
        n = int(0.9 * SR)
        n = min(n, N - start)
        t = np.arange(n) / SR
        pl = np.sin(2 * np.pi * hz(note) * t) * np.exp(-t * 5.0) * 0.05
        pan = 0.35 + 0.3 * (s % 2)
        dl[start:start + n] += pl * (1 - pan)
        dr[start:start + n] += pl * pan
# its echo, bouncing between the sides
d = int(step * 3 * SR)
L += dl; R += dr
L[d:] += dr[:-d] * 0.45; R[2 * d:] += dl[:-2 * d] * 0.30

# Star shimmers: high, short, scattered sparkles.
for i in range(140):
    start = rng.randint(0, N - SR)
    n = int(rng.uniform(0.4, 1.2) * SR)
    t = np.arange(n) / SR
    f = hz(rng.choice([86, 88, 89, 91, 93, 96]))
    v = np.sin(2 * np.pi * f * t) * np.exp(-t * 4) * np.sin(np.pi * t / t[-1]) * 0.012
    pan = rng.rand()
    L[start:start + n] += v * (1 - pan); R[start:start + n] += v * pan

# The satellite: beep-beep pairs every two bars, drifting in from far away, with radio hiss.
for bar in range(2, BARS, 2):
    base = bar * BAR * SR
    level = 0.5 + 0.5 * np.sin(np.pi * bar / BARS)
    for b in range(2):
        start = int(base + b * 0.32 * SR)
        n = int(0.16 * SR)
        t = np.arange(n) / SR
        beep = np.sin(2 * np.pi * 1320 * t) * env_adsr(n, 0.005, 0.03) * 0.03 * level
        L[start:start + n] += beep * 0.7; R[start:start + n] += beep * 0.3
    n = int(0.9 * SR)
    hiss = rng.randn(n) * np.sin(np.linspace(0, np.pi, n)) * 0.006 * level
    L[int(base):int(base) + n] += hiss; R[int(base):int(base) + n] += hiss * 0.8

# Smoothing (a gentle low pass), fades so it loops, a soft limit.
L = lowpass(L, 7000); R = lowpass(R, 7000)
fade = int(3 * SR)
for ch in (L, R):
    ch[:fade] *= np.linspace(0, 1, fade)
    ch[-fade:] *= np.linspace(1, 0, fade)
mix = np.stack([L, R], axis=1)
mix = np.tanh(mix * 1.6) / np.tanh(1.6)
mix = mix / max(1e-9, np.abs(mix).max()) * 0.85
pcm = (mix * 32767).astype(np.int16)

enc = lameenc.Encoder()
enc.set_bit_rate(128)
enc.set_in_sample_rate(SR)
enc.set_channels(2)
enc.set_quality(2)
data = enc.encode(pcm.tobytes()) + enc.flush()
os.makedirs(OUT_DIR, exist_ok=True)
open(OUT, "wb").write(data)
print("wrote %s: %.0f s, %d KB" % (OUT, N / SR, len(data) >> 10))
