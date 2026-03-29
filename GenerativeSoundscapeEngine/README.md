# Generative Soundscape Engine — Bela

A sample-based generative ambient engine for the Bela platform. It randomly triggers audio samples with varying pitch, pan, and level, shaped by attack/release envelopes and routed through a chain of real-time effects. Eight potentiometers give you hands-on control over the density, texture, and character of the resulting soundscape. Four LEDs provide visual feedback.

## How It Works

At startup the engine loads all `.wav` files from the `samples/` directory, analyses each one for its RMS energy, and ranks them from quietest to loudest. During playback a scheduler triggers new voices at random intervals — each voice plays a sample picked from the bank with randomised pitch, pan, and level, faded in and out by a half-second attack/release envelope.

The Texture knob biases sample selection toward calm or intense material and also influences pitch range, while the Density knob controls how many voices overlap and how frequently new ones are triggered. The signal passes through an effects chain of overdrive, low-pass filter, tape delay, and Schroeder reverb — all continuously adjustable.

The output is soft-clipped via `tanh` to prevent harsh digital clipping while allowing warm saturation at higher levels.

## Knob Mapping

The eight potentiometers connect to analog inputs 0–7. Each one is smoothed to avoid zipper noise.

| Analog In | Name       | What It Does                                                                 |
|-----------|------------|------------------------------------------------------------------------------|
| 0         | Density    | Number of simultaneous voices (1–10) and trigger frequency                   |
| 1         | Reverb     | Dry/wet mix of Schroeder reverb                                              |
| 2         | Delay Mix  | Dry/wet mix of tape-style delay                                              |
| 3         | Delay Time | Tape delay time (50 ms – 750 ms)                                            |
| 4         | Drive      | Overdrive/saturation amount (soft clipping via tanh)                         |
| 5         | Filter     | Low-pass filter cutoff (200 Hz – 7 kHz). Above 90% the filter is bypassed   |
| 6         | Texture    | Sample energy bias (calm ↔ intense) and pitch depth                          |
| 7         | Volume     | Master output level (quadratic taper for fine control at low levels)          |

## LED Mapping

Four LEDs connect to digital outputs 0–3.

| Digital Out | Name      | Behaviour                                     |
|-------------|-----------|-----------------------------------------------|
| 0           | Activity  | Lights when more than 2 voices are active      |
| 1           | Trigger   | Flashes briefly each time a new voice starts   |
| 2           | Level     | Lights when the output signal is present       |
| 3           | Heartbeat | Slow pulse indicating the system is running    |

## Hardware Wiring

Potentiometers should be 10 kΩ linear taper, wired with the wiper to the corresponding analog input. LEDs need a current-limiting resistor (220 Ω typical) between the digital output pin and the LED anode.

## Samples

Create a `samples/` subdirectory inside the project folder and place your `.wav` files there. Mono or stereo, any sample rate — stereo files are mixed to mono at load time. Up to 32 samples are supported.

```
bela-soundscape/
├── render.cpp
├── Effects.h
├── Effects.cpp
├── settings.json
└── samples/
    ├── drone_pad.wav
    ├── field_recording.wav
    ├── texture_01.wav
    └── ...
```

At startup the console prints the energy analysis for each sample, showing you the quiet-to-loud ranking that the Texture knob uses.

## Effects Chain

The signal flows through these stages in order:

1. **Voice mixer** — Up to 10 voices are summed with equal-power panning, then scaled by `1/√(active voices)` to maintain consistent headroom.
2. **Overdrive** — Soft saturation via `tanh`. Gain range 1× – 6×. Bypassed when the knob is below 5%.
3. **Low-pass filter** — Chamberlin state-variable filter with NaN recovery and stability limiting. Bypassed when the knob is above 90%.
4. **Tape delay** — Filtered feedback path with saturation, giving a warm analog character. Left and right channels use offset delay times for stereo width.
5. **Reverb** — Schroeder reverb (4 comb filters + 2 allpass filters), with slightly different decay times for left and right to widen the stereo image.
6. **Output limiter** — Soft clipping via `tanh` to prevent harsh digital overs.

## Building & Running

Upload the entire project folder to your Bela via the IDE or `scp`, then build and run:

```bash
# From the Bela board
make -C /root/Bela PROJECT=bela-soundscape run
```

Or open the folder in the Bela IDE and press Run.

The project links against `libsndfile` (pre-installed on Bela) for sample loading. Compilation uses `-Os` optimisation to fit within the BeagleBone's limited RAM.

### If compilation fails with Error 137

The BeagleBone may not have enough RAM for the compiler. Create a swap file by running:

```bash
dd if=/dev/zero of=/var/swap bs=1M count=256
chmod 600 /var/swap
mkswap /var/swap
swapon /var/swap
```

Then try building again. To make the swap permanent, add `/var/swap none swap sw 0 0` to `/etc/fstab`.

## Technical Notes

- All DSP is written in plain C (structs and functions) to minimise compiler memory usage on the BeagleBone.
- Effect buffers are heap-allocated at runtime via `calloc` rather than statically, further reducing compile-time memory.
- Sample selection uses a xorshift PRNG rather than the C++ Mersenne Twister to avoid heavy template instantiation.
- The Texture knob applies a soft Gaussian bias (sum of 3 uniform randoms) across the energy-sorted sample index, so there is always some variety even at extreme knob positions.
- Pitch is always at or below the original sample rate (ratio 0.25 – 1.0), so no interpolation artefacts from upward transposition.

## File Overview

| File           | Purpose                                                       |
|----------------|---------------------------------------------------------------|
| `render.cpp`   | Bela setup/render/cleanup, voice management, knob + LED logic |
| `Effects.h`    | C struct declarations for all DSP modules                     |
| `Effects.cpp`  | DSP implementations (filter, reverb, delay, drive, envelope)  |
| `settings.json`| Compiler flags and linker settings                            |
