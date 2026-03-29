/*
 *  GENERATIVE SOUNDSCAPE ENGINE — Bela
 *
 *  Knobs 0-7: Density, Reverb, Delay, DelayTime,
 *             Drive, Filter, Texture, Volume
 *  LEDs 0-3:  Activity, Trigger, Level, Heartbeat
 */

#include <Bela.h>
#include <libraries/sndfile/sndfile.h>
#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <dirent.h>
#include "Effects.h"

/* ── Config ─────────────────────────────── */
#define MAX_VOICES   10
#define MAX_SAMPLES  112
#define ENV_ATK      0.5f
#define ENV_REL      0.5f
#define REL_EARLY    0.6f
#define MAX_SMP_FRAMES (44100 * 60)  /* cap at 60 seconds per sample */

/* ── Sample bank (stored as 16-bit to halve RAM) ── */
static short* gSmpData[MAX_SAMPLES];
static int    gSmpLen[MAX_SAMPLES];
static float  gSmpEnergy[MAX_SAMPLES];
static float  gSmpRate[MAX_SAMPLES];  /* playback ratio: originalSR / belaSR */
static char   gSmpName[MAX_SAMPLES][64]; /* short filename for console */
static int    gSmpOrder[MAX_SAMPLES]; /* sorted by energy */
static int    gSmpCount = 0;

/* ── Voice ──────────────────────────────── */
typedef struct {
    int   active;
    int   smpIdx;
    float pos, pitch, level, pan;
    int   released;
    Env   env;
} Voice;

static Voice gV[MAX_VOICES];

/* ── Effects ────────────────────────────── */
static Reverb gRevL, gRevR;
static Delay  gDlyL, gDlyR;
static Drive  gOvdL, gOvdR;
static SVF    gFltL, gFltR;

/* ── State ──────────────────────────────── */
static float gKnob[8];
static int   gClock = 0, gNextTrig = 0;
static float gTrigLed = 0, gHbPhase = 0;
static unsigned int gSeed = 54321;

/* ── xorshift RNG ───────────────────────── */
static float rndF(float lo, float hi) {
    gSeed ^= gSeed << 13;
    gSeed ^= gSeed >> 17;
    gSeed ^= gSeed << 5;
    float t = (float)(gSeed & 0xFFFF) / 65535.0f;
    return lo + t * (hi - lo);
}

/* ── Load one wav via libsndfile ─────────── */
/* Reads in small chunks to avoid large temporary buffers */
#define LOAD_CHUNK 4096

static int loadOneWav(const char* path, float belaSR) {
    if (gSmpCount >= MAX_SAMPLES) return 0;

    SF_INFO info;
    memset(&info, 0, sizeof(info));
    SNDFILE* sf = sf_open(path, SFM_READ, &info);
    if (!sf) return 0;

    int frames = (int)info.frames;
    int ch = info.channels;
    if (frames <= 0 || ch <= 0) { sf_close(sf); return 0; }

    /* Cap length to save RAM */
    if (frames > MAX_SMP_FRAMES) frames = MAX_SMP_FRAMES;

    /* Allocate 16-bit mono output */
    short* mono = (short*)malloc(frames * sizeof(short));
    if (!mono) { sf_close(sf); return 0; }

    /* Read in small chunks, mix to mono, convert to int16 */
    float chunk[LOAD_CHUNK * 2]; /* supports up to stereo */
    int written = 0;
    double rmsSum = 0;

    while (written < frames) {
        int toRead = frames - written;
        if (toRead > LOAD_CHUNK) toRead = LOAD_CHUNK;
        /* For >2 channels, use a heap buffer */
        float* buf = chunk;
        float* heapBuf = NULL;
        if (ch > 2) {
            heapBuf = (float*)malloc(toRead * ch * sizeof(float));
            if (!heapBuf) break;
            buf = heapBuf;
        }

        int got = (int)sf_readf_float(sf, buf, toRead);
        if (got <= 0) { if (heapBuf) free(heapBuf); break; }

        int i, c;
        for (i = 0; i < got; i++) {
            float s = 0;
            for (c = 0; c < ch; c++)
                s += buf[i * ch + c];
            s /= (float)ch;
            rmsSum += (double)s * s;
            /* Convert to int16 */
            int v = (int)(s * 32767.0f);
            if (v > 32767) v = 32767;
            if (v < -32768) v = -32768;
            mono[written + i] = (short)v;
        }
        written += got;
        if (heapBuf) free(heapBuf);
    }
    sf_close(sf);

    if (written == 0) { free(mono); return 0; }

    float rms = sqrtf((float)(rmsSum / written));

    int idx = gSmpCount;
    gSmpData[idx]   = mono;
    gSmpLen[idx]    = written;
    gSmpEnergy[idx] = rms;
    gSmpRate[idx]   = (float)info.samplerate / belaSR;

    /* Extract just the filename from the path */
    const char* slash = strrchr(path, '/');
    const char* name = slash ? slash + 1 : path;
    strncpy(gSmpName[idx], name, 63);
    gSmpName[idx][63] = '\0';

    gSmpCount++;

    rt_printf("  [%d] %s  %d fr  %dHz  ratio=%.3f  rms=%.4f\n",
              idx, path, written, info.samplerate, gSmpRate[idx], rms);
    return 1;
}

/* ── Load all wavs from directory ────────── */
static void loadSamples(const char* dir, float belaSR) {
    DIR* d = opendir(dir);
    if (!d) { rt_printf("Cannot open %s\n", dir); return; }

    /* Collect names (simple fixed buffer) */
    char paths[MAX_SAMPLES][256];
    int n = 0;
    struct dirent* e;
    while ((e = readdir(d)) != NULL && n < MAX_SAMPLES) {
        int len = strlen(e->d_name);
        if (len < 5) continue;
        const char* ext = e->d_name + len - 4;
        if ((ext[0]=='.' || ext[0]=='.') &&
            (ext[1]=='w' || ext[1]=='W') &&
            (ext[2]=='a' || ext[2]=='A') &&
            (ext[3]=='v' || ext[3]=='V'))
        {
            snprintf(paths[n], 256, "%s/%s", dir, e->d_name);
            n++;
        }
    }
    closedir(d);

    /* Simple sort */
    int i, j;
    for (i = 0; i < n - 1; i++) {
        for (j = i + 1; j < n; j++) {
            if (strcmp(paths[j], paths[i]) < 0) {
                char tmp[256];
                memcpy(tmp, paths[i], 256);
                memcpy(paths[i], paths[j], 256);
                memcpy(paths[j], tmp, 256);
            }
        }
    }

    for (i = 0; i < n; i++)
        loadOneWav(paths[i], belaSR);

    /* Normalise energy to 0..1 */
    float maxE = 0;
    for (i = 0; i < gSmpCount; i++)
        if (gSmpEnergy[i] > maxE) maxE = gSmpEnergy[i];
    if (maxE > 0)
        for (i = 0; i < gSmpCount; i++)
            gSmpEnergy[i] /= maxE;

    /* Build sorted index (quiet first) */
    for (i = 0; i < gSmpCount; i++) gSmpOrder[i] = i;
    for (i = 0; i < gSmpCount - 1; i++) {
        for (j = i + 1; j < gSmpCount; j++) {
            if (gSmpEnergy[gSmpOrder[j]] < gSmpEnergy[gSmpOrder[i]]) {
                int tmp = gSmpOrder[i];
                gSmpOrder[i] = gSmpOrder[j];
                gSmpOrder[j] = tmp;
            }
        }
    }

    /* Report memory usage */
    long totalBytes = 0;
    for (i = 0; i < gSmpCount; i++)
        totalBytes += gSmpLen[i] * (long)sizeof(short);
    rt_printf("Loaded %d samples. Total RAM: %.1f MB (16-bit)\n",
              gSmpCount, totalBytes / (1024.0f * 1024.0f));
}

/* ── Energy-biased selection ─────────────── */
static int selectSample(float texture) {
    if (gSmpCount <= 1) return 0;
    float target = texture * (gSmpCount - 1);
    float spread = gSmpCount * 0.2f;
    if (spread < 1.0f) spread = 1.0f;
    float r = (rndF(0,1) + rndF(0,1) + rndF(0,1)) / 3.0f;
    int pos = (int)(target + (r - 0.5f) * 2.0f * spread + 0.5f);
    if (pos < 0) pos = 0;
    if (pos >= gSmpCount) pos = gSmpCount - 1;
    return gSmpOrder[pos];
}

/* ── Read sample with lerp (int16 → float) ── */
static inline float readSmp(int idx, float pos) {
    int i0 = (int)pos;
    int len = gSmpLen[idx];
    if (i0 < 0 || i0 >= len) return 0;
    float frac = pos - (float)i0;
    float s0 = (float)gSmpData[idx][i0] * (1.0f / 32767.0f);
    float s1 = (i0 + 1 < len) ? (float)gSmpData[idx][i0 + 1] * (1.0f / 32767.0f) : 0;
    return s0 + frac * (s1 - s0);
}

/* ── Helpers ─────────────────────────────── */
static int countActive(void) {
    int n = 0, i;
    for (i = 0; i < MAX_VOICES; i++)
        if (gV[i].active) n++;
    return n;
}

static void triggerVoice(float sr, float texture) {
    if (gSmpCount == 0) return;
    int slot = -1, i;
    for (i = 0; i < MAX_VOICES; i++)
        if (!gV[i].active) { slot = i; break; }
    if (slot < 0) return;

    Voice* v = &gV[slot];
    v->smpIdx   = selectSample(texture);
    v->pos      = 0;
    v->level    = rndF(0.15f, 0.65f);
    v->pan      = rndF(0.0f, 1.0f);
    float lo    = 0.7f + texture * 0.15f;
    v->pitch    = rndF(lo, 1.0f) * gSmpRate[v->smpIdx];
    v->released = 0;
    env_init(&v->env, sr, ENV_ATK, ENV_REL);
    env_trigger(&v->env);
    v->active = 1;

    rt_printf("► Voice %d: [%d] %s  pitch=%.2f  level=%.2f  pan=%.2f  dur=%.1fs\n",
              slot, v->smpIdx, gSmpName[v->smpIdx],
              v->pitch, v->level, v->pan,
              (float)gSmpLen[v->smpIdx] / (sr * v->pitch));
}

/* ═══════════════════════════════════════════ */
bool setup(BelaContext* ctx, void*) {
    rt_printf("\n=== SOUNDSCAPE ENGINE ===\n\n");
    gSeed = ctx->audioFrames * 7 + 31;

    int i;
    for (i = 0; i < 4; i++)
        pinMode(ctx, 0, i, OUTPUT);
    for (i = 0; i < MAX_VOICES; i++)
        gV[i].active = 0;
    for (i = 0; i < 8; i++)
        gKnob[i] = 0.5f;
    gKnob[5] = 1.0f; /* filter starts fully open */

    float sr = ctx->audioSampleRate;
    loadSamples("samples", sr);

    reverb_init(&gRevL, sr); reverb_init(&gRevR, sr);
    reverb_setDecay(&gRevL, 0.8f); reverb_setDecay(&gRevR, 0.82f);
    delay_init(&gDlyL, sr); delay_init(&gDlyR, sr);
    svf_init(&gFltL, sr); svf_init(&gFltR, sr);
    gOvdL.gain = 1.0f; gOvdR.gain = 1.0f;

    gNextTrig = (int)(rndF(0.2f, 1.0f) * sr);
    rt_printf("SR=%.0f  Ready.\n\n", sr);
    return true;
}

/* ═══════════════════════════════════════════ */
void render(BelaContext* ctx, void*) {
    float sr = ctx->audioSampleRate;
    int aDiv = (int)(sr / ctx->analogSampleRate + 0.5f);
    unsigned int n;

    for (n = 0; n < ctx->audioFrames; n++) {

        /* ── Knobs ──────────────────────────── */
        if (ctx->analogInChannels >= 8) {
            unsigned int af = n / aDiv;
            if (af < ctx->analogFrames) {
                int k;
                for (k = 0; k < 8; k++) {
                    float raw = analogRead(ctx, af, k);
                    gKnob[k] += 0.005f * (raw - gKnob[k]);
                }
            }
        }

        int   maxVc   = 1 + (int)(gKnob[0] * 9.0f);
        float revMix  = gKnob[1];
        float dlyMix  = gKnob[2];
        float dlyTime = 0.05f + gKnob[3] * 0.7f;
        float drv     = gKnob[4];
        float fCut    = 200.0f + gKnob[5] * 6800.0f;
        float tex     = gKnob[6];
        float master  = gKnob[7] * gKnob[7] * 4.0f; /* quadratic taper */

        delay_setTime(&gDlyL, dlyTime * 0.75f);
        delay_setTime(&gDlyR, dlyTime);
        delay_setFeedback(&gDlyL, 0.3f + dlyMix * 0.35f);
        delay_setFeedback(&gDlyR, 0.3f + dlyMix * 0.35f);
        delay_setMix(&gDlyL, dlyMix);
        delay_setMix(&gDlyR, dlyMix);
        reverb_setDecay(&gRevL, revMix);
        reverb_setDecay(&gRevR, revMix);
        drive_set(&gOvdL, drv);
        drive_set(&gOvdR, drv);
        svf_setParams(&gFltL, fCut, 0.707f);
        svf_setParams(&gFltR, fCut, 0.707f);

        /* ── Trigger ────────────────────────── */
        gClock++;
        if (gClock >= gNextTrig) {
            if (countActive() < maxVc && gSmpCount > 0) {
                triggerVoice(sr, tex);
                gTrigLed = 1.0f;
                float df = 1.0f - gKnob[0];
                gNextTrig = gClock + (int)(rndF(0.1f+df*0.5f, 0.1f+df*3.9f) * sr);
            } else {
                gNextTrig = gClock + (int)(0.05f * sr);
            }
        }

        /* ── Mix voices ─────────────────────── */
        float mL = 0, mR = 0;
        int v, nActive = 0;
        for (v = 0; v < MAX_VOICES; v++) {
            if (!gV[v].active) continue;
            nActive++;

            int sLen = gSmpLen[gV[v].smpIdx];
            float left = ((float)sLen - gV[v].pos) / gV[v].pitch;
            if (!gV[v].released && left <= REL_EARLY * sr) {
                env_release(&gV[v].env);
                gV[v].released = 1;
            }

            float s = readSmp(gV[v].smpIdx, gV[v].pos);
            gV[v].pos += gV[v].pitch;

            if ((int)gV[v].pos >= sLen) {
                env_release(&gV[v].env);
                gV[v].released = 1;
            }

            float e = env_process(&gV[v].env);
            if (gV[v].env.state == ENV_IDLE ||
                ((int)gV[v].pos >= sLen && e < 0.001f)) {
                gV[v].active = 0;
                nActive--;
                continue;
            }

            float out = s * e * gV[v].level;
            float pa = gV[v].pan * 1.5707963f;
            mL += out * cosf(pa);
            mR += out * sinf(pa);
        }

        /* Scale down by sqrt of active voices to keep headroom */
        if (nActive > 1) {
            float scale = 1.0f / sqrtf((float)nActive);
            mL *= scale;
            mR *= scale;
        }

        /* ── FX ─────────────────────────────── */
        if (drv > 0.05f) {
            mL = drive_process(&gOvdL, mL);
            mR = drive_process(&gOvdR, mR);
        }
        if (gKnob[5] < 0.9f) {
            mL = svf_process(&gFltL, mL);
            mR = svf_process(&gFltR, mR);
        }
        if (dlyMix > 0.01f) {
            mL = delay_process(&gDlyL, mL);
            mR = delay_process(&gDlyR, mR);
        }
        if (revMix > 0.01f) {
            float rL = reverb_process(&gRevL, mL);
            float rR = reverb_process(&gRevR, mR);
            mL = mL * (1.0f - revMix) + rL * revMix;
            mR = mR * (1.0f - revMix) + rR * revMix;
        }
        mL = tanhf(mL * master);
        mR = tanhf(mR * master);

        audioWrite(ctx, n, 0, mL);
        audioWrite(ctx, n, 1, mR);

        /* ── LEDs ───────────────────────────── */
        if ((n & 63) == 0) {
            digitalWriteOnce(ctx, n, 0, countActive() > 2 ? 1 : 0);
            digitalWriteOnce(ctx, n, 1, gTrigLed > 0.5f ? 1 : 0);
            gTrigLed *= 0.92f;
            digitalWriteOnce(ctx, n, 2, (fabsf(mL)+fabsf(mR)) > 0.1f ? 1 : 0);
            gHbPhase += 64.0f / sr;
            if (gHbPhase > 2.0f) gHbPhase -= 2.0f;
            float hb = gHbPhase < 1.0f ? 0.5f*(1.0f+sinf(gHbPhase*6.2832f)) : 0;
            digitalWriteOnce(ctx, n, 3, hb > 0.5f ? 1 : 0);
        }
    }
}

/* ═══════════════════════════════════════════ */
void cleanup(BelaContext*, void*) {
    int i;
    for (i = 0; i < gSmpCount; i++)
        free(gSmpData[i]);
    rt_printf("Stopped.\n");
}
