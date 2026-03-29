# Granulita Bela

A real-time granular synthesizer for the [Bela](https://bela.io) platform, inspired by the Granulita Versio eurorack module. Audio is continuously recorded into a 4-second stereo circular buffer; grains are read back from any point in that buffer with independent control over pitch, density, size, spray, and panning — all driven by 8 analog inputs.

---

## Files

| File | Description |
|---|---|
| `render.cpp` | Main synthesizer — drop into a Bela IDE project |
| `project.json` | Bela project settings (sample rate, block size, I/O counts) |

---

## Hardware connections

### Audio

| Bela jack | Signal |
|---|---|
| Audio in L (ch 0) | Stereo left input |
| Audio in R (ch 1) | Stereo right input |
| Audio out L (ch 0) | Granular output left |
| Audio out R (ch 1) | Granular output right |

Mono sources can be patched into ch 0 only — ch 1 mirrors it automatically.

### Analog inputs — 8 continuous controllers

All inputs expect **0–5 V** (Bela normalizes to 0.0–1.0 internally). Eurorack CV goes in directly via the Bela analog header.

| Pin | Parameter | Range |
|---|---|---|
| A0 | **Position** | Scrub point through the 4 s record buffer (0 = oldest, 1 = newest) |
| A1 | **Grain size** | 5 ms – 500 ms |
| A2 | **Density** | 1 – 64 grains / second |
| A3 | **Transpose** | −24 to +24 semitones (centre = unity pitch) |
| A4 | **Spray** | Position randomness — scatter grains around the A0 point |
| A5 | **Pan spread** | Stereo field width (0 = mono centre, 1 = maximum random panning) |
| A6 | **Envelope shape** | 0 = triangle window, 1 = Hann window, intermediate values blend the two |
| A7 | **Dry / wet** | 0 = dry input pass-through, 1 = 100% granular wet signal |

All CV inputs are passed through a one-pole lowpass smoother (`kSmoothCoeff = 0.005`) to prevent stepping artifacts from pot jitter or coarse ADC resolution.

### LEDs — digital outputs D0–D3

Wire each LED with a **220–470 Ω series resistor** between the Bela digital pin and the LED anode. Connect cathodes to GND.

| Pin | Behaviour |
|---|---|
| D0 | **Spawn flash** — pulses high (~23 ms) on every grain spawn. Blinks slowly at low density; becomes a rapid flicker or stays mostly on at high density. |
| D1 | **Density brightness** — software PWM whose duty cycle tracks the number of currently active grains (0 = no grains, full brightness = 64 grains). |
| D2 | **Pitch up** — on when A3 is above centre (cloud pitched above unity). Off at unity or below. A small deadband (±2%) prevents flickering at noon. |
| D3 | **Clip warning** — lights when the pre-tanh grain sum exceeds the saturation threshold. Back off the input level or reduce density if this stays on. |

---

## Signal flow

```
Audio in L/R
     │
     ▼
Circular buffer (4 s stereo, interleaved)
     │
     ├──────────────────────────────────────┐
     │                                      │
     ▼                                      ▼
Grain scheduler ◄── CV smoother ◄── A0–A7  Grain pool (up to 64 grains)
     │                                      │
     │  spawn()                             │  per grain:
     └─────────────────────────────────────►│  · hermite interpolation
                                            │  · envelope (tri / Hann blend)
                                            │  · pitch (2^(st/12))
                                            │  · equal-power panning
                                            │
                                            ▼
                                     Accumulate sum
                                            │
                                     tanh soft-clip (× 0.5)
                                            │
                                     Dry / wet blend (A7)
                                            │
                                     Audio out L/R
```

---

## Technical details

**Circular recording.** Audio is written into a heap-allocated interleaved stereo float buffer on every sample. The write head wraps at `kBufferSeconds × sampleRate` frames (default 4 s). Grains can read from anywhere in this buffer, including material being written in real time.

**Cubic Hermite interpolation.** Each grain reads the buffer at a fractional position using a 4-point Hermite polynomial. This gives smooth transposition across the full ±24 semitone range without audible zipper or aliasing artifacts.

**Grain pool.** Grains are stored in a fixed-size `std::array<Grain, 64>`. Spawning and retiring grains never allocates memory, making the audio thread allocation-free after `setup()`.

**Software PWM for D1.** The density LED is driven by comparing a per-sample counter against a threshold derived from the smoothed active-grain ratio. The PWM period is 256 samples (~5.8 ms at 44.1 kHz), short enough to appear as continuous brightness to the eye but long enough to avoid inducing audible interference on nearby analog signals.

**Clip detection.** The clip flag is evaluated on the raw accumulated grain sum *before* the `tanhf(x × 0.5)` soft-clip stage. The threshold is 1.5, which realistically fires when multiple loud grains overlap. Checking after tanh would never trigger because the function asymptotically approaches ±1.

---

## Tunable constants

All key parameters are `constexpr` values at the top of `render.cpp` and can be changed without restructuring the code.

| Constant | Default | Effect |
|---|---|---|
| `kBufferSeconds` | `4` | Length of the record buffer in seconds |
| `kMaxGrains` | `64` | Maximum simultaneous grains |
| `kMinGrainMs` / `kMaxGrainMs` | `5` / `500` | Grain size range in milliseconds |
| `kMinDensity` / `kMaxDensity` | `1` / `64` | Density range in grains per second |
| `kMaxTranspose` | `24` | Semitone range for pitch transpose (±) |
| `kSmoothCoeff` | `0.005` | CV smoothing tightness — higher = faster response, more stepping |
| `kSpawnPulseLen` | `1024` | D0 flash length in samples (~23 ms at 44.1 kHz) |
| `kPWMPeriod` | `256` | D1 PWM period in samples |
| `kClipThreshold` | `1.5` | Pre-tanh sum level that triggers the D3 clip LED |

---

## Building

1. Open the [Bela IDE](http://bela.local) in your browser.
2. Create a new C++ project.
3. Replace the default `render.cpp` with the one from this repository.
4. Copy `project.json` into the project folder (or configure I/O manually in the IDE settings: 2 audio in, 2 audio out, 8 analog in, 4 digital out, block size 16).
5. Click **Build & Run**.

No external libraries are required beyond the standard Bela API.

---

## License

MIT — do whatever you like with it.
