# Deep Rumble

A deep, evolving, seismic rumble synthesizer for the [Bela](https://bela.io) embedded audio platform. All synthesis runs in real time on the Bela's BeagleBone Black, driven by 8 analog CV/potentiometer inputs and outputting stereo audio with 4 LED indicators.

The sound is intentionally unstable — five independent randomisation systems ensure it never repeats itself exactly.

---

## Hardware

| Item | Notes |
|---|---|
| Bela (original or Mini) | Tested at 44100 Hz, 16-sample block |
| 8 × potentiometers or CV sources | 0–3.3 V, wired to Analog In 0–7 |
| 4 × LEDs + resistors (~220 Ω) | Wired to Digital Out D0–D3, active HIGH |
| Stereo output | Bela audio out L/R |

Wire each potentiometer between 3.3 V and GND with the wiper to the corresponding analog pin. Bela normalises all analog reads to 0..1 internally.

---

## Installation

1. Open the Bela IDE in your browser (`http://bela.local`)
2. Create a new project
3. Replace the default `render.cpp` with `deep_rumble.cpp`
4. Click **Build & Run**

> **Note:** The file includes `<libraries/math_neon/math_neon.h>` for ARM NEON fast-math (`sinf_neon`). If this header is unavailable on your build, replace every `sinf_neon` call with `sinf` — the sound will be identical, marginally slower.

---

## Controls

All inputs accept 0–3.3 V. Potentiometers work well; the inputs are also CV-compatible for modular integration.

| Pin | Parameter | Range | Curve | Character |
|---|---|---|---|---|
| **Analog 0** | Master Pitch | 4–120 Hz | Exponential | Low end = infrasonic sub drones; high end = audible bass tones |
| **Analog 1** | Rumble Density | 1–8 layers | Linear | Adds detuned oscillator layers; each layer thickens and widens the sound |
| **Analog 2** | Filter Cutoff | 20–8000 Hz | Exponential | Low = smothered and muffled; high = wide open roar |
| **Analog 3** | Grain Texture | 0–1 | Quadratic | 0 = no grains; low = occasional seismic thuds; high = continuous gravel crunch |
| **Analog 4** | Noise Mix | 0–100% | Linear | 0 = pure oscillators; 1 = pure filtered noise; default sits noise-heavy |
| **Analog 5** | Tremolo Rate | 0.05–30 Hz | Exponential | Controls how often the irregular amplitude envelope picks a new random target |
| **Analog 6** | Distortion | 0–100% | Cubic | Low = warm saturation; top third = brutal hard clipping |
| **Analog 7** | Sub-octave Blend | 0–150% | Linear | Mixes in a pure sine at pitch/2; at max it can completely dominate the mix |

### Exponential curves

Pitch, Cutoff and Tremolo Rate all use exponential mappings so that musically useful ranges are spread evenly across the knob — turning a linear cutoff knob from 20 to 600 Hz would bunch all the interesting territory into one corner.

### Cubic distortion curve

The Distortion knob uses a cubed input (`drive³`) so the bottom 60% of the throw stays relatively clean. The top 30% transitions quickly from warm saturation to severe hard clipping.

---

## Sound Engine

```
Analog inputs
    │
    ▼
┌─────────────────────────────────────────────────────┐
│  8 × detuned oscillators (sine+triangle blend)      │
│  + sub-octave sine (pitch/2)                        │
│       │                                             │
│  Haas stereo widener (per-oscillator pan + delay)   │
└────────────────┬────────────────────────────────────┘
                 │
┌────────────────┴────────────────────────────────────┐
│  Broadband noise (independent L/R, decorrelated)    │
│  + random burst spikes (0.8–6 s intervals)          │
└────────────────┬────────────────────────────────────┘
                 │  Noise Mix crossfade
                 ▼
          Low-shelf boost (+6 dB @ 80 Hz)
                 │
          Cascaded 2× LP filter (24 dB/oct)
                 │
          Soft-clip + cubic waveshaper
                 │
┌────────────────┴────────────────────────────────────┐
│  Grain texture engine (added AFTER main LP)         │
│  Up to 16 simultaneous bandpass-filtered bursts     │
│  each with independent pan, length, and BP freq     │
└────────────────┬────────────────────────────────────┘
                 │
          Irregular tremolo envelope
                 │
          Mid-Side width LFO (0.07 Hz)
                 │
          Schroeder reverb (4 combs + 2 all-pass)
          LP-gated input — only bass enters the tail
                 │
             Stereo out
```

### Grain texture engine

Grains are short bursts of bandpass-filtered noise with exponential decay envelopes. Each grain is triggered stochastically: at low texture the interval between triggers is ~180 ms (sparse, thunderous); at high texture intervals collapse to ~1 ms (dense, continuous gravel). Each grain picks a random bandpass centre between 80 and 1200 Hz, a random stereo pan, and a random duration (12–200 ms). Up to 16 grains run simultaneously.

Grains are summed **after** the main LP filter chain so they are not attenuated by it.

### Reverb

A classic Schroeder architecture: 4 parallel comb filters with one-pole HF damping in the feedback loop, followed by 2 series all-pass filters for diffusion. L and R instances use slightly different delay lengths (~23 samples apart) for natural stereo spread in the tail. Only frequencies below ~200 Hz feed the reverb, keeping the tail warm and avoiding mid-range muddiness.

---

## Irregularity Systems

Five independent randomisation processes prevent the sound from ever settling into a predictable pattern:

**A — Per-oscillator pitch drift.** Each oscillator independently wanders ±2.5% from its base detune ratio, picking new random targets every 1–5 seconds (staggered so they never all shift at once). The result is a slowly morphing cluster of pitches that never forms the same chord twice.

**B — Irregular tremolo.** Rather than a sine LFO, the amplitude modulation uses a sample-and-hold random envelope: a new target level (0.2–1.0) is chosen at random intervals and smoothly interpolated toward. The Tremolo Rate knob sets how frequently new targets are chosen — slow gives gradual lurching swells, fast gives rapid unpredictable stuttering.

**C — Filter cutoff wander.** An additive random offset (±180 Hz) is applied to the filter cutoff, wandering slowly at block rate. The timbre shifts organically without being obviously periodic.

**D — Noise burst spikes.** Every 0.8–6 seconds a random amplitude surge (1.5–4× gain) fires on the broadband noise layer, decaying back with a one-pole envelope. These land like distant thunder against the continuous rumble.

**E — Sub-octave flutter.** The sub-octave oscillator's frequency is perturbed by up to ±0.8 Hz at random intervals (0.3–1.8 s), causing the deepest bass note to subtly drift and waver.

---

## LED Indicators

LEDs are driven from digital pins D0–D3 (active HIGH). Intermediate brightness levels are achieved with block-rate PWM (16 steps), running at ~2.7 kHz at a 16-sample block size — well above flicker threshold.

| Pin | LED | Signal |
|---|---|---|
| **D0** | Tremolo | Tracks the irregular tremolo envelope — dims and brightens as the amplitude lurches |
| **D1** | Grain flash | Strobes on every grain trigger; sparse at low texture, rapid flicker at high texture |
| **D2** | Output VU | Peak-hold envelope of the final stereo output; instant attack, ~300 ms decay |
| **D3** | Sub heartbeat | Toggles state on each cycle of the sub-octave oscillator — visibly slow at low pitch, blurs to a dim glow at higher pitches |

---

## Tuning the Internal Constants

These are defined at the top of `deep_rumble.cpp` and do not require recompiling filter coefficients at runtime:

| Constant | Default | Effect |
|---|---|---|
| `STEREO_WIDTH` | 0.72 | Pan spread of the oscillator bank (0 = mono, 1 = fully wide) |
| `REVERB_MIX` | 0.20 | Wet/dry blend of the reverb tail |
| `REVERB_DAMP` | 0.45 | HF damping inside comb filter feedback (0 = bright, 1 = dull) |
| `REVERB_ROOM` | 0.84 | Comb filter feedback amount — controls tail length |
| `BASS_SHELF_GAIN` | 6.0 dB | Low-shelf boost at ~80 Hz applied before the LP filter |

Additional irregularity parameters (drift depth, wander range, burst intensity etc.) are defined as `static const` values near the top of the irregularity state section and are straightforward to adjust.

---

## Project Structure

```
deep_rumble/
├── deep_rumble.cpp    Single-file Bela project (drop in as render.cpp)
└── README.md          This file
```

---

## Licence

Released under the MIT Licence. Use freely, modify freely, make something loud.
