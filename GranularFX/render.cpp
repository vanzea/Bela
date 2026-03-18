/*
 * Granulita Bela — Granular Synthesizer
 * Inspired by the Granulita Versio eurorack module
 *
 * 8 Analog Inputs (continuous controllers):
 *   A0 — Grain Position    (0.0–1.0 through buffer)
 *   A1 — Grain Size        (5ms–500ms)
 *   A2 — Density           (1–64 grains/sec)
 *   A3 — Pitch Transpose   (-24 to +24 semitones)
 *   A4 — Spray (position randomness)
 *   A5 — Pan Spread        (stereo field width)
 *   A6 — Grain Envelope    (attack/decay shape: 0=triangular, 1=hann)
 *   A7 — Dry/Wet Mix
 *
 * Audio:
 *   Input  0/1 — Stereo input (recorded into circular buffer)
 *   Output 0/1 — Stereo granular output
 *
 * LEDs on digital pins D0–D3:
 *   D0 — Grain activity    pulse on every grain spawn
 *   D1 — Density indicator brightness tracks grain count (PWM via rapid toggle)
 *   D2 — Pitch indicator   on when transpose > centre (above unity pitch)
 *   D3 — Clip/overload     lights when output is near saturation
 *
 * Build:
 *   Copy this file and project.json into a Bela project folder.
 *   The project uses no external dependencies beyond the Bela API.
 */

#include <Bela.h>
#include <cmath>
#include <cstring>
#include <random>
#include <array>
#include <vector>

// ─── Constants ──────────────────────────────────────────────────────────────

static constexpr int   kBufferSeconds  = 4;          // circular record buffer length
static constexpr int   kMaxGrains      = 64;
static constexpr int   kAnalogInputs   = 8;
static constexpr float kMinGrainMs     = 5.0f;
static constexpr float kMaxGrainMs     = 500.0f;
static constexpr float kMinDensity     = 1.0f;       // grains / second
static constexpr float kMaxDensity     = 64.0f;
static constexpr float kMaxTranspose   = 24.0f;      // semitones
static constexpr float kSmoothCoeff    = 0.005f;     // one-pole LP for CV smoothing

// ─── LED pins ───────────────────────────────────────────────────────────────
// Bela digital I/O is set per-frame with digitalWrite() / digitalRead()

static constexpr int kLedSpawn    = 0;   // D0 — pulse on grain spawn
static constexpr int kLedDensity  = 1;   // D1 — PWM brightness ∝ active grain count
static constexpr int kLedPitch    = 2;   // D2 — on when pitched above unity
static constexpr int kLedClip     = 3;   // D3 — on when output near saturation

// LED state
static int   gLedSpawnPulse  = 0;     // sample countdown for spawn flash (0 = off)
static float gLedDensityPWM  = 0.0f; // 0..1 duty cycle for density LED
static int   gLedPWMCounter  = 0;     // running counter for software PWM
static int   gActiveGrains   = 0;     // grain count, updated once per block

static constexpr int  kSpawnPulseLen  = 1024;  // ~23 ms at 44100 Hz
static constexpr int  kPWMPeriod      = 256;   // PWM period in samples
// Clip LED checks the raw accumulated sum BEFORE the tanhf(x*0.5) scaling.
// With many overlapping grains the sum easily exceeds 2.0; threshold at 1.5.
static constexpr float kClipThreshold = 1.5f;

// ─── Grain ──────────────────────────────────────────────────────────────────

struct Grain {
    bool  active      = false;
    float readPos     = 0.0f;   // fractional sample position in buffer
    float speed       = 1.0f;   // playback rate (pitch)
    int   lengthSamps = 0;      // grain duration in samples
    int   elapsed     = 0;      // samples played so far
    float panL        = 1.0f;
    float panR        = 1.0f;
    float envShape    = 0.5f;   // 0=triangle 1=Hann (per-grain at spawn)
};

// ─── State ──────────────────────────────────────────────────────────────────

static float* gBuffer    = nullptr;   // interleaved stereo circular buffer
static int    gBufFrames = 0;
static int    gWriteHead = 0;

static std::array<Grain, kMaxGrains> gGrains;
static float gSpawnTimer = 0.0f;      // countdown to next grain spawn (samples)

static float gSmoothCV[kAnalogInputs] = {};

static std::mt19937 gRng(42);
static std::uniform_real_distribution<float> gUni01(0.0f, 1.0f);

static float gSampleRate = 44100.0f;

// ─── Helpers ────────────────────────────────────────────────────────────────

static inline float lerp(float a, float b, float t) { return a + t * (b - a); }

// Hermite interpolation on stereo interleaved buffer
static inline void readBuffer(float pos, float& outL, float& outR) {
    int i0 = static_cast<int>(pos);
    float frac = pos - i0;

    auto at = [&](int idx, int ch) -> float {
        int n = ((idx % gBufFrames) + gBufFrames) % gBufFrames;
        return gBuffer[n * 2 + ch];
    };

    // Cubic Hermite
    auto hermite = [&](int ch) -> float {
        float p0 = at(i0 - 1, ch);
        float p1 = at(i0,     ch);
        float p2 = at(i0 + 1, ch);
        float p3 = at(i0 + 2, ch);
        float a  = -0.5f*p0 + 1.5f*p1 - 1.5f*p2 + 0.5f*p3;
        float b  =       p0 - 2.5f*p1 + 2.0f*p2 - 0.5f*p3;
        float c  = -0.5f*p0            + 0.5f*p2;
        float d  =            p1;
        return a*frac*frac*frac + b*frac*frac + c*frac + d;
    };

    outL = hermite(0);
    outR = hermite(1);
}

// Grain amplitude envelope
static inline float grainEnvelope(int elapsed, int length, float shape) {
    if (length <= 0) return 0.0f;
    float t = static_cast<float>(elapsed) / static_cast<float>(length);
    // Triangle
    float tri = (t < 0.5f) ? (t * 2.0f) : (2.0f - t * 2.0f);
    // Hann
    float hann = 0.5f * (1.0f - cosf(float(M_PI) * 2.0f * t));
    return lerp(tri, hann, shape);
}

// Spawn a new grain given current CV values
static void spawnGrain(float position, float size, float spray,
                       float transpose, float panSpread, float envShape)
{
    // Find a free grain slot
    for (auto& g : gGrains) {
        if (g.active) continue;

        float jitter = (gUni01(gRng) - 0.5f) * 2.0f * spray;
        float startNorm = position + jitter;
        // Wrap to [0,1)
        startNorm -= floorf(startNorm);

        g.readPos     = startNorm * static_cast<float>(gBufFrames - 1);
        g.lengthSamps = static_cast<int>(size * gSampleRate);
        if (g.lengthSamps < 1) g.lengthSamps = 1;
        g.elapsed     = 0;
        g.envShape    = envShape;

        // Pitch shift: speed = 2^(semitones/12)
        float semitones = lerp(-kMaxTranspose, kMaxTranspose, transpose);
        g.speed = powf(2.0f, semitones / 12.0f);

        // Panning
        float pan = (gUni01(gRng) - 0.5f) * 2.0f * panSpread; // -1..+1
        pan = pan * float(M_PI) * 0.25f;                        // ±45°
        g.panL = cosf(float(M_PI_4) + pan);
        g.panR = sinf(float(M_PI_4) + pan);

        g.active = true;
        break;
    }
}

// ─── Bela Callbacks ─────────────────────────────────────────────────────────

bool setup(BelaContext* ctx, void* /*userData*/) {
    gSampleRate = ctx->audioSampleRate;
    gBufFrames  = static_cast<int>(gSampleRate * kBufferSeconds);
    gBuffer     = new float[gBufFrames * 2]();   // zero-initialised stereo

    // Initialise smooth CV array
    for (auto& v : gSmoothCV) v = 0.5f;

    // Configure LED pins as digital outputs
    pinMode(ctx, 0, kLedSpawn,   OUTPUT);
    pinMode(ctx, 0, kLedDensity, OUTPUT);
    pinMode(ctx, 0, kLedPitch,   OUTPUT);
    pinMode(ctx, 0, kLedClip,    OUTPUT);

    // Ensure Bela reads all 8 analog channels
    // (Bela Mini / Bela Cape have 8 analog inputs by default)
    return true;
}

void render(BelaContext* ctx, void* /*userData*/) {
    // ── Read & smooth CV ──
    // Analog inputs are sampled at half the audio rate; we read once per block
    float rawCV[kAnalogInputs] = {};
    for (int ch = 0; ch < kAnalogInputs; ++ch) {
        if (ch < static_cast<int>(ctx->analogInChannels)) {
            rawCV[ch] = analogRead(ctx, 0, ch); // 0..1 on Bela
        }
    }
    for (int ch = 0; ch < kAnalogInputs; ++ch) {
        gSmoothCV[ch] += kSmoothCoeff * (rawCV[ch] - gSmoothCV[ch]);
    }

    // ── Map CV to parameters ──
    float position  = gSmoothCV[0];                                    // 0..1
    float grainMs   = lerp(kMinGrainMs, kMaxGrainMs, gSmoothCV[1]);   // ms
    float density   = lerp(kMinDensity, kMaxDensity, gSmoothCV[2]);   // grains/s
    float transpose = gSmoothCV[3];                                     // 0..1 → ±24 st
    float spray     = gSmoothCV[4];                                     // 0..1
    float panSpread = gSmoothCV[5];                                     // 0..1
    float envShape  = gSmoothCV[6];                                     // 0=tri 1=hann
    float dryWet    = gSmoothCV[7];                                     // 0..1

    float grainSizeSec = grainMs * 0.001f;
    float samplesPerGrain = (density > 0.0f) ? (gSampleRate / density) : gSampleRate;

    // ── Count active grains once per block (used by density LED) ──
    {
        int count = 0;
        for (const auto& g : gGrains) count += g.active ? 1 : 0;
        gActiveGrains = count;
    }

    // ── Per-sample loop ──
    for (unsigned int n = 0; n < ctx->audioFrames; ++n) {

        // Record input into circular buffer
        float inL = audioRead(ctx, n, 0);
        float inR = (ctx->audioInChannels > 1) ? audioRead(ctx, n, 1) : inL;
        gBuffer[gWriteHead * 2 + 0] = inL;
        gBuffer[gWriteHead * 2 + 1] = inR;
        gWriteHead = (gWriteHead + 1) % gBufFrames;

        // Spawn new grains
        gSpawnTimer -= 1.0f;
        if (gSpawnTimer <= 0.0f) {
            spawnGrain(position, grainSizeSec, spray, transpose, panSpread, envShape);
            gSpawnTimer += samplesPerGrain;
            gLedSpawnPulse = kSpawnPulseLen;   // trigger spawn flash
        }

        // Accumulate grain outputs
        float outL = 0.0f, outR = 0.0f;

        for (auto& g : gGrains) {
            if (!g.active) continue;

            float gL, gR;
            readBuffer(g.readPos, gL, gR);

            float env = grainEnvelope(g.elapsed, g.lengthSamps, g.envShape);
            outL += gL * env * g.panL;
            outR += gR * env * g.panR;

            g.readPos += g.speed;
            // Wrap read position within buffer
            if (g.readPos >= static_cast<float>(gBufFrames))
                g.readPos -= static_cast<float>(gBufFrames);
            if (g.readPos < 0.0f)
                g.readPos += static_cast<float>(gBufFrames);

            g.elapsed++;
            if (g.elapsed >= g.lengthSamps)
                g.active = false;
        }

        // ── Clip check happens BEFORE tanh scaling ──
        bool clipping = (fabsf(outL) >= kClipThreshold) ||
                        (fabsf(outR) >= kClipThreshold);

        // Soft-clip with tanh to prevent clipping artefacts
        outL = tanhf(outL * 0.5f);
        outR = tanhf(outR * 0.5f);

        // Dry/wet mix
        float mixL = lerp(inL, outL, dryWet);
        float mixR = lerp(inR, outR, dryWet);

        audioWrite(ctx, n, 0, mixL);
        if (ctx->audioOutChannels > 1)
            audioWrite(ctx, n, 1, mixR);

        // ── LED updates ──

        // D0 — spawn pulse: high for kSpawnPulseLen samples after each grain spawn
        digitalWrite(ctx, n, kLedSpawn, (gLedSpawnPulse > 0) ? HIGH : LOW);
        if (gLedSpawnPulse > 0) --gLedSpawnPulse;

        // D1 — density PWM using grain count cached once per block
        {
            float targetPWM = static_cast<float>(gActiveGrains) /
                              static_cast<float>(kMaxGrains);
            // Smooth the duty cycle to avoid abrupt jumps
            gLedDensityPWM += 0.01f * (targetPWM - gLedDensityPWM);

            gLedPWMCounter = (gLedPWMCounter + 1) % kPWMPeriod;
            // Avoid always-on (threshold==period) or always-off (threshold==0)
            int threshold = static_cast<int>(gLedDensityPWM * (kPWMPeriod - 1));
            digitalWrite(ctx, n, kLedDensity, (gLedPWMCounter < threshold) ? HIGH : LOW);
        }

        // D2 — pitch LED: on when transpose CV is above centre (pitching up)
        {
            bool pitchedUp = gSmoothCV[3] > 0.52f;   // small deadband around unity
            digitalWrite(ctx, n, kLedPitch, pitchedUp ? HIGH : LOW);
        }

        // D3 — clip LED: pre-tanh sum exceeded threshold this sample
        digitalWrite(ctx, n, kLedClip, clipping ? HIGH : LOW);
    }   // end per-sample loop
}

void cleanup(BelaContext* /*ctx*/, void* /*userData*/) {
    delete[] gBuffer;
    gBuffer = nullptr;
}

