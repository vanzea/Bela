# Bela Talking Computer

A generative talking computer voice for the [Bela](https://bela.io) platform, using TMS5220 LPC speech synthesis — the same chip that powered the Texas Instruments Speak & Spell (1978). Eight analog inputs act as continuous controllers to shape the voice and the words it speaks in real time.

---

## How it works

The synthesis engine is a direct C++ port of the [Talkie Arduino library](https://github.com/ArminJo/Talkie) by Peter Knight and Armin Joachimsmeyer, stripped of all Arduino/AVR dependencies. It emulates the TMS5220 voice synthesis processor:

- Speech data is stored as compact LPC (Linear Predictive Coding) bitstreams, exactly as they were encoded in the original Texas Instruments ROM chips
- A 10-pole all-pole lattice filter is excited every 8kHz sample with either a pitch-synchronous glottal chirp (voiced sounds) or white noise (unvoiced sounds)
- Frames of synthesis parameters are loaded every 25ms (200 samples at 8kHz)

The voice data comes from the **Vocab_US_TI99** vocabulary — the actual speech ROM data from the TI-99/4A home computer's speech synthesiser module, extracted and documented by the MAME project and Talkie library team.

A random sentence generator assembles words into phrases at runtime: articles, nouns, verbs, and adjectives are picked randomly and combined into structures ranging from single words to five-word sentences.

---

## Project files

| File | Description |
|------|-------------|
| `render.cpp` | Complete synthesis engine and Bela audio render loop |
| `Vocab_US_TI99.h` | TI-99/4A LPC speech data (~55 words, PROGMEM-free) |

No external libraries, no packages, no internet connection required on Bela.

---

## Setup

1. Create a new project in the Bela IDE
2. Upload both `render.cpp` and `Vocab_US_TI99.h` into the project
3. Hit **Run**

No extra build flags or linker options are needed.

---

## Analog input mapping

Connect 10kΩ potentiometers or 0–3.3V CV signals to Bela's analog inputs:

| Input | Parameter | Range |
|-------|-----------|-------|
| A0 | Speech rate | 0.4× (slow) to 2.0× (fast) |
| A1 | Pitch | 0.5× (lower) to 2.0× (higher) |
| A2 | Word gap | 0 to 300ms silence between words |
| A3 | Ring modulator depth | 0 = bypass, full = heavy metallic effect |
| A4 | Sentence complexity | Single words ↔ five-word phrases (alternating) |
| A5 | Glitch / stutter | Probability of rewinding the data pointer |
| A6 | Low-pass filter | Muffled telephone tone ↔ full bandwidth |
| A7 | Output gain | |

---

## Vocabulary

The word list is drawn from the TI-99/4A speech ROM. The sentence generator picks from four categories:

**Articles:** the, a

**Nouns:** space, time, power, water, land, number, point, speed, mode, level, score, game, star, world, life, sector, signal, data, memory, words

**Verbs:** is, are, will, go, stop, move, wait, run, check, enter, load, read, do, come, see, know, need, make, find, learn, help, play, call

**Adjectives:** good, bad, high, low, fast, slow, full, old, next, last, long

---

## Sentence structures

Sentence complexity is controlled by A4. Below centre it alternates between single words and short phrases; above centre it generates longer constructions:

| Complexity | Structure example |
|------------|-------------------|
| Low | *"data"* / *"move"* |
| Mid-low | *"time will"* |
| Mid | *"the game stop"* |
| Mid-high | *"the fast signal move"* |
| High | *"the old memory load the data"* |

---

## Effects

**Ring modulator (A3):** Multiplies the speech signal by a sine wave (30–200Hz), adding a metallic robotic character on top of the already synthetic voice.

**Low-pass filter (A6):** A one-pole filter sweeping from 300Hz (telephone) to 8kHz (full bandwidth). At low values it produces a muffled, distant sound.

**Glitch (A5):** Randomly rewinds the LPC data pointer, causing the synthesiser to repeat short fragments of speech — a stutter or freeze effect.

**Pitch (A1):** Adjusts the playback rate of the 8kHz TMS output relative to Bela's sample rate, shifting pitch without changing the LPC filter parameters. This produces the characteristic chipmunk/slow-motion pitch shift.

---

## Credits

- **TMS5220 / Speak & Spell** — Texas Instruments, 1978
- **Talkie library** — Peter Knight (original), Armin Joachimsmeyer (ArminJo fork)
- **TI-99/4A speech ROM data** — extracted and documented by the MAME project
- **Bela platform** — Augmented Instruments Laboratory, Queen Mary University of London

Speech synthesis code ported from Talkie, licensed under GNU GPL v3.
