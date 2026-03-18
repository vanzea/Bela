
/*
 * deep_rumble.cpp
 * Deep Rumble Synthesizer for Bela  —  v4.2
 *
 * Sound engine:
 *   - 8 detuned sub-bass oscillators (sine/triangle), pitch 10–80 Hz
 *   - Dedicated sub-octave oscillator (pure sine, pitch/2)
 *   - Grain texture engine: bandpass-filtered noise bursts with exponential
 *     decay envelopes. Low knob = silence between rare heavy thuds.
 *     High knob = constant gravelly crunch. Mixed AFTER the main LP.
 *   - Filtered broadband noise (noise-dominant by default)
 *   - Cascaded 2× LP for dark low-end
 *   - Low-shelf boost ~80 Hz
 *   - Soft-clip + cubic waveshaper blend
 *   - Sine tremolo LFO
 *   - Haas stereo widener + Mid-Side width LFO
 *   - Schroeder reverb (4 combs + 2 all-pass), low-end only
 *
 * 8 Analog Inputs (0–3.3 V, normalised 0..1):
 *   Analog 0 → Master Pitch       10–80 Hz
 *   Analog 1 → Rumble Density     1–8 oscillator layers
 *   Analog 2 → Filter Cutoff      20–600 Hz
 *   Analog 3 → Grain Texture      0 = silent; low = sparse thuds; high = dense gravel
 *   Analog 4 → Noise Mix          biased 0.40–1.00
 *   Analog 5 → Tremolo Rate       0.1–12 Hz
 *   Analog 6 → Distortion         0–100% waveshaper drive
 *   Analog 7 → Sub-octave Blend   0–100%
 *
 * Fixed tweakables at top of file:
 *   STEREO_WIDTH / REVERB_MIX / REVERB_DAMP / REVERB_ROOM / BASS_SHELF_GAIN
 *
 * LED outputs (digital pins, all active HIGH):
 *   D0 — Tremolo pulse   : brightness follows the tremolo LFO in real time
 *   D1 — Grain flash     : strobes on every new grain trigger; density tracks
 *                          the Grain Texture knob (sparse ↔ rapid flicker)
 *   D2 — Output level VU : peak-hold envelope of the final stereo output;
 *                          bright when loud, dims with a slow decay tail
 *   D3 — Sub heartbeat   : pulses at the sub-octave frequency (pitch/2),
 *                          giving a slow visible throb that matches the deepest
 *                          bass note — visible below ~15 Hz as individual blinks
 */

#include <Bela.h>
#include <cmath>
#include <cstring>
#include <libraries/math_neon/math_neon.h>  // replace sinf_neon→sinf if unavailable

// ─── Tuneable constants ───────────────────────────────────────────────────────

static const float STEREO_WIDTH    = 0.72f;
static const float REVERB_MIX      = 0.20f;
static const float REVERB_DAMP     = 0.45f;
static const float REVERB_ROOM     = 0.84f;
static const float BASS_SHELF_GAIN = 6.0f;
static const float OSC_MIX_MAX     = 0.45f;

// ─── LED pin assignments ──────────────────────────────────────────────────────

static const int kLED_Tremolo  = 0;  // D0: tremolo LFO brightness
static const int kLED_Grain    = 1;  // D1: grain trigger flash
static const int kLED_VU       = 2;  // D2: output level peak-hold
static const int kLED_SubHeart = 3;  // D3: sub-octave heartbeat

// ─── Constants ────────────────────────────────────────────────────────────────

static const int   kNumOscillators = 8;
static const float kTwoPi          = 2.0f * (float)M_PI;

// ─── Grain constants ──────────────────────────────────────────────────────────

static const int kMaxGrains     = 16;
// Grain lengths: short for high-texture crunch, long for low-texture thuds
static const int kGrainMinSamps = 512;    // ~12 ms at 44100
static const int kGrainMaxSamps = 8820;   // 200 ms at 44100

// ─── Sample rate ──────────────────────────────────────────────────────────────

static float gSampleRate    = 44100.0f;
static float gInvSampleRate = 1.0f / 44100.0f;

// ─── Biquad ───────────────────────────────────────────────────────────────────

struct Biquad {
    float b0=1, b1=0, b2=0, a1=0, a2=0;
    float x1=0, x2=0, y1=0, y2=0;
    void clearState() { x1=x2=y1=y2=0.0f; }
};

static void setBiquadLP(Biquad& bq, float freq, float Q, float sr) {
    freq = fmaxf(fminf(freq, sr*0.49f), 5.0f);
    float w = kTwoPi*freq/sr, s=sinf(w), c=cosf(w), a=s/(2.0f*Q);
    float n = 1.0f/(1.0f+a);
    bq.b0=(1.0f-c)*0.5f*n; bq.b1=(1.0f-c)*n; bq.b2=bq.b0;
    bq.a1=-2.0f*c*n; bq.a2=(1.0f-a)*n;
}

static void setBiquadBP(Biquad& bq, float freq, float Q, float sr) {
    freq = fmaxf(fminf(freq, sr*0.49f), 20.0f);
    float w = kTwoPi*freq/sr, s=sinf(w), c=cosf(w), a=s/(2.0f*Q);
    float n = 1.0f/(1.0f+a);
    bq.b0=s*0.5f*n; bq.b1=0.0f; bq.b2=-s*0.5f*n;
    bq.a1=-2.0f*c*n; bq.a2=(1.0f-a)*n;
}

static void setBiquadLowShelf(Biquad& bq, float freq, float gainDB, float sr) {
    float A=powf(10.0f,gainDB/40.0f), w=kTwoPi*freq/sr;
    float s=sinf(w), c=cosf(w), al=s*0.5f*sqrtf(2.0f), sq=2.0f*sqrtf(A)*al;
    float a0=(A+1.0f)+(A-1.0f)*c+sq, n=1.0f/a0;
    bq.b0= A*((A+1.0f)-(A-1.0f)*c+sq)*n;
    bq.b1= 2.0f*A*((A-1.0f)-(A+1.0f)*c)*n;
    bq.b2= A*((A+1.0f)-(A-1.0f)*c-sq)*n;
    bq.a1=-2.0f*((A-1.0f)+(A+1.0f)*c)*n;
    bq.a2=((A+1.0f)+(A-1.0f)*c-sq)*n;
}

static inline float processBiquad(Biquad& bq, float x) {
    float y=bq.b0*x+bq.b1*bq.x1+bq.b2*bq.x2-bq.a1*bq.y1-bq.a2*bq.y2;
    bq.x2=bq.x1; bq.x1=x; bq.y2=bq.y1; bq.y1=y;
    return y;
}

// ─── Reverb ───────────────────────────────────────────────────────────────────

static const int kNumCombs=4, kNumAP=2;
static const int kCombLenL[]={1557,1617,1491,1422};
static const int kCombLenR[]={1580,1640,1514,1445};
static const int kAPLenL[]  ={225,556};
static const int kAPLenR[]  ={241,579};
static const int kMaxCombLen=2048, kMaxApLen=1024;

struct CombFilter {
    float buf[kMaxCombLen]; int len=1557,idx=0; float filt=0;
    CombFilter(){memset(buf,0,sizeof(buf));}
    float process(float x){
        float out=buf[idx];
        filt=out*(1-REVERB_DAMP)+filt*REVERB_DAMP;
        buf[idx]=x+filt*REVERB_ROOM;
        if(++idx>=len)idx=0; return out;
    }
};
struct AllPassFilter {
    float buf[kMaxApLen]; int len=225,idx=0;
    AllPassFilter(){memset(buf,0,sizeof(buf));}
    float process(float x){
        float b=buf[idx],v=x+b*0.5f; buf[idx]=v;
        if(++idx>=len)idx=0; return b-0.5f*v;
    }
};
struct Reverb {
    CombFilter c[kNumCombs]; AllPassFilter ap[kNumAP];
    void init(const int cL[],const int aL[]){
        for(int i=0;i<kNumCombs;++i)c[i].len=cL[i];
        for(int i=0;i<kNumAP;++i)ap[i].len=aL[i];
    }
    float process(float x){
        float s=0; for(int i=0;i<kNumCombs;++i)s+=c[i].process(x);
        s*=0.25f; for(int i=0;i<kNumAP;++i)s=ap[i].process(s); return s;
    }
} gRevL, gRevR;

// ─── Haas delay ───────────────────────────────────────────────────────────────

static const int kHaasMax=32;
struct HaasDelay {
    float buf[kHaasMax]={}; int idx=0,len=7;
    float process(float x){
        buf[idx]=x; int r=(idx-len+kHaasMax)%kHaasMax;
        float o=buf[r]; if(++idx>=kHaasMax)idx=0; return o;
    }
} gHaas[kNumOscillators];
static float gPanL[kNumOscillators], gPanR[kNumOscillators];

// ─── SmoothedParam ────────────────────────────────────────────────────────────
// NOTE: coeff is set in setup() based on sample rate.
// next() is called per-sample for audio params, per-block for grain texture.

struct SmoothedParam {
    float cur=0, tgt=0, coeff=0.999f;
    void  set(float v){tgt=v;}
    float next(){cur+=(1.0f-coeff)*(tgt-cur); return cur;}
};

static SmoothedParam gPitch,gDensity,gCutoff,gNoiseMix,
                     gTremoloRate,gDistortion,gSubBlend;

// Grain texture uses a much FASTER smoother (block-rate read, not sample-rate)
// so it responds quickly to knob turns.
static float gGrainTextureRaw   = 0.0f;  // direct analog read, no smoothing
static float gGrainTextureSmooth= 0.0f;  // one-pole at block rate (~10 ms)
static const float kGrainSmoothCoeff = 0.85f; // block-rate one-pole

// ─── Oscillator state ─────────────────────────────────────────────────────────

static float gPhase[kNumOscillators]={}, gSubPhase=0;
static const float kDetuneRatios[]={1.000f,1.004f,0.996f,1.009f,
                                    0.991f,1.014f,0.986f,1.019f};

// ─── Filters ──────────────────────────────────────────────────────────────────

static Biquad gNoiseFL,gNoiseFR;
static Biquad gOutL1,gOutL2,gOutR1,gOutR2;
static Biquad gShelfL,gShelfR;
static Biquad gRevLP;

// ─── LFOs ────────────────────────────────────────────────────────────────────

static float gTremPhase=0, gWidthPhase=0;
static const float kWidthLFOHz=0.07f;

// ─── LED state ────────────────────────────────────────────────────────────────
//
//  LEDs are driven digitally (on/off) using PWM-style bit-banging at block rate:
//  each LED has a 0..1 brightness value that is converted to a duty-cycle
//  counter so intermediate brightnesses are rendered as rapid on/off toggling.
//
//  D0 Tremolo  — brightness = tremolo LFO value (0.75–1.0 → mapped to 0..1)
//  D1 Grain    — set to 1.0 on grain trigger, decays with a fast one-pole
//  D2 VU       — peak-hold envelope of |output|; attack instant, decay slow
//  D3 Sub beat — toggles HIGH/LOW at the sub-oscillator zero-crossing

// Per-LED PWM counter (0..kLEDPWMSteps-1); LED is HIGH while counter < brightness*steps
static const int kLEDPWMSteps = 16;   // 16-level brightness per block
static int  gLEDPWMCounter = 0;       // shared counter, increments each block

static float gLEDBrightness[4] = {0,0,0,0};  // 0..1 for each LED

// D1 grain flash decay coefficient (block-rate one-pole, ~80 ms decay)
static float gGrainFlash    = 0.0f;
static const float kGrainFlashDecay = 0.75f;  // per-block decay

// D2 VU peak envelope
static float gVUPeak        = 0.0f;
static float gVUBlockPeak   = 0.0f;  // max |output| seen this block
static const float kVUAttack = 1.0f;    // instant attack (direct assign)
static const float kVUDecay  = 0.92f;   // per-block decay (~300 ms to -20 dB)

// D3 sub heartbeat: tracks zero-crossing of the sub oscillator
static float gSubPhasePrev  = 0.0f;    // previous block's sub phase for edge detect

// ─── RNG — four completely independent streams ─────────────────────────────────
//   gRandL / gRandR  : broadband noise
//   gRandGrain       : per-sample noise fed into each grain's filter
//   gRandTrig        : grain trigger/setup decisions only

static unsigned int gRandL=12345u, gRandR=67890u,
                    gRandGrain=99991u, gRandTrig=55555u;

static inline float lcg(unsigned int& s){
    s=s*1664525u+1013904223u;
    return (float)(int)s*(1.0f/2147483648.0f);
}
static inline float lcgPos(unsigned int& s){return lcg(s)*0.5f+0.5f;}

// ─── Waveshaper ───────────────────────────────────────────────────────────────

static inline float softClip(float x,float d){
    x*=(1.0f+d*5.0f);
    float x2=x*x;
    float th=x*(27.0f+x2)/(27.0f+9.0f*x2);
    float cu=fmaxf(-1.0f,fminf(1.0f,x-x2*x*0.1667f));
    return th*(1.0f-d*0.4f)+cu*(d*0.4f);
}

// ─── Grain engine ─────────────────────────────────────────────────────────────
//
//  Key design decisions in v4.2:
//
//  1. gRandTrig is used ONLY for setup. gRandGrain is used ONLY for noise samples.
//  2. Grain BP filter centre is 80–1200 Hz (absolute, not relative to pitch),
//     so it always produces audible signal regardless of master pitch setting.
//  3. Grain amplitude starts at 1.0 and the decay is set so it reaches
//     ~0.001 at end-of-grain — a full 60 dB swing, clearly audible.
//  4. Grains are summed AFTER the main LP + waveshaper chain.
//  5. grainGain scales from 0.0 (texture=0) to 1.2 (texture=1), giving a
//     strong, obvious contrast across the knob's full range.
//  6. Inter-grain interval: texture=0 → ~8000 samples (~180 ms gaps, sparse);
//     texture=1 → ~40 samples (almost continuous, dense gravel).

struct Grain {
    bool  active=false;
    int   age=0, length=0;
    float amp=0, decay=0, panL=0.707f, panR=0.707f;
    Biquad filt;
};
static Grain gGrains[kMaxGrains];
static int   gGrainTimer=800;

static void triggerGrain(float tex) {
    Grain* g=nullptr;
    for(int i=0;i<kMaxGrains;++i) if(!gGrains[i].active){g=&gGrains[i];break;}
    if(!g) return;

    g->active=true; g->age=0;

    // tex=0 → long grain (thunderous thud); tex=1 → short (sharp click/gravel)
    float normLen = 1.0f - tex*0.85f;
    g->length = (int)(kGrainMinSamps + normLen*(kGrainMaxSamps-kGrainMinSamps));

    // Always start at full amplitude; exponential to -60 dB over grain length
    g->amp   = 1.0f;
    g->decay = powf(0.001f, 1.0f/(float)g->length);

    // Stereo pan
    float ang = lcgPos(gRandTrig)*(float)M_PI*0.5f;
    g->panL=cosf(ang); g->panR=sinf(ang);

    // BP centre: 80–1200 Hz regardless of pitch, so always clearly audible
    float bpFreq = 80.0f + lcgPos(gRandTrig)*1120.0f;
    float bpQ    = 0.8f  + lcgPos(gRandTrig)*3.0f;
    setBiquadBP(g->filt, bpFreq, bpQ, gSampleRate);
    g->filt.clearState();
}

static int nextInterval(float tex) {
    // tex=0 → ~8000 samples between grains; tex=1 → ~40 samples
    float gap = 8000.0f * powf(0.005f, tex);   // exponential sweep
    // Jitter ±40% so it never sounds metronomic
    float jitter = (lcg(gRandTrig)*0.4f) * gap;
    int interval = (int)fmaxf(1.0f, gap + jitter);
    return interval;
}

// ─────────────────────────────────────────────────────────────────────────────
//  setup()
// ─────────────────────────────────────────────────────────────────────────────

bool setup(BelaContext* context, void* userData) {
    gSampleRate    = context->audioSampleRate;
    gInvSampleRate = 1.0f/gSampleRate;

    // Per-sample smoothing ~20 ms
    float sm = expf(-1.0f/(0.020f*gSampleRate));
    gPitch.coeff=gDensity.coeff=gCutoff.coeff=gNoiseMix.coeff=
    gTremoloRate.coeff=gDistortion.coeff=gSubBlend.coeff=sm;

    gPitch.cur=30.0f; gCutoff.cur=100.0f;
    gNoiseMix.cur=0.70f; gTremoloRate.cur=1.0f;
    gGrainTextureSmooth=0.0f; gGrainTextureRaw=0.0f;

    setBiquadLP(gNoiseFL, 80.0f,  1.0f, gSampleRate);
    setBiquadLP(gNoiseFR, 80.0f,  1.0f, gSampleRate);
    setBiquadLP(gOutL1,  160.0f,  0.7f, gSampleRate);
    setBiquadLP(gOutL2,   96.0f,  0.7f, gSampleRate);
    setBiquadLP(gOutR1,  160.0f,  0.7f, gSampleRate);
    setBiquadLP(gOutR2,   96.0f,  0.7f, gSampleRate);
    setBiquadLowShelf(gShelfL, 80.0f, BASS_SHELF_GAIN, gSampleRate);
    setBiquadLowShelf(gShelfR, 80.0f, BASS_SHELF_GAIN, gSampleRate);
    setBiquadLP(gRevLP, 200.0f, 0.7f, gSampleRate);

    float sc=gSampleRate/44100.0f;
    int cL[kNumCombs],cR[kNumCombs],apL[kNumAP],apR[kNumAP];
    for(int i=0;i<kNumCombs;++i){
        cL[i]=(int)fminf(kCombLenL[i]*sc,kMaxCombLen-1);
        cR[i]=(int)fminf(kCombLenR[i]*sc,kMaxCombLen-1);
    }
    for(int i=0;i<kNumAP;++i){
        apL[i]=(int)fminf(kAPLenL[i]*sc,kMaxApLen-1);
        apR[i]=(int)fminf(kAPLenR[i]*sc,kMaxApLen-1);
    }
    gRevL.init(cL,apL); gRevR.init(cR,apR);

    for(int i=0;i<kNumOscillators;++i){
        float n=(float)i/(float)(kNumOscillators-1);
        float ang=(n-0.5f)*STEREO_WIDTH*(float)M_PI*0.5f;
        gPanL[i]=cosf((float)M_PI*0.25f+ang);
        gPanR[i]=sinf((float)M_PI*0.25f+ang);
        int hl=1+(int)(n*19.0f*sc);
        gHaas[i].len=hl<kHaasMax?hl:kHaasMax-1;
    }

    for(int i=0;i<kMaxGrains;++i) gGrains[i].active=false;
    gGrainTimer=800;

    // Configure LED pins as digital outputs, start LOW
    for(int pin=0; pin<4; ++pin){
        pinMode(context, 0, pin, OUTPUT);
        digitalWrite(context, 0, pin, LOW);
    }

    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
//  render()
// ─────────────────────────────────────────────────────────────────────────────

void render(BelaContext* context, void* userData) {

    // ── Analog reads ──────────────────────────────────────────────────────────
    float aPitch    = analogRead(context,0,0);
    float aDensity  = analogRead(context,0,1);
    float aCutoff   = analogRead(context,0,2);
    float aGrain    = analogRead(context,0,3);  // 0..1 direct
    float aNoiseMix = analogRead(context,0,4);
    float aTremRate = analogRead(context,0,5);
    float aDist     = analogRead(context,0,6);
    float aSubBlend = analogRead(context,0,7);

    gPitch.set      (10.0f + aPitch   *70.0f);
    gDensity.set    (1.0f  + aDensity *7.0f);
    gCutoff.set     (20.0f + aCutoff  *580.0f);
    gNoiseMix.set   (0.40f + aNoiseMix*0.60f);
    gTremoloRate.set(0.1f  + aTremRate*11.9f);
    gDistortion.set (aDist);
    gSubBlend.set   (aSubBlend);

    // Grain texture: simple block-rate one-pole smoother directly on the raw read.
    // No per-sample SmoothedParam — that was causing the frozen-value bug.
    gGrainTextureRaw    = aGrain;
    gGrainTextureSmooth = gGrainTextureSmooth*kGrainSmoothCoeff
                        + gGrainTextureRaw*(1.0f-kGrainSmoothCoeff);
    float tex = gGrainTextureSmooth;   // 0..1, used throughout this block

    // ── Filter update ─────────────────────────────────────────────────────────
    float cutoff = fminf(fmaxf(gCutoff.next(),15.0f), gSampleRate*0.45f);
    float cp     = fminf(cutoff*1.2f, gSampleRate*0.45f);
    setBiquadLP(gNoiseFL,cutoff,0.75f,gSampleRate);
    setBiquadLP(gNoiseFR,cutoff,0.75f,gSampleRate);
    setBiquadLP(gOutL1,cp,    0.7f,gSampleRate);
    setBiquadLP(gOutL2,cp*0.6f,0.7f,gSampleRate);
    setBiquadLP(gOutR1,cp,    0.7f,gSampleRate);
    setBiquadLP(gOutR2,cp*0.6f,0.7f,gSampleRate);

    // Pre-compute grain gain for this block.
    // 0 → completely silent; 1 → loud crunch. Strong taper at low end
    // so the contrast from silence to first grains is obvious.
    float grainGain = tex * tex * 1.2f;   // quadratic: 0.0 at 0, 1.2 at 1

    // ── Per-sample loop ───────────────────────────────────────────────────────
    for(unsigned int n=0; n<context->audioFrames; ++n){

        float pitch    = gPitch.next();
        float density  = gDensity.next();
        float noiseMix = gNoiseMix.next();
        float tremRate = gTremoloRate.next();
        float dist     = gDistortion.next();
        float subBlend = gSubBlend.next();
        int   nAct     = (int)fminf(density,(float)kNumOscillators);
        float oscGain  = nAct>0 ? 1.0f/(float)nAct : 0.0f;

        // Sub-octave
        gSubPhase += pitch*0.5f*gInvSampleRate;
        if(gSubPhase>=1.0f) gSubPhase-=1.0f;
        float subSine = sinf_neon(kTwoPi*gSubPhase);

        // Oscillator bank
        float oscL=0,oscR=0;
        for(int i=0;i<nAct;++i){
            gPhase[i]+=pitch*kDetuneRatios[i]*gInvSampleRate;
            if(gPhase[i]>=1.0f) gPhase[i]-=1.0f;
            float ph=gPhase[i];
            float si=sinf_neon(kTwoPi*ph);
            float tr=(ph<0.5f)?4.0f*ph-1.0f:3.0f-4.0f*ph;
            float o=(si*0.5f+tr*0.5f)*oscGain;
            oscL+=o*gPanL[i];
            oscR+=gHaas[i].process(o)*gPanR[i];
        }
        float sc=subSine*subBlend*0.8f;
        oscL=(oscL+sc)*OSC_MIX_MAX;
        oscR=(oscR+sc)*OSC_MIX_MAX;

        // Broadband noise
        float nL=processBiquad(gNoiseFL,lcg(gRandL));
        float nR=processBiquad(gNoiseFR,lcg(gRandR));

        float mixL=oscL*(1.0f-noiseMix)+nL*noiseMix;
        float mixR=oscR*(1.0f-noiseMix)+nR*noiseMix;

        // Low-shelf + cascaded LP + waveshaper
        mixL=processBiquad(gShelfL,mixL);
        mixR=processBiquad(gShelfR,mixR);
        mixL=processBiquad(gOutL1,mixL); mixL=processBiquad(gOutL2,mixL);
        mixR=processBiquad(gOutR1,mixR); mixR=processBiquad(gOutR2,mixR);
        mixL=softClip(mixL,dist);
        mixR=softClip(mixR,dist);

        // ── Grain engine (summed AFTER main LP + waveshaper) ──────────────
        // Fire new grain when timer expires
        if(--gGrainTimer<=0){
            if(tex > 0.005f) triggerGrain(tex);   // no grains when knob is at zero
            gGrainTimer = nextInterval(tex);
        }

        float gL=0, gR=0;
        for(int i=0;i<kMaxGrains;++i){
            Grain& g=gGrains[i];
            if(!g.active) continue;
            float gn = processBiquad(g.filt, lcg(gRandGrain));
            float gv = gn * g.amp;
            gL += gv*g.panL;
            gR += gv*g.panR;
            g.amp*=g.decay;
            if(++g.age>=g.length || g.amp<1e-5f) g.active=false;
        }

        // Grains bypass the steep output LP but pass through a moderate LP
        // to keep their high end warmly rolled off (not too bright/harsh).
        // Using the noise filter as a convenient 80 Hz LP would be too dark;
        // instead we apply a simple inline one-pole at ~1.5 kHz.
        // (No extra biquad object needed — a one-pole is sufficient here.)
        static float grainLP_L=0, grainLP_R=0;
        {
            // one-pole LP coefficient at ~1.5 kHz
            static float grainLPcoeff = -1.0f;   // computed once below
            if(grainLPcoeff < 0.0f)
                grainLPcoeff = expf(-kTwoPi*1500.0f*gInvSampleRate);
            grainLP_L = grainLP_L*grainLPcoeff + gL*(1.0f-grainLPcoeff);
            grainLP_R = grainLP_R*grainLPcoeff + gR*(1.0f-grainLPcoeff);
        }

        mixL += grainLP_L * grainGain;
        mixR += grainLP_R * grainGain;

        // Tremolo
        gTremPhase+=tremRate*gInvSampleRate;
        if(gTremPhase>=1.0f) gTremPhase-=1.0f;
        float trem=0.75f+0.25f*sinf_neon(kTwoPi*gTremPhase);
        mixL*=trem; mixR*=trem;

        // Mid-Side width LFO
        gWidthPhase+=kWidthLFOHz*gInvSampleRate;
        if(gWidthPhase>=1.0f) gWidthPhase-=1.0f;
        float wm=0.05f*sinf_neon(kTwoPi*gWidthPhase);
        float mid=0.5f*(mixL+mixR), side=0.5f*(mixL-mixR)*(1.0f+wm);
        mixL=mid+side; mixR=mid-side;

        // Reverb
        float ri=processBiquad(gRevLP,0.5f*(mixL+mixR));
        float rvL=gRevL.process(ri), rvR=gRevR.process(ri);
        mixL=mixL*(1.0f-REVERB_MIX)+rvL*REVERB_MIX;
        mixR=mixR*(1.0f-REVERB_MIX)+rvR*REVERB_MIX;

        mixL*=0.58f; mixR*=0.58f;

        audioWrite(context,n,0,mixL);
        if(context->audioOutChannels>1) audioWrite(context,n,1,mixR);

        // Accumulate peak for D2 VU meter
        float absPeak = fmaxf(fabsf(mixL), fabsf(mixR));
        if(absPeak > gVUBlockPeak) gVUBlockPeak = absPeak;
    }

    // ── LED updates (block rate) ──────────────────────────────────────────────
    //
    //  All four LED brightnesses are computed from live signal values, then
    //  converted to on/off via a shared PWM counter that increments each block.
    //  At a typical Bela block size of 16 samples this gives 16 brightness
    //  levels with a PWM frequency of ~2.7 kHz — well above flicker perception.

    // D0 — Tremolo LFO brightness
    //   The tremolo swings 0.75–1.0; remap to 0..1 so the LED visibly breathes.
    {
        float tremVal = 0.75f + 0.25f * sinf_neon(kTwoPi * gTremPhase);
        gLEDBrightness[kLED_Tremolo] = (tremVal - 0.75f) * 4.0f;  // 0..1
    }

    // D1 — Grain flash: set to 1.0 when a grain fired this block (detected by
    //   checking whether gGrainTimer was reset during the loop), then decay.
    //   We re-check by comparing the timer: if it was reset it will be larger
    //   than context->audioFrames, meaning a trigger happened this block.
    {
        if((unsigned int)gGrainTimer > context->audioFrames && tex > 0.005f)
            gGrainFlash = 1.0f;                   // grain just fired
        else
            gGrainFlash *= kGrainFlashDecay;      // decay toward zero
        gLEDBrightness[kLED_Grain] = gGrainFlash;
    }

    // D2 — VU: track peak amplitude of the output across the whole block.
    //   gVUBlockPeak is accumulated inside the per-sample loop (see above),
    //   then used here to drive an attack/decay envelope.
    {
        if(gVUBlockPeak > gVUPeak) gVUPeak = gVUBlockPeak;  // instant attack
        else                       gVUPeak *= kVUDecay;      // slow decay
        gVUBlockPeak = 0.0f;                                 // reset for next block
        // Signal sits roughly 0..0.6 after the 0.58 master gain; map to 0..1
        gLEDBrightness[kLED_VU] = fminf(gVUPeak / 0.6f, 1.0f);
    }

    // D3 — Sub heartbeat: toggle on each positive zero-crossing of sub LFO.
    //   gSubPhase wraps 0→1; a crossing is detected when the phase this block
    //   is less than it was last block (i.e. it wrapped around).
    {
        static bool subLEDState = false;
        if(gSubPhase < gSubPhasePrev)             // wrapped → zero crossing
            subLEDState = !subLEDState;           // toggle
        gSubPhasePrev = gSubPhase;
        gLEDBrightness[kLED_SubHeart] = subLEDState ? 1.0f : 0.0f;
    }

    // Apply brightnesses via block-rate PWM
    // Counter cycles 0..kLEDPWMSteps-1; LED is HIGH when counter < brightness*steps
    gLEDPWMCounter = (gLEDPWMCounter + 1) % kLEDPWMSteps;
    for(int pin=0; pin<4; ++pin){
        int threshold = (int)(gLEDBrightness[pin] * (float)kLEDPWMSteps);
        digitalWrite(context, 0, pin,
                     gLEDPWMCounter < threshold ? HIGH : LOW);
    }
}

void cleanup(BelaContext* context, void* userData){}
