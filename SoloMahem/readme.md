# Solo Mayhem

A stereo audio effect for the **Bela** platform, designed for live keyboard and synthesizer solos. One macro knob takes you from clean to complete chaos, while seven character knobs and four LEDs let you shape and monitor the sound as you play.

## Concept

Solo Mayhem is built around a single **Intensity** macro (P0) that walks through a curated path of effects: drive → ring modulation → granular freezer → stereo ping-pong delay → stereo reverb. Each block fades in at a different point along the macro, so the sweep feels like a journey rather than a single wet/dry blend.

The dry signal is preserved at unity gain throughout the entire chain — your keyboard always sits exactly where it is in the mix, and the wet bus is added in parallel on top.

## Hardware

- **Bela** or **Bela Mini** with cape
- 8× 10kΩ potentiometers wired to analog inputs **A0–A7**
- 4× LEDs (with appropriate current-limiting resistors, ~330Ω) wired to digital pins **D0–D3**
- Stereo audio in / stereo audio out

## Controls

| Pot | Name | Function |
|-----|------|----------|
| P0 | **Intensity** | Macro from clean (0) to mayhem (100). Fades in each effect along a curated curve. |
| P1 | Drive character | Amount of saturation applied to the input |
| P2 | Ring mod frequency | 20 Hz – 2 kHz, exponential. Tremolo at the bottom, metallic clang at the top. |
| P3 | Delay time | 30 ms – 1.2 s |
| P4 | Stereo spread | Ping-pong L/R offset ratio |
| P5 | Grain size | 10 ms – 400 ms |
| P6 | Reverb size | Small room → cathedral |
| P7 | Tilt EQ | Dark ↔ bright tone tilt on the wet bus |

## LEDs

| LED | Function |
|-----|----------|
| D0 | Intensity meter (PWM, perceptually compensated) |
| D1 | Delay feedback level — warns of runaway |
| D2 | Grain trigger flash |
| D3 | Limiter activity |

## Intensity zones (P0)

| P0 range | What happens |
|----------|--------------|
| 0–15 | Clean. Subtle saturation creeps in at the top edge. |
| 15–35 | Drive grows; short delay and small reverb appear. |
| 35–55 | Delay opens up, feedback rises; reverb tail lengthens. |
| 55–75 | Ring mod and granular freezer engage. |
| 75–90 | Granular density and pitch jitter ramp up; reverb approaches cathedral. |
| 90–100 | Feedback near self-oscillation, grains pitch-shift wildly. Mayhem. |

## Signal flow
