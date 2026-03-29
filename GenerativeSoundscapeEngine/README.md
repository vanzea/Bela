# Generative Soundscape Engine — Bela

A sample-based generative ambient engine for the Bela platform. It continuously triggers audio samples in random combinations, creating evolving soundscapes shaped in real time by eight potentiometers. Samples are stored as 8-bit mu-law encoded audio to maximise the number of samples that fit in the BeagleBone's limited RAM.

## How It Works

At startup the engine loads all `.wav` files from the `samples/` directory and compresses each one to 8-bit mu-law encoding (roughly 1 byte per sample, compared to 4 bytes for raw float). Each sample is analysed for its RMS energy and ranked from quietest to loudest.

During playback a scheduler triggers new voices at random intervals. Each voice picks a sample from the bank, influenced by the Energy knob, and assigns it a random pitch (within the range set by the Pitch knob), level, and stereo pan position. A half-second attack/release envelope fades each voice in and out smoothly.

Up to 10 voices can play simultaneously. The mixed output passes through an overdrive and a low-pass filter before being soft-clipped to the stereo outputs.

## Knob Mapping

Eight potentiometers connect to analog inputs 0–7. All values are smoothed to prevent zipper noise.

| Knob | Name    | What It Does |
|------|---------|-------------|
| 0    | Density | Number of simultaneous voices (1–10) and how frequently new voices are triggered. High = dense and busy, low = sparse and spacious. |
| 1    | Energy  | Biases sample selection by RMS energy. Fully CCW favours quiet, calm samples. Fully CW favours loud, intense samples. There is always some randomness — the knob shifts the centre of a soft probability curve. |
| 2    | Pitch   | Sets the random pitch range for newly triggered voices. Fully CCW = original pitch only. Fully CW = random pitch between original and one octave lower (50% speed). Each voice gets its own random pitch within this range at trigger time. |
| 3    | *(free)* | Wired but unassigned. Available for future use. |
| 4    | Drive   | Overdrive amount. Soft saturation via tanh, gain range 1–6×. Below 5% the overdrive is bypassed. |
| 5    | Filter  | Low-pass filter cutoff, 200 Hz – 7 kHz. Above 90% the filter is bypassed entirely so the full spectrum passes through. |
| 6    | Speed   | Global playback speed multiplier applied to all voices in real time. Fully CW = normal speed. Turning CCW slows all currently playing voices down continuously, approaching frozen. Uses a cubic taper so most of the knob range gives useful slow-motion. Minimum speed is 0.5% (never fully stops). |
| 7    | Volume  | Master output level with a quadratic taper for fine control at lower levels. |

The difference between Pitch (knob 2) and Speed (knob 6): Pitch sets a random value per voice when it starts — once a voice is playing, its pitch is fixed. Speed affects all playing voices at once in real time, so turning it mid-playback stretches or compresses everything you hear.

## LED Mapping

Four LEDs connect to digital outputs 0–3.

| Digital Out | Name      | Behaviour |
|-------------|-----------|-----------|
| 0           | Activity  | On when more than 2 voices are active |
| 1           | Trigger   | Flashes briefly each time a new voice starts |
| 2           | Level     | On when the output signal is present |
| 3           | Heartbeat | Slow pulse indicating the system is running |

## Hardware Wiring

Potentiometers: 10 kΩ linear taper. Wire the outer pins to 3.3V and ground, the wiper to the corresponding analog input.

LEDs: Connect each LED's anode through a 220 Ω resistor to the digital output pin, cathode to ground.

## Samples

Create a `samples/` subdirectory inside the project folder and place your `.wav` files there. Mono or stereo at any sample rate — stereo files are mixed to mono at load time, and sample rate differences are automatically compensated during playback.

Up to 256 samples are supported. Each one is capped at 60 seconds. At 44.1 kHz mono, each second of audio uses about 44 KB in RAM (one byte per sample thanks to mu-law encoding), so a full set of 256 ten-second samples would use roughly 110 MB.

```
bela-soundscape/
├── render.cpp
├── Effects.h
├── Effects.cpp
├── settings.json
└── samples/
    ├── texture_calm_01.wav
    ├── field_recording.wav
    ├── drone_intense.wav
    └── ...
```

At startup the console prints each loaded sample with its frame count, original sample rate, playback ratio, and RMS energy, followed by the total RAM usage and the energy ranking.

## Console Output

During playback, each triggered voice prints a line like:

```
► Voice 3: [47] field_recording.wav  pitch=0.82  level=0.42  pan=0.73  dur=11.7s
```

Showing the voice slot, sample index, filename, pitch ratio (including sample rate compensation), random level, pan position, and estimated playback duration.

## Signal Chain

1. **Voice mixer** — Up to 10 voices are summed with equal-power panning, then scaled by 1/√(active voices) to maintain consistent headroom regardless of voice count.
2. **Overdrive** — Soft saturation via tanh, gain 1–6×. Bypassed below 5%.
3. **Low-pass filter** — Chamberlin state-variable filter with stability clamping and automatic NaN recovery. Bypassed above 90%.
4. **Output limiter** — Soft clipping via tanh to prevent harsh digital overs.

## Building and Running

Upload the project folder to your Bela via the IDE or scp, then build and run:

```bash
make -C /root/Bela PROJECT=bela-soundscape run
```

Or open the folder in the Bela IDE and press Run. Do a clean build when switching between versions (Project Explorer → Manage projects → Clean project).

The project links against libsndfile (pre-installed on Bela) for sample loading. Compilation uses `-Os` optimisation to fit within the BeagleBone's limited RAM.

### If compilation fails with Error 137

The BeagleBone may not have enough RAM for the compiler. Create a swap file:

```bash
dd if=/dev/zero of=/var/swap bs=1M count=256
chmod 600 /var/swap
mkswap /var/swap
swapon /var/swap
```

Then build again. Add `/var/swap none swap sw 0 0` to `/etc/fstab` to make it permanent across reboots.

## Technical Notes

- All DSP is plain C (structs and functions wrapped in `extern "C"`) to minimise compiler memory usage.
- Samples are stored as 8-bit mu-law encoded bytes. A 256-entry lookup table handles decoding during playback — one table lookup plus a lerp per sample per voice.
- Sample loading reads in 4096-frame chunks to avoid large temporary buffers.
- The path list for directory scanning is heap-allocated and freed after loading.
- Random number generation uses a xorshift PRNG (zero memory, no template instantiation).
- Sample rate differences between files and the Bela are compensated automatically via a per-sample playback ratio.
- The Energy knob uses a soft Gaussian-like bias (sum of 3 uniform randoms) across the energy-sorted sample index, so there is always variety even at extreme positions.

## File Overview

| File           | Purpose |
|----------------|---------|
| render.cpp     | Bela setup/render/cleanup, voice management, knob and LED logic, sample loading |
| Effects.h      | C struct declarations for all DSP modules and mu-law codec |
| Effects.cpp    | DSP implementations: filter, overdrive, envelope, mu-law encode/decode |
| settings.json  | Compiler flags (`-Os`) and linker settings (`-lsndfile`) |
