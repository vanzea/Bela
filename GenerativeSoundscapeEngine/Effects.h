#ifndef EFFECTS_H
#define EFFECTS_H

#ifdef __cplusplus
extern "C" {
#endif

/* ── clamp ──────────────────────────────── */
static inline float clampf(float v, float lo, float hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

/* ── One-pole LP ────────────────────────── */
typedef struct {
    float sr, a, z;
} OnePole;

void onepole_init(OnePole* f, float sr);
void onepole_setCutoff(OnePole* f, float hz);
float onepole_process(OnePole* f, float in);

/* ── State-variable filter ──────────────── */
typedef struct {
    float sr, f, q, lp, bp, hp;
} SVF;

void svf_init(SVF* f, float sr);
void svf_setParams(SVF* f, float cutoff, float res);
float svf_process(SVF* f, float in);

/* ── Schroeder reverb ───────────────────── */
#define REV_MAX 4096
typedef struct {
    float decay;
    float* combBuf[4];
    int   combLen[4], combIdx[4];
    float* apBuf[2];
    int   apLen[2], apIdx[2];
} Reverb;

void reverb_init(Reverb* r, float sr);
void reverb_setDecay(Reverb* r, float d);
float reverb_process(Reverb* r, float in);

/* ── Tape delay ─────────────────────────── */
#define DLY_MAX 44100
typedef struct {
    float sr;
    float* buf;
    int   writeIdx, delaySamp, len;
    float feedback, mix;
    OnePole lpf;
} Delay;

void delay_init(Delay* d, float sr);
void delay_setTime(Delay* d, float sec);
void delay_setFeedback(Delay* d, float fb);
void delay_setMix(Delay* d, float m);
float delay_process(Delay* d, float in);

/* ── Overdrive ──────────────────────────── */
typedef struct { float gain; } Drive;

static inline void drive_set(Drive* d, float amt) {
    d->gain = 1.0f + amt * 5.0f;
}
float drive_process(Drive* d, float in);

/* ── AR Envelope ────────────────────────── */
enum EnvState { ENV_IDLE = 0, ENV_ATTACK, ENV_SUSTAIN, ENV_RELEASE };

typedef struct {
    float sr, value, attackInc, relCoeff;
    int   state;
} Env;

void env_init(Env* e, float sr, float atk, float rel);
void env_trigger(Env* e);
void env_release(Env* e);
float env_process(Env* e);

#ifdef __cplusplus
}
#endif

#endif
