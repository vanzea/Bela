// render.cpp — Solo Mayhem stereo effect for Bela
// 8 pots (P0 = Intensity macro), 4 LEDs on D0-D3 (software PWM)
// Dry signal preserved at unity; wet effects added in parallel with wide stereo.

#include <Bela.h>
#include <cmath>
#include <vector>
#include <cstdlib>

static const float TWO_PI = 6.28318530718f;
static const int kPotPins[8] = {0,1,2,3,4,5,6,7};
static const int kLedPins[4] = {0,1,2,3};
static float pot[8] = {0};

static inline float smooth(float& y, float x, float a=0.005f){ y += a*(x-y); return y; }
static inline float clampf(float x, float lo, float hi){ return x<lo?lo:(x>hi?hi:x); }
static inline float mix(float a, float b, float t){ return a + (b-a)*t; }
static inline float softclip(float x){ return tanhf(x); }

static const int kDelayMax = 96000;
static std::vector<float> dlL, dlR;
static int dWrite = 0;
static float rmPhaseL=0, rmPhaseR=0;

static const int kGrainBuf = 96000;
static std::vector<float> gBufL, gBufR;
static int gWrite = 0;
struct Grain { int pos; int len; int age; float pitch; float panL, panR; bool active; };
static const int kMaxGrains = 6;
static Grain grains[kMaxGrains];
static int grainTrigCounter = 0;
static bool grainBlink = false;

struct Comb { std::vector<float> buf; int idx=0; float fb=0.7f; float damp=0.2f; float store=0; };
struct Allp { std::vector<float> buf; int idx=0; };
static Comb combs[8];
static Allp allps[2];
static const int combLens[8] = {1557,1617,1491,1422,1277,1356,1188,1116};
static const int allpLens[2] = {556,441};

static inline float combProc(Comb& c, float in){
    float y = c.buf[c.idx];
    c.store = y*(1-c.damp) + c.store*c.damp;
    c.buf[c.idx] = in + c.store*c.fb;
    if(++c.idx >= (int)c.buf.size()) c.idx=0;
    return y;
}
static inline float allpProc(Allp& a, float in){
    float y = a.buf[a.idx];
    a.buf[a.idx] = in + y*0.5f;
    if(++a.idx >= (int)a.buf.size()) a.idx=0;
    return y - in*0.5f;
}

static float fbMeter=0, limMeter=0, intensityVis=0;
static float limGain = 1.f;
static unsigned int pwmCounter = 0;
static const unsigned int kPwmPeriod = 32;

static inline float zone(float p, float lo, float hi){
    return clampf((p-lo)/(hi-lo), 0.f, 1.f);
}

bool setup(BelaContext* ctx, void*){
    dlL.assign(kDelayMax,0); dlR.assign(kDelayMax,0);
    gBufL.assign(kGrainBuf,0); gBufR.assign(kGrainBuf,0);
    for(int i=0;i<8;i++) combs[i].buf.assign(combLens[i],0);
    for(int i=0;i<2;i++) allps[i].buf.assign(allpLens[i],0);
    for(int i=0;i<kMaxGrains;i++) grains[i].active=false;
    for(int i=0;i<4;i++) pinMode(ctx, 0, kLedPins[i], OUTPUT);
    return true;
}

void render(BelaContext* ctx, void*){
    if(ctx->analogInChannels >= 8 && ctx->analogFrames > 0){
        for(int i=0;i<8;i++)
            smooth(pot[i], analogRead(ctx,0,kPotPins[i]));
    }

    const float P0 = pot[0];
    const float drive   = 1.f + zone(P0,0.0f,0.9f)*8.f * (0.5f+pot[1]);
    const float driveComp = 1.f / sqrtf(drive);
    const float rmFreq  = 20.f * powf(100.f, pot[2]);
    const float rmWet   = zone(P0,0.55f,0.85f) * 0.7f;
    const float dlTime  = 0.03f + pot[3]*1.17f;
    const float spread  = pot[4];
    const float dlFb    = mix(0.f, 0.92f, zone(P0,0.15f,0.95f));
    const float dlWet   = zone(P0,0.15f,0.6f);
    const float grainSize = 0.01f + pot[5]*0.39f;
    const float grainWet  = zone(P0,0.55f,1.0f);
    const float revSize = 0.5f + pot[6]*0.49f;
    const float revWet  = zone(P0,0.2f,0.95f) * 0.45f;
    const float tilt    = pot[7]*2.f - 1.f;

    for(int i=0;i<8;i++) combs[i].fb = 0.7f + revSize*0.15f;

    int dSampL = (int)clampf(dlTime*ctx->audioSampleRate,1,kDelayMax-1);
    int dSampR = (int)clampf(dlTime*(1.f+spread*1.2f)*ctx->audioSampleRate,1,kDelayMax-1);
    int grainLenSamp = (int)(grainSize*ctx->audioSampleRate);
    int grainPeriod  = grainLenSamp/2;
    if(grainPeriod < 1) grainPeriod = 1;

    static float tiltState=0;
    const float tiltA = 0.02f + fabsf(tilt)*0.3f;

    for(unsigned int n=0; n<ctx->audioFrames; n++){
        float inL = audioRead(ctx,n,0);
        float inR = audioRead(ctx,n,1);
        float dryL = inL, dryR = inR;

        float xL = softclip(inL*drive) * driveComp * 1.4f;
        float xR = softclip(inR*drive) * driveComp * 1.4f;

        rmPhaseL += TWO_PI*rmFreq/ctx->audioSampleRate;
        rmPhaseR += TWO_PI*rmFreq*1.07f/ctx->audioSampleRate;
        if(rmPhaseL>TWO_PI) rmPhaseL-=TWO_PI;
        if(rmPhaseR>TWO_PI) rmPhaseR-=TWO_PI;
        xL = mix(xL, xL*sinf(rmPhaseL), rmWet);
        xR = mix(xR, xR*sinf(rmPhaseR), rmWet);

        gBufL[gWrite]=xL; gBufR[gWrite]=xR;
        if(++gWrite>=kGrainBuf) gWrite=0;

        if(grainWet>0.01f && ++grainTrigCounter>=grainPeriod){
            grainTrigCounter=0;
            for(int g=0;g<kMaxGrains;g++) if(!grains[g].active){
                grains[g].active=true;
                grains[g].age=0;
                grains[g].len=grainLenSamp;
                int off = rand()%kGrainBuf;
                grains[g].pos = (gWrite - off + kGrainBuf) % kGrainBuf;
                grains[g].pitch = 1.f + (((rand()%200)-100)/100.f)*zone(P0,0.75f,1.f);
                float p = (rand()%1000)/1000.f;
                grains[g].panL = sqrtf(1.f - p);
                grains[g].panR = sqrtf(p);
                grainBlink=true;
                break;
            }
        }
        float gL=0,gR=0;
        for(int g=0;g<kMaxGrains;g++) if(grains[g].active){
            float w = 0.5f*(1.f - cosf(TWO_PI*grains[g].age/(float)grains[g].len));
            int idx = grains[g].pos % kGrainBuf;
            float gs = (gBufL[idx]+gBufR[idx])*0.5f*w;
            gL += gs*grains[g].panL;
            gR += gs*grains[g].panR;
            grains[g].pos = (grains[g].pos + (int)grains[g].pitch + kGrainBuf) % kGrainBuf;
            if(++grains[g].age >= grains[g].len) grains[g].active=false;
        }
        xL = xL + gL * grainWet;
        xR = xR + gR * grainWet;

        int rL = (dWrite - dSampL + kDelayMax) % kDelayMax;
        int rR = (dWrite - dSampR + kDelayMax) % kDelayMax;
        float dL = dlL[rL], dR = dlR[rR];
        dlL[dWrite] = softclip(xL + dR*dlFb);
        dlR[dWrite] = softclip(xR + dL*dlFb);
        if(++dWrite>=kDelayMax) dWrite=0;
        xL = xL + dL * dlWet;
        xR = xR + dR * dlWet;

        float rsumL=0, rsumR=0;
        for(int i=0;i<4;i++) rsumL += combProc(combs[i], xL);
        for(int i=4;i<8;i++) rsumR += combProc(combs[i], xR);
        rsumL *= 0.25f; rsumR *= 0.25f;
        float rL_out = allpProc(allps[0], rsumL);
        float rR_out = allpProc(allps[1], rsumR);
        xL = xL + rL_out * revWet;
        xR = xR + rR_out * revWet;

        tiltState += tiltA*((xL+xR)*0.5f - tiltState);
        if(tilt<0){ xL = mix(xL, tiltState, -tilt); xR = mix(xR, tiltState, -tilt); }
        else      { xL = xL + (xL-tiltState)*tilt; xR = xR + (xR-tiltState)*tilt; }

        float wetM = (xL+xR)*0.5f;
        float wetS = (xL-xR)*0.5f;
        const float wideAmount = 1.6f;
        xL = wetM + wetS*wideAmount;
        xR = wetM - wetS*wideAmount;

        xL = dryL + xL * 0.7f;
        xR = dryR + xR * 0.7f;

        float peak = fmaxf(fabsf(xL), fabsf(xR));
        float target = (peak > 0.9f) ? (0.9f / peak) : 1.f;
        if(target < limGain) limGain = target;
        else                 limGain += 0.0005f*(1.f - limGain);
        float oL = xL * limGain;
        float oR = xR * limGain;
        limMeter += 0.01f*((limGain<0.95f?1.f:0.f) - limMeter);
        fbMeter  += 0.001f*(fabsf(dL)+fabsf(dR) - fbMeter);

        if(ctx->digitalFrames > 0){
            unsigned int phase = pwmCounter++ % kPwmPeriod;
            float led0 = sqrtf(clampf(intensityVis, 0, 1));
            float led1 = clampf(fbMeter*4.f, 0, 1);
            float led3 = clampf(limMeter*2.f, 0, 1);
            digitalWriteOnce(ctx, n, kLedPins[0], phase < (unsigned)(led0*kPwmPeriod) ? 1 : 0);
            digitalWriteOnce(ctx, n, kLedPins[1], phase < (unsigned)(led1*kPwmPeriod) ? 1 : 0);
            digitalWriteOnce(ctx, n, kLedPins[2], grainBlink ? 1 : 0);
            digitalWriteOnce(ctx, n, kLedPins[3], phase < (unsigned)(led3*kPwmPeriod) ? 1 : 0);
        }

        audioWrite(ctx,n,0,oL);
        audioWrite(ctx,n,1,oR);
    }

    intensityVis += 0.05f*(pot[0] - intensityVis);
    grainBlink = false;
}

void cleanup(BelaContext*, void*){}
