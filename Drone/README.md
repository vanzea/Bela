# BLACK DIFFUSE AURORAL

A drone synthesiser for the [Bela](https://bela.io) platform.

It reconstructs the sonic character of *N_O_I_S_U — "Black Diffuse Auroral"*:
a sub-heavy, slowly beating low-frequency oscillator cluster, soft-saturated,
smeared through a long diffusion reverb, with a fine granular crackle layer on
top and a fully decorrelated stereo field. Eight potentiometers turn that fixed
recording into a playable instrument; four LEDs give visual feedback.

---

## Table of contents

- [What this is](#what-this-is)
- [How the design was derived](#how-the-design-was-derived)
- [Signal chain](#signal-chain)
- [The eight potentiometers](#the-eight-potentiometers)
- [The four LEDs](#the-four-leds)
- [Hardware setup](#hardware-setup)
- [Building and running](#building-and-running)
- [Tuning and patch ideas](#tuning-and-patch-ideas)
- [Code structure](#code-structure)
- [CPU and memory notes](#cpu-and-memory-notes)
- [Known limitations](#known-limitations)
- [License and credit](#license-and-credit)

---

## What this is

This is a single-file Bela program (`render.cpp`). It does not sample or play
back the source recording — it is a *synthesis model* whose parameters were
chosen so that, at the centre of every knob's travel, the instrument sits on
the analysed character of the original track. Turning the knobs walks the sound
into a wide territory around that point: deeper sub, brighter haze, denser
crackle, longer or shorter diffusion, mono through to fully phasey stereo.

The design goal was an *instrument*, not a fixed re-creation. If you want the
knobs to hug the original tightly instead, the parameter ranges in
`render()` are the place to narrow them.

---

## How the design was derived

The model is not guesswork. The source MP3 (44.1 kHz stereo, ~11.8 minutes)
was decoded to raw PCM and measured. The findings that shaped the algorithm:

| Property | Measurement | Consequence for the design |
|---|---|---|
| Energy distribution | 81 % of all energy below 160 Hz; the 40–80 Hz octave alone holds 55 % | The whole instrument is built around a sub-bass core. |
| Spectral centroid | ~114 Hz overall; drifts 70 → 190 Hz across the track | Centre-knob sound is dark; Drive and Grain Tone open it up. |
| Bass fine structure | A dense cluster of partials near 30, 33, 39, 46, 50, 57, 65, 67, 90, 103, 134 Hz, spaced 3–7 Hz apart | Six **detuned** sine partials with **inharmonic** ratios, not a clean tone. The close spacing *is* the beating. |
| Bottom octave | A continuous foundation of energy down to ~20 Hz, below the lowest cluster partial | A dedicated deep-sine sub-bass floor, an octave below the cluster, holding 20–35 Hz. |
| Tonality | Long-term spectral flatness near zero | The core is pitched/tonal, hence oscillators rather than noise. |
| Transient layer | ~5 onsets/sec, ~24 ms attack, long decay | A granular crackle layer with a soft 24 ms attack, riding the drone. |
| Stereo image | L/R correlation ≈ −0.02; side and mid energy equal | A reverb with fully divergent L/R delay lines for true decorrelation. |
| Amplitude movement | Dominant modulation ~0.06 Hz (a ~16 s swell), plus a ~0.25 Hz shimmer | A very slow swell LFO breathing the cluster. |
| Levels | Peak −1.6 dBFS, RMS −19.3 dBFS, crest 17.7 dB | Output gain-staged to land near −19 dBFS RMS with headroom. |

The synthesised output was rendered back offline and re-measured to confirm it
lands inside these target ranges (RMS ≈ −18 dB, L/R correlation ≈ −0.11,
sub-bass dominant with the 15–25 Hz bottom octave matching the source, no
NaN/Inf).

---

## Signal chain

```
                       Pot 1 Pitch   Pot 2 Spread
                            |             |
              6 x DriftSine  (inharmonic, slow random pitch drift)
                            |
                       sum / 6
                            |
                   slow swell LFO  <-- Pot 8 Swell
                            |
                  asymmetric tanh saturation  <-- Pot 3 Drive
                            |
        +-------------------+--------------------+
        |                                        |
   GrainLayer  <-- Pot 4 Density, Pot 5 Tone     (dry mono)
        |                                        |
        +------------------ sum -----------------+
                            |
              DiffuseReverb  (4 combs -> 4 allpass, per channel)
                            |             ^
                            |             |
                            |        Pot 6 Diffusion
                   wet L / wet R  (divergent L/R delay lines)
                            |
                  width crossfade (mono <-> decorrelated)  <-- Pot 7 Width
                            |
                            +  sub-bass floor  <-- Pot 3 Drive
                            |   (deep sine, octave below cluster,
                            |    bypasses the reverb)
                            |
                  DC blocker -> master trim -> soft clip
                            |
                       audio out L / R
```

Each stage in plain terms:

1. **DriftSine cluster.** Six sine oscillators. Their frequency ratios come
   straight from the analysed peak set, normalised so they are deliberately
   *not* a clean harmonic series. Each partial also has its own very slow
   random pitch drift, which keeps the beating alive and non-repeating.

2. **Swell LFO.** A ~0.06 Hz sine that breathes the cluster amplitude. Depth is
   on Pot 8; at zero it is steady, at full it produces a deep ~16-second
   crescendo/decay.

3. **Asymmetric tanh saturation.** A small DC bias before the `tanh` produces
   even-order harmonics, adding the gentle haze the analysis found up to about
   1.5 kHz. A squared term sprinkles in a touch more high-order content at high
   drive. Gain compensation keeps Drive a *timbre* control, not just a volume.

4. **Sub-bass floor.** The discrete partial cluster leaves the bottom octave
   thin — its lowest partial sits near 30 Hz with nothing below. The source has
   a continuous foundation down to ~20 Hz, so a dedicated deep sine, an octave
   below the cluster fundamental (clamped to 20–42 Hz), holds that floor. It is
   lightly soft-clipped for weight, breathes with the swell LFO, and
   deliberately **bypasses the reverb** — a long diffusion tail on a ~25 Hz tone
   would build muddy rumble, so the sub is added back as a clean, centred floor
   after the reverb instead. Its level is governed by the Drive knob (see Pot 3
   below).

5. **GrainLayer.** Short bandpass-filtered noise grains with a soft 24 ms
   attack and a squared decay. Density and bandpass centre are on Pots 4 and 5.

6. **DiffuseReverb.** A Freeverb-style topology — four parallel feedback combs
   into four series allpasses — but with the left and right delay lengths set
   far apart so the two channels diffuse into independent fields. This is what
   produces the near-zero stereo correlation.

7. **Width crossfade.** Blends between a mono sum and the decorrelated reverb
   pair, so Pot 7 sweeps mono → fully phasey. The clean sub-bass floor is added
   here, equally to both channels, so it stays solid and centred.

8. **Output stage.** A per-channel DC blocker, a fixed master trim that lands
   the level near the measured −19 dBFS RMS, and a `tanh` soft clip that should
   only ever engage as a safety net.

---

## The eight potentiometers

Wired to analog inputs 0–7. Every pot is smoothed by a one-pole filter, so
there is no zipper noise. The centre of each knob's travel is tuned to the
analysed source character.

| # | Analog in | Name | Range | What it does |
|---|---|---|---|---|
| 1 | 0 | **Drift Pitch** | 22–112 Hz, exponential (centre ≈ 50 Hz) | Base frequency of the whole cluster. |
| 2 | 1 | **Spread** | 0 → ~0.06 detune depth | Detuning between the six partials. Low = fused single tone; high = wide, churning beating. |
| 3 | 2 | **Drive** | tanh pre-gain 1 → 14, plus sub-bass floor level | Saturation into harmonic haze, *and* the level of the deep sub-bass floor. Past the centre this is the seismic, sub-heavy territory — both the grit and the sub rise together. A baseline of sub is always present even at minimum Drive, so the bottom octave is never empty. |
| 4 | 3 | **Grain Density** | 0 → ~12 grains/sec | Rate of the granular crackle layer. Fully down = silent. |
| 5 | 4 | **Grain Tone** | 60 Hz – 6 kHz, exponential | Bandpass centre of the grains: dark thud → airy crackle. |
| 6 | 5 | **Diffusion** | reverb feedback 0.70 → 0.965 | Length of the reverb tail — the "auroral" smear. Damping eases off as it opens, so longer is also slightly brighter. |
| 7 | 6 | **Width** | mono → full decorrelation | Stereo image, from centred mono to fully phasey. |
| 8 | 7 | **Swell** | 0 → full depth | Depth of the slow ~0.06 Hz amplitude breathing. |

---

## The four LEDs

Wired to digital outputs 0–3. Brightness is dithered at block rate, so these
behave like dim/bright indicators rather than simple on/off.

| # | Digital out | Indicates |
|---|---|---|
| 0 | 0 | Cluster swell level (the slow breathing). |
| 1 | 1 | Grain trigger flash — blinks on each crackle grain. |
| 2 | 2 | Drive amount. |
| 3 | 3 | Reverb tail energy. |

---

## Hardware setup

- A Bela or Bela Mini board.
- 8 potentiometers, each wired across 3.3 V and ground with the wiper to analog
  inputs 0–7. Linear-taper pots are fine; the code applies exponential mapping
  in software where it matters.
- 4 LEDs on digital pins 0–3, each in series with a current-limiting resistor
  (≈220 Ω), to ground.
- Stereo line/headphone output. Note this patch is **bass-heavy** — use
  monitoring that can actually reproduce content down to ~25 Hz, or you will
  not hear most of what the instrument is doing.

The digital pin directions are set automatically by Bela from the project
settings; ensure pins 0–3 are configured as **outputs** in the IDE project
settings, or set them in `setup()` with `pinMode()` if you adapt the code.

---

## Building and running

### Using the Bela IDE (recommended)

1. Open the Bela IDE in your browser (the board serves it at `bela.local`).
2. Create a new C++ project.
3. Replace the project's `render.cpp` with the one from this repository.
4. Press **Run**.

### From the command line

Copy the file to the board and build with the standard Bela makefile:

```sh
scp render.cpp root@bela.local:~/Bela/projects/black_diffuse_auroral/
ssh root@bela.local
cd ~/Bela
make PROJECT=black_diffuse_auroral run
```

### A note on compiler warnings

The file compiles cleanly under a strict warning set:

```
-Wall -Wextra -Wconversion -Wfloat-conversion -Wsign-conversion
```

The only warnings you may still see are *unused parameter* notices for the
`userData` and `context` arguments — those are part of Bela's mandated
function signatures and are harmless.

---

## Tuning and patch ideas

Starting points, all reachable with the knobs:

- **Faithful "Black Diffuse Auroral".** Every knob near centre. Pitch ≈ 50 Hz,
  moderate Spread, Drive just past centre, light Grain Density, dark Grain
  Tone, long Diffusion, wide Width, moderate Swell.
- **Pure sub drone.** Grain Density fully down, Pitch low, Spread low, Drive
  low to moderate. A clean, almost sine-like foundation tone — note that even
  at low Drive the sub-bass floor keeps the bottom octave present.
- **Seismic / earthquake.** Pitch low, Drive high, Diffusion long. High Drive
  pushes both the asymmetric saturation (a dense low-order harmonic stack) and
  the sub-bass floor together — this is the deliberately physical, room-shaking
  setting.
- **Granular texture bed.** Drive low, Grain Density high, Grain Tone bright,
  Diffusion long. The drone recedes and the crackle becomes the foreground.
- **Wide ambient pad.** Spread high, Diffusion near maximum, Width full, Swell
  deep. Slow, evolving, fully decorrelated.

If you want to change the *core* character rather than just play it:

- The six partial ratios are in `setup()` (the `ratios[]` array). They are the
  inharmonic fingerprint of the cluster — edit these to retune the beating.
- The reverb delay lengths are in `DiffuseReverb::init()` (`cl/cr/al/ar`).
  Keeping the L and R sets far apart is what preserves the decorrelation.
- The master level is the `kMasterTrim` constant in `render()`.

---

## Code structure

`render.cpp` is self-contained. Reading top to bottom:

- **Constants and helpers** — sample-rate globals, `clampf`, the `OnePole`
  control smoother, and the random helpers (`frand`, `frand01`).
- **`DriftSine`** — one sine partial with slow random pitch drift.
- **`Allpass`** — a Schroeder allpass section (diffusion building block).
- **`Comb`** — a feedback comb filter with damping (Freeverb-style).
- **`DiffuseReverb`** — the full stereo reverb: four combs into four allpasses
  per channel, with divergent L/R delay lengths.
- **`GrainLayer`** — the granular crackle generator.
- **Global state** — the cluster, reverb, grain layer, the sub-bass floor
  oscillator phase, control smoothers.
- **`setup()`** — initialises ratios, buffers and smoother time-constants.
- **`render()`** — reads and smooths the pots, maps them to musical ranges,
  runs the per-sample synthesis loop, and drives the LEDs.
- **`cleanup()`** — buffers are freed by the `Comb`/`Allpass` destructors.

The full pin map is also repeated as a comment block at the end of the file.

---

## CPU and memory notes

- The synthesis loop is light: six `sinf` calls, a `tanhf`, one grain voice,
  and an eight-element reverb per sample. It runs comfortably in real time on a
  standard Bela at 44.1 kHz.
- All buffers are allocated once in `setup()`. There is **no allocation,
  locking, or file I/O inside `render()`** — it is real-time safe.
- Pots are read once per audio block, not per sample, and then smoothed; this
  is both cheaper and correct, since analog inputs run slower than audio.
- Total reverb memory is a few tens of kilobytes — negligible.

---

## Known limitations

- At the exact centre-knob snapshot the instrument sits at the **dark end** of
  the source's range (centroid ≈ 60 Hz). The original track itself brightens to
  ~190 Hz as it develops; Drive and Grain Tone are how you reach that.
- The grain layer is monophonic — one grain at a time. At very high density
  grains overlap by retriggering, which thins rather than stacks. This matches
  the source's *crust* character; it is not a polyphonic granular engine.
- The "BPM" that a beat-tracker might report from the output is not a rhythm —
  it is the statistical regularity of the grain layer, exactly as in the
  source.

---

## License and credit

Synthesis model and code: free to use, modify and build on.

The instrument is an original DSP model. It was *informed by* analysis of
*N_O_I_S_U — "Black Diffuse Auroral"* but contains no audio from that
recording; all rights in the original work remain with its creator.
