// ============================================================================
//  BLACK DIFFUSE AURORAL  -  drone synthesiser for the Bela platform
// ----------------------------------------------------------------------------
//  Reconstructs the character of N_O_I_S_U - "Black Diffuse Auroral":
//  a sub-heavy, beating low-frequency oscillator cluster, soft-saturated,
//  smeared through a long diffusion reverb, with a fine granular crackle
//  layer on top and a fully decorrelated ("auroral") stereo field.
//
//  Derived from analysis of the source file:
//    - 81% of energy below 160 Hz, fundamental cluster near 50 Hz
//    - 6+ partials spaced 3-7 Hz apart -> slow beating, not a clean tone
//    - ~5 grains/sec, ~24 ms attack, long decay -> granular crust
//    - L/R correlation ~ -0.02 -> wide, phasey stereo
//    - ~0.06 Hz amplitude swell -> very slow breathing
//
//  Hardware: 8 potentiometers on analog in 0..7, 4 LEDs on digital 0..3.
//  Build target: Bela / BeagleBone, mono-summed synth, stereo output.
// ============================================================================

#include <Bela.h>
#include <cmath>
#include <cstdlib>
#include <cstring>

// ----------------------------------------------------------------------------
//  Constants
// ----------------------------------------------------------------------------
static const float PI  = 3.14159265358979f;
static const float TPI = 6.28318530717959f;

static float gSR      = 44100.0f;   // set in setup()
static float gInvSR   = 1.0f / 44100.0f;

static const int kPartials = 6;     // size of the detuned LFO cluster

// reciprocal of RAND_MAX as a float, so rand()->[0,1) needs no int division
static const float kInvRandMax = 1.0f / (float)RAND_MAX;

// Pin maps -------------------------------------------------------------------
enum { P_PITCH=0, P_SPREAD=1, P_DRIVE=2, P_GRAINDENS=3,
       P_GRAINTONE=4, P_DIFFUSION=5, P_WIDTH=6, P_SWELL=7 };
enum { LED_SWELL=0, LED_GRAIN=1, LED_DRIVE=2, LED_REVERB=3 };

// ----------------------------------------------------------------------------
//  Small helpers
// ----------------------------------------------------------------------------
static inline float clampf(float x, float lo, float hi){
    return x < lo ? lo : (x > hi ? hi : x);
}
// one-pole smoother for control values (de-zippers the pots)
struct OnePole {
    float z = 0.0f, a = 0.002f;
    inline float process(float x){ z += a * (x - z); return z; }
    inline void setTimeMs(float ms){
        a = 1.0f - expf(-1.0f / (0.001f * ms * gSR));
    }
};
// uniform white noise in [-1,1]
static inline float frand(){
    return 2.0f * ((float)rand() * kInvRandMax) - 1.0f;
}
// uniform random in [0,1)
static inline float frand01(){
    return (float)rand() * kInvRandMax;
}

// ----------------------------------------------------------------------------
//  Sine partial with slow independent random pitch drift
//  -> this IS the measured beating cluster
// ----------------------------------------------------------------------------
struct DriftSine {
    float phase   = 0.0f;
    float drift   = 0.0f;     // current slow pitch offset (ratio)
    float driftT  = 0.0f;     // target offset
    int   driftCt = 0;        // samples until next target pick
    float baseRatio = 1.0f;   // fixed inharmonic ratio of this partial

    inline float process(float baseHz, float spread){
        // re-pick a slow drift target a few times per second
        if (--driftCt <= 0) {
            driftT  = frand() * 0.5f;                 // +/- half a "spread unit"
            driftCt = (int)(gSR * (0.8f + 1.6f * frand01()));
        }
        drift += (driftT - drift) * 0.00003f;          // very slow glide

        float hz = baseHz * baseRatio * (1.0f + spread * drift);
        phase += TPI * hz * gInvSR;
        if (phase >= TPI) phase -= TPI;
        // cheap accurate sine
        return sinf(phase);
    }
};

// ----------------------------------------------------------------------------
//  Schroeder all-pass section (diffusion building block)
// ----------------------------------------------------------------------------
struct Allpass {
    float* buf = nullptr;
    int    len = 0, idx = 0;
    float  g   = 0.5f;
    void init(int n){
        len = n; idx = 0;
        buf = new float[(size_t)n];
        memset(buf, 0, (size_t)n * sizeof(float));
    }
    ~Allpass(){ delete[] buf; }
    inline float process(float x){
        float d = buf[idx];
        float y = -x + d;
        buf[idx] = x + d * g;
        if (++idx >= len) idx = 0;
        return y;
    }
};

// ----------------------------------------------------------------------------
//  Feedback comb filter with a damping low-pass (Freeverb-style)
// ----------------------------------------------------------------------------
struct Comb {
    float* buf = nullptr;
    int    len = 0, idx = 0;
    float  fb  = 0.8f;     // feedback (-> decay)
    float  damp= 0.4f;     // high-frequency damping
    float  store = 0.0f;   // damping lp state
    void init(int n){
        len = n; idx = 0;
        buf = new float[(size_t)n];
        memset(buf, 0, (size_t)n * sizeof(float));
    }
    ~Comb(){ delete[] buf; }
    inline float process(float x){
        float y = buf[idx];
        store = y * (1.0f - damp) + store * damp;   // dampen the tail
        buf[idx] = x + store * fb;
        if (++idx >= len) idx = 0;
        return y;
    }
};

// ----------------------------------------------------------------------------
//  Stereo diffusion reverb: 4 combs in parallel -> 3 all-passes in series,
//  per channel, with deliberately mismatched delay lengths for decorrelation.
// ----------------------------------------------------------------------------
struct DiffuseReverb {
    Comb     combL[4], combR[4];
    Allpass  apL[4],   apR[4];     // 4th allpass added for decorrelation
    // base prime-ish lengths (samples @ 44.1k); L and R are deliberately
    // far apart so the two channels diffuse into independent fields
    // (analysis showed L/R correlation ~ -0.02).
    void init(){
        const int cl[4] = {1116, 1188, 1277, 1356};
        const int cr[4] = {1601, 1693, 1781, 1867};
        const int al[4] = {225, 556, 441, 341};
        const int ar[4] = {276, 619, 487, 398};
        for (int i=0;i<4;i++){ combL[i].init(cl[i]); combR[i].init(cr[i]); }
        for (int i=0;i<4;i++){ apL[i].init(al[i]);   apR[i].init(ar[i]); }
    }
    void setDecay(float fb, float damp){
        for (int i=0;i<4;i++){
            combL[i].fb=fb;  combL[i].damp=damp;
            combR[i].fb=fb;  combR[i].damp=damp;
        }
    }
    inline void process(float in, float& outL, float& outR){
        float l=0.0f, r=0.0f;
        for (int i=0;i<4;i++){ l+=combL[i].process(in); r+=combR[i].process(in); }
        l *= 0.25f; r *= 0.25f;
        for (int i=0;i<4;i++){ l=apL[i].process(l); r=apR[i].process(r); }
        outL=l; outR=r;
    }
};

// ----------------------------------------------------------------------------
//  Granular crackle generator: short filtered noise grains, soft 24 ms attack
// ----------------------------------------------------------------------------
struct GrainLayer {
    // current grain envelope state
    bool   active = false;
    int    pos = 0, len = 0, atk = 0;
    int    nextIn = 0;            // samples until next grain
    float  amp = 0.0f;
    // 2-pole state-variable bandpass for grain tone
    float  bpLo=0.0f, bpBp=0.0f;
    float  fLast = 0.0f;          // flash signal for LED

    inline void trigger(){
        active = true; pos = 0;
        len = (int)(gSR * (0.10f + 0.25f * frand01())); // 100-350ms
        atk = (int)(gSR * 0.024f);                       // 24 ms attack
        amp = 0.4f + 0.6f * frand01();
    }
    // density 0..1 -> ~0..12 grains/sec ; toneHz = bandpass centre
    inline float process(float density, float toneHz){
        fLast *= 0.999f;
        if (--nextIn <= 0){
            float rate = density * 12.0f;                 // grains per second
            if (rate < 0.001f) rate = 0.001f;
            float meanGap = gSR / rate;
            nextIn = (int)(meanGap * (0.5f + frand01()));
            if (density > 0.001f){ trigger(); fLast = 1.0f; }
        }
        if (!active) return 0.0f;

        // envelope: linear attack, exponential-ish decay
        float env;
        if (pos < atk)            env = (float)pos / (float)atk;
        else {
            float d = (float)(pos - atk) / (float)(len - atk + 1);
            env = (1.0f - d); env *= env;                 // squared decay
        }
        // bandpass-filtered white noise
        float n = frand();
        float f = 2.0f * sinf(PI * clampf(toneHz,40.0f,8000.0f) * gInvSR);
        float q = 0.6f;
        bpLo += f * bpBp;
        float hp = n - bpLo - q * bpBp;
        bpBp += f * hp;
        float out = bpBp * env * amp;

        if (++pos >= len) active = false;
        return out;
    }
};

// ============================================================================
//  Global instrument state
// ============================================================================
static DriftSine     gCluster[kPartials];
static DiffuseReverb gReverb;
static GrainLayer    gGrains;

// control smoothers
static OnePole sPitch, sSpread, sDrive, sGrainDens, sGrainTone,
               sDiffusion, sWidth, sSwell;

// slow swell LFO (~0.06 Hz measured)
static float gSwellPhase = 0.0f;

// final output DC blocker (per channel)
static float gDcXl=0.0f, gDcYl=0.0f, gDcXr=0.0f, gDcYr=0.0f;

// LED metering smoothers
static float gLedDriveSm=0.0f, gLedRevSm=0.0f, gLedSwellSm=0.0f;

// audio-frame counter for cheap block-rate control updates
static int gCtl = 0;

// ----------------------------------------------------------------------------
//  setup()
// ----------------------------------------------------------------------------
bool setup(BelaContext* context, void* userData)
{
    gSR    = context->audioSampleRate;
    gInvSR = 1.0f / gSR;

    // Inharmonic ratios for the partials. These come straight from the
    // analysed peak set (30/33/46/50/65/103 Hz) normalised to the ~50 Hz
    // fundamental -> deliberately NOT a clean harmonic series, which is
    // what gives the cluster its slow churning beat.
    static const float ratios[kPartials] =
        { 0.602f, 0.660f, 0.922f, 1.000f, 1.300f, 2.060f };

    for (int i=0;i<kPartials;i++){
        gCluster[i].baseRatio = ratios[i];
        gCluster[i].phase     = TPI * frand01();          // random start
        gCluster[i].driftCt   = (int)(gSR * frand01());
    }

    gReverb.init();
    gGrains.nextIn = 1;

    // control smoothing time-constants
    sPitch.setTimeMs(120.0f);   sSpread.setTimeMs(200.0f);
    sDrive.setTimeMs(80.0f);    sGrainDens.setTimeMs(150.0f);
    sGrainTone.setTimeMs(120.0f);sDiffusion.setTimeMs(300.0f);
    sWidth.setTimeMs(150.0f);   sSwell.setTimeMs(250.0f);

    // initialise smoothers near the analysed "centre" sound so the first
    // block does not jump
    sPitch.z=0.5f; sSpread.z=0.4f; sDrive.z=0.45f; sGrainDens.z=0.4f;
    sGrainTone.z=0.3f; sDiffusion.z=0.7f; sWidth.z=0.8f; sSwell.z=0.6f;

    return true;
}

// ----------------------------------------------------------------------------
//  render()
// ----------------------------------------------------------------------------
void render(BelaContext* context, void* userData)
{
    const int nAudio  = context->audioFrames;
    const int nAnalog = context->analogFrames;

    // --- read & smooth the 8 pots once per block -----------------------------
    // analogRead is at analog rate; sample frame 0 of this block.
    float rawPitch     = analogRead(context, 0, P_PITCH);
    float rawSpread    = analogRead(context, 0, P_SPREAD);
    float rawDrive     = analogRead(context, 0, P_DRIVE);
    float rawGrainDens = analogRead(context, 0, P_GRAINDENS);
    float rawGrainTone = analogRead(context, 0, P_GRAINTONE);
    float rawDiffusion = analogRead(context, 0, P_DIFFUSION);
    float rawWidth     = analogRead(context, 0, P_WIDTH);
    float rawSwell     = analogRead(context, 0, P_SWELL);

    float kPitch     = sPitch.process(rawPitch);
    float kSpread    = sSpread.process(rawSpread);
    float kDrive     = sDrive.process(rawDrive);
    float kGrainDens = sGrainDens.process(rawGrainDens);
    float kGrainTone = sGrainTone.process(rawGrainTone);
    float kDiffusion = sDiffusion.process(rawDiffusion);
    float kWidth     = sWidth.process(rawWidth);
    float kSwell     = sSwell.process(rawSwell);

    // --- map pot values to musical ranges ------------------------------------
    // Pot 1  Drift Pitch  : 22..112 Hz, exponential, centre (~0.5) ~50 Hz
    float baseHz = 22.0f * powf(5.09f, kPitch);           // 22 -> ~112, 0.5->~50
    // Pot 2  Spread       : 0 (fused) .. ~0.06 detune depth (wide beating)
    float spread = kSpread * 0.06f;
    // Pot 3  Drive        : tanh pre-gain 1..14 (seismic territory at the top)
    float drive  = 1.0f + kDrive * 13.0f;
    // Pot 4  Grain Density
    float grainD = kGrainDens;
    // Pot 5  Grain Tone   : 60 Hz .. 6 kHz bandpass centre, exponential
    float grainHz= 60.0f * powf(100.0f, kGrainTone);
    // Pot 6  Diffusion    : reverb feedback 0.70 .. 0.965, damping eases off
    float revFb  = 0.70f + kDiffusion * 0.265f;
    float revDmp = 0.55f - kDiffusion * 0.35f;            // longer = brighter tail
    gReverb.setDecay(revFb, revDmp);
    // Pot 7  Width        : 0 (mono) .. 1 (full decorrelation)
    float width  = kWidth;
    // Pot 8  Swell        : depth of the ~0.06 Hz amplitude breathing
    float swellD = kSwell;

    // swell LFO advance (per block is fine, it is glacially slow)
    const float swellRate = 0.06f;                        // Hz, measured
    float swellInc = TPI * swellRate * gInvSR;

    // --- per-sample synthesis ------------------------------------------------
    for (int n=0; n<nAudio; ++n){

        // 1) detuned LFO cluster -------------------------------------------
        float cluster = 0.0f;
        for (int p=0;p<kPartials;p++)
            cluster += gCluster[p].process(baseHz, spread);
        cluster *= (1.0f / kPartials);

        // 2) slow swell -----------------------------------------------------
        gSwellPhase += swellInc;
        if (gSwellPhase >= TPI) gSwellPhase -= TPI;
        float swell = 1.0f - swellD * 0.5f * (1.0f - sinf(gSwellPhase));
        cluster *= swell;

        // 3) soft saturation -> low-order harmonic haze ---------------------
        // asymmetric pre-bias gives even harmonics; the cluster alone is too
        // pure to reach the measured ~70-190 Hz centroid, so Drive must add
        // real upper-partial energy even at moderate settings.
        float biased = cluster + 0.12f;
        float driven = tanhf(biased * drive) - tanhf(0.12f * drive);
        // squared term sprinkles in a touch more high-order haze
        driven += 0.15f * (driven * driven - 0.02f) * (drive * 0.1f);
        // gentle gain compensation so Drive does not just get louder
        driven *= (1.0f / (0.6f + 0.5f * drive));
        driven *= 1.7f;

        // 4) granular crackle layer ----------------------------------------
        float grain = gGrains.process(grainD, grainHz) * 0.85f;

        // dry mono sum fed to the reverb
        float dryMono = driven + grain;

        // 5) diffusion reverb ----------------------------------------------
        float wetL, wetR;
        gReverb.process(dryMono, wetL, wetR);

        // mix: the source is heavily smeared, so favour the wet field
        float mixWet = 0.78f;
        float baseL  = dryMono * (1.0f - mixWet) + wetL * mixWet;
        float baseR  = dryMono * (1.0f - mixWet) + wetR * mixWet;

        // 6) stereo decorrelation ------------------------------------------
        // crossfade between mono (sum) and the decorrelated reverb pair
        float mono = 0.5f * (baseL + baseR);
        float outL = mono + width * (baseL - mono);
        float outR = mono + width * (baseR - mono);

        // 7) DC blockers ----------------------------------------------------
        float yl = outL - gDcXl + 0.9985f * gDcYl;
        gDcXl = outL; gDcYl = yl;
        float yr = outR - gDcXr + 0.9985f * gDcYr;
        gDcXr = outR; gDcYr = yr;

        // master trim: source measured ~ -19 dBFS RMS / -1.6 dBFS peak.
        // This headroomy level keeps the soft clipper out of the signal
        // path during normal playing.
        const float kMasterTrim = 0.15f;
        yl *= kMasterTrim;
        yr *= kMasterTrim;

        // final limiter-ish soft clip for safety only (should rarely engage)
        yl = tanhf(yl);
        yr = tanhf(yr);

        audioWrite(context, n, 0, yl);
        audioWrite(context, n, 1, yr);

        // --- LED metering (cheap, per-sample envelope) --------------------
        float lvl = 0.5f * (fabsf(yl) + fabsf(yr));
        gLedSwellSm += 0.0008f * (swell * lvl * 3.0f - gLedSwellSm);
        gLedDriveSm += 0.002f  * (kDrive            - gLedDriveSm);
        gLedRevSm   += 0.0006f * (revFb * lvl * 3.0f - gLedRevSm);
    }

    // --- drive the 4 LEDs (digital out, simple PWM-by-threshold) -------------
    // Bela digital pins are 0/1; we dither at block rate for brightness.
    static int ditherCt = 0;
    ditherCt = (ditherCt + 1) & 0x0FFFFFFF;        // keep small, no overflow
    float ledSwell = clampf(gLedSwellSm, 0.0f, 1.0f);
    float ledGrain = clampf(gGrains.fLast, 0.0f, 1.0f);
    float ledDrive = clampf(gLedDriveSm, 0.0f, 1.0f);
    float ledRev   = clampf(gLedRevSm,   0.0f, 1.0f);

    for (int n=0; n<context->digitalFrames; ++n){
        int   step = (ditherCt * context->digitalFrames + n) & 15;
        float ph   = (float)step / 16.0f;
        digitalWriteOnce(context, n, LED_SWELL,  ledSwell > ph ? 1 : 0);
        digitalWriteOnce(context, n, LED_GRAIN,  ledGrain > ph ? 1 : 0);
        digitalWriteOnce(context, n, LED_DRIVE,  ledDrive > ph ? 1 : 0);
        digitalWriteOnce(context, n, LED_REVERB, ledRev   > ph ? 1 : 0);
    }

    (void)nAnalog; (void)gCtl;
}

// ----------------------------------------------------------------------------
//  cleanup()
// ----------------------------------------------------------------------------
void cleanup(BelaContext* context, void* userData)
{
    // Allpass / Comb destructors free their buffers automatically.
}

// ============================================================================
//  POTENTIOMETER MAP  (analog in 0..7)
// ----------------------------------------------------------------------------
//  0  DRIFT PITCH   base frequency of the cluster, 20-80 Hz (centre ~50)
//  1  SPREAD        detune between the 6 partials -> beating / diffusion
//  2  DRIVE         tanh saturation -> harmonic haze; full = seismic sub
//  3  GRAIN DENSITY rate of the crackle layer, 0 to ~12 grains/sec
//  4  GRAIN TONE    bandpass centre of the grains, 60 Hz - 6 kHz
//  5  DIFFUSION     reverb decay length -> the "auroral" smear
//  6  WIDTH         stereo decorrelation, mono -> fully wide
//  7  SWELL         depth of the ~0.06 Hz slow amplitude breathing
//
//  LED MAP  (digital out 0..3)
//  0  cluster swell level     2  drive amount
//  1  grain trigger flash     3  reverb tail energy
// ============================================================================
