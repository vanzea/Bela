# Generative Soundscape Engine — Bela

A sample-based generative ambient engine for the Bela platform. It randomly triggers audio samples with varying pitch, pan, and level, shaped by an attack/release envelope and routed through a chain of effects controlled by 8 potentiometers.

## Hardware Wiring

### Potentiometers (10kΩ linear, wiper → analog in)

| Analog In | Function       | Range                        |
|-----------|----------------|------------------------------|
| 0         | Density        | 1–10 simultaneous voices     |
| 1         | Reverb Mix     | Dry → full wet               |
| 2         | Delay Mix      | Dry → full wet               |
| 3         | Delay Time     | 50 ms → 1.5 s               |
| 4         | Overdrive      | Clean → heavy saturation     |
| 5         | Filter Cutoff  | 200 Hz → 18 kHz (low-pass)  |
| 6         | Texture        | Calm (quiet samples) ↔ intense (loud) — also shifts pitch range |
| 7         | Master Volume  | Silent → full                |

### LEDs (with appropriate resistor, e.g. 220Ω)

| Digital Out | Function       | Behaviour                     |
|-------------|----------------|-------------------------------|
| 0           | Voice Activity | On when several voices active |
| 1           | Trigger Flash  | Blinks when a voice starts    |
| 2           | Level Meter    | On when signal is present     |
| 3           | Heartbeat      | Slow pulse (system alive)     |

## Samples

Create a `samples/` folder inside the project directory and place `.wav` files in it (mono or stereo, any sample rate — Bela will handle conversion). The engine loads all `.wav` files at startup and picks from them randomly during playback.

```
bela-soundscape/
├── render.cpp
├── Effects.h
├── settings.json
└── samples/
    ├── drone_pad.wav
    ├── field_recording.wav
    ├── texture_01.wav
    └── ...
```

## How It Works

1. **Sample analysis** — At startup, every `.wav` is scanned for RMS energy and ranked from quietest to loudest. This energy profile drives knob 6 (Texture).
2. **Voice triggering** — A scheduler fires at random intervals (density knob controls frequency). Each trigger picks a sample biased by the Texture knob — left favours quiet/calm samples, right favours loud/intense ones. Up to 10 voices.
3. **Per-voice randomisation** — Each voice gets a random level (0.15–0.85), pan (full L–R), and pitch ratio (original down to 2 octaves below — calmer textures pitch down further).
4. **Envelope** — Half-second linear attack, exponential release triggered automatically before the sample ends.
4. **Effects chain** — Overdrive → State-variable LP filter → Tape delay (with filtered feedback + saturation) → Schroeder reverb. All controllable in real time.
5. **Output** — Soft-clipped stereo via `tanh()`.

## Building & Running

Upload the entire `bela-soundscape/` folder to your Bela (via the IDE or `scp`), then build and run from the Bela IDE or command line:

```bash
# From the Bela board:
make -C /root/Bela PROJECT=bela-soundscape run
```

Or simply open the project folder in the Bela IDE and press **Run**.
