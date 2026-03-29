#include "Effects.h"
#include <math.h>
#include <string.h>
#include <stdlib.h>

/* ── OnePole ──────────────────────────────── */

void onepole_init(OnePole* f, float sr) {
    f->sr = sr; f->a = 1.0f; f->z = 0;
}
void onepole_setCutoff(OnePole* f, float hz) {
    float w = 2.0f * M_PI * hz / f->sr;
    f->a = w / (1.0f + w);
}
float onepole_process(OnePole* f, float in) {
    f->z += f->a * (in - f->z);
    return f->z;
}

/* ── SVF ──────────────────────────────────── */

void svf_init(SVF* f, float sr) {
    f->sr = sr; f->f = 0.1f; f->q = 0.5f;
    f->lp = f->bp = f->hp = 0;
}
void svf_setParams(SVF* f, float cutoff, float res) {
    float c = cutoff;
    /* Limit to sr/6 — Chamberlin SVF is unstable above ~sr/4 */
    float maxCut = f->sr * 0.16f;
    if (c > maxCut) c = maxCut;
    if (c < 20.0f) c = 20.0f;
    f->f = 2.0f * sinf(3.14159265f * c / f->sr);
    /* Clamp coefficient for absolute safety */
    if (f->f > 0.9f) f->f = 0.9f;
    float r = res;
    if (r < 0.5f) r = 0.5f;
    f->q = 1.0f / r;
}
float svf_process(SVF* f, float in) {
    f->hp = in - f->lp - f->q * f->bp;
    f->bp += f->f * f->hp;
    f->lp += f->f * f->bp;
    /* Kill NaN / denormals — if it blows up, reset */
    if (!(f->lp >= -100.0f && f->lp <= 100.0f)) {
        f->lp = f->bp = f->hp = 0;
    }
    return f->lp;
}

/* ── Reverb ───────────────────────────────── */

void reverb_init(Reverb* r, float sr) {
    float sc = sr / 44100.0f;
    r->decay = 0.85f;
    r->combLen[0] = (int)(1557*sc); if(r->combLen[0]>=REV_MAX) r->combLen[0]=REV_MAX-1;
    r->combLen[1] = (int)(1617*sc); if(r->combLen[1]>=REV_MAX) r->combLen[1]=REV_MAX-1;
    r->combLen[2] = (int)(1491*sc); if(r->combLen[2]>=REV_MAX) r->combLen[2]=REV_MAX-1;
    r->combLen[3] = (int)(1422*sc); if(r->combLen[3]>=REV_MAX) r->combLen[3]=REV_MAX-1;
    r->apLen[0]   = (int)(225*sc);  if(r->apLen[0]>=REV_MAX) r->apLen[0]=REV_MAX-1;
    r->apLen[1]   = (int)(556*sc);  if(r->apLen[1]>=REV_MAX) r->apLen[1]=REV_MAX-1;
    int i;
    for (i = 0; i < 4; i++) {
        r->combBuf[i] = (float*)calloc(REV_MAX, sizeof(float));
        r->combIdx[i] = 0;
    }
    for (i = 0; i < 2; i++) {
        r->apBuf[i] = (float*)calloc(REV_MAX, sizeof(float));
        r->apIdx[i] = 0;
    }
}

void reverb_setDecay(Reverb* r, float d) {
    r->decay = 0.7f + 0.28f * clampf(d, 0.0f, 1.0f);
}

float reverb_process(Reverb* r, float in) {
    float out = 0;
    int i;
    for (i = 0; i < 4; i++) {
        int idx = (r->combIdx[i] - r->combLen[i] + REV_MAX) % REV_MAX;
        float del = r->combBuf[i][idx];
        r->combBuf[i][r->combIdx[i]] = in + del * r->decay;
        r->combIdx[i] = (r->combIdx[i] + 1) % REV_MAX;
        out += del;
    }
    out *= 0.25f;
    for (i = 0; i < 2; i++) {
        int idx = (r->apIdx[i] - r->apLen[i] + REV_MAX) % REV_MAX;
        float del = r->apBuf[i][idx];
        float tmp = out + del * (-0.5f);
        r->apBuf[i][r->apIdx[i]] = tmp;
        r->apIdx[i] = (r->apIdx[i] + 1) % REV_MAX;
        out = del + tmp * 0.5f;
    }
    return out;
}

/* ── Delay ────────────────────────────────── */

void delay_init(Delay* d, float sr) {
    d->sr = sr;
    d->len = DLY_MAX;
    d->buf = (float*)calloc(DLY_MAX, sizeof(float));
    d->writeIdx = 0;
    d->delaySamp = DLY_MAX / 2;
    d->feedback = 0.4f;
    d->mix = 0.3f;
    onepole_init(&d->lpf, sr);
    onepole_setCutoff(&d->lpf, 3500.0f);
}

void delay_setTime(Delay* d, float sec) {
    int s = (int)(sec * d->sr);
    if (s < 1) s = 1;
    if (s >= d->len) s = d->len - 1;
    d->delaySamp = s;
}
void delay_setFeedback(Delay* d, float fb) { d->feedback = clampf(fb, 0, 0.92f); }
void delay_setMix(Delay* d, float m) { d->mix = clampf(m, 0, 1); }

float delay_process(Delay* d, float in) {
    int ri = (d->writeIdx - d->delaySamp + d->len) % d->len;
    float del = d->buf[ri];
    float flt = onepole_process(&d->lpf, del);
    float fb  = tanhf(flt * d->feedback);
    d->buf[d->writeIdx] = in + fb;
    d->writeIdx = (d->writeIdx + 1) % d->len;
    return in * (1.0f - d->mix) + del * d->mix;
}

/* ── Drive ────────────────────────────────── */

float drive_process(Drive* d, float in) {
    return tanhf(in * d->gain) * 0.7f;
}

/* ── Envelope ─────────────────────────────── */

void env_init(Env* e, float sr, float atk, float rel) {
    e->sr = sr;
    e->value = 0;
    e->state = ENV_IDLE;
    e->attackInc = 1.0f / (atk * sr);
    e->relCoeff = expf(-4.6f / (rel * sr));
}
void env_trigger(Env* e) { e->state = ENV_ATTACK; }
void env_release(Env* e) { if (e->state != ENV_IDLE) e->state = ENV_RELEASE; }

float env_process(Env* e) {
    switch (e->state) {
    case ENV_ATTACK:
        e->value += e->attackInc;
        if (e->value >= 1.0f) { e->value = 1.0f; e->state = ENV_SUSTAIN; }
        break;
    case ENV_SUSTAIN:
        break;
    case ENV_RELEASE:
        e->value *= e->relCoeff;
        if (e->value < 0.001f) { e->value = 0; e->state = ENV_IDLE; }
        break;
    default:
        e->value = 0;
        break;
    }
    return e->value;
}
