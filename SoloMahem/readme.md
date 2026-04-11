# Solo Mayhem

A stereo audio effect for the **Bela** platform, designed for live keyboard and synthesizer solos. One macro knob takes you from clean to complete chaos, while seven character knobs and four LEDs let you shape and monitor the sound as you play.

## Concept

Solo Mayhem is built around a single **Intensity** macro (P0) that walks through a curated path of effects: drive → ring modulation → granular freezer → stereo ping-pong delay → stereo reverb. Each block fades in at a different point along the macro, so the sweep feels like a journey rather than a single wet/dry blend.

The dry signal is preserved at unity gain throughout the entire chain — your keyboard always sits exactly where it is in the mix, and the wet bus is added in parallel on top.

## Hardware

- **Bela** or **Bela Mini** with cape
- 8× 10kΩ potentiometers wired to analog inputs **A0–A7**
- 4× LEDs (with ~330Ω current-limiting resistors) wired to digital pins **D0–D3**
- Stereo audio in / stereo audio out

## Controls

| Pot | Name | Function |
|-----|------|----------|
| P0 | **Intensity** | Macro from clean (0) to mayhem (100) |
| P1 | Drive character | Saturation amount |
| P2 | Ring mod frequency | 20 Hz – 2 kHz, exponential |
| P3 | Delay time | 30 ms – 1.2 s |
| P4 | Stereo spread | Ping-pong L/R offset ratio |
| P5 | Grain size | 10 ms – 400 ms |
| P6 | Reverb size | Small room → cathedral |
| P7 | Tilt EQ | Dark ↔ bright |

## LEDs

| LED | Function |
|-----|----------|
| D0 | Intensity meter (PWM) |
| D1 | Delay feedback level |
| D2 | Grain trigger flash |
| D3 | Limiter activity |

## Project settings (Bela IDE)

- Analog channels: **8 in**
- Digital channels: **enabled**
- Block size: **16**

## Files

- `render.cpp` — the effect implementation

## Tweaking

- `wideAmount` (default `1.6f`) — stereo width of wet bus
- `xL = dryL + xL * 0.7f` — wet bus level
- `kMaxGrains` — drop from 6 to 4 if CPU-tight

## License

Do whatever you want with it.
