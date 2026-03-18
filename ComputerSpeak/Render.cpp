/*
 * Bela Talking Computer -- Talkie TMS5220 LPC synthesis
 * ======================================================
 * Direct port of Talkie library (Peter Knight / Armin Joachimsmeyer)
 * getBits(), setNextSynthesizerData() and timerInterrupt() ported
 * exactly from Talkie.cpp, all Arduino/AVR/PROGMEM dependencies removed.
 *
 * PROJECT FILES: render.cpp + Vocab_US_TI99.h
 *
 * Analog inputs:
 *   A0 - Speech rate     (0.4-2.0x playback speed)
 *   A1 - Pitch           (0.5-2.0x pitch shift via resample rate)
 *   A2 - Word gap        (silence between words, 0-300ms)
 *   A3 - Ring mod depth  (robotic metallic effect)
 *   A4 - Sentence length (single word <-> 5-word phrase, alternating)
 *   A5 - Glitch          (stutter probability)
 *   A6 - Low-pass        (muffled telephone <-> bright)
 *   A7 - Output gain
 */

#include <Bela.h>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <stdint.h>

#include "Vocab_US_TI99.h"

// ---- TMS5220 tables (from TalkieLPC.h) -----------------------------------

static const uint8_t tmsEnergy[0x10] = {
    0x00,0x01,0x02,0x03,0x04,0x06,0x08,0x0B,
    0x10,0x16,0x1F,0x2B,0x3B,0x54,0x74,0x00
};
static const uint8_t tmsPeriod[0x40] = {
    0x00,0x10,0x11,0x12,0x13,0x14,0x15,0x16,
    0x17,0x18,0x19,0x1A,0x1B,0x1C,0x1D,0x1E,
    0x1F,0x20,0x21,0x22,0x23,0x24,0x25,0x26,
    0x27,0x28,0x29,0x2A,0x2B,0x2D,0x2F,0x31,
    0x33,0x35,0x36,0x39,0x3B,0x3D,0x3F,0x42,
    0x45,0x47,0x49,0x4D,0x4F,0x51,0x55,0x57,
    0x5C,0x5F,0x63,0x66,0x6A,0x6E,0x73,0x77,
    0x7B,0x80,0x85,0x8A,0x8F,0x95,0x9A,0xA0
};
static const int16_t tmsK1[0x20] = {
    -501,-498,-497,-495,-493,-491,-488,-482,
    -478,-474,-469,-464,-459,-452,-445,-437,
    -412,-380,-339,-288,-227,-158,-81,-1,
    80,157,226,287,337,379,411,436
};
static const int16_t tmsK2[0x20] = {
    -328,-303,-274,-244,-211,-175,-138,-99,
    -61,-22,16,55,93,131,168,205,
    240,274,307,324,339,352,363,372,
    380,388,395,401,406,410,414,417
};
// K3-K10 are int8_t in Talkie (not int16_t)
static const int8_t tmsK3[0x10]  = {-110,-97,-83,-70,-56,-43,-29,-16,2,16,29,43,56,70,83,97};
static const int8_t tmsK4[0x10]  = {-110,-97,-83,-70,-56,-43,-29,-16,2,16,29,43,56,70,83,97};
static const int8_t tmsK5[0x10]  = {-110,-97,-83,-70,-56,-43,-29,-16,2,16,29,43,56,70,83,97};
static const int8_t tmsK6[0x10]  = {-110,-97,-83,-70,-56,-43,-29,-16,2,16,29,43,56,70,83,97};
static const int8_t tmsK7[0x10]  = {-110,-97,-83,-70,-56,-43,-29,-16,2,16,29,43,56,70,83,97};
static const int8_t tmsK8[0x08]  = {-110,-73,-37,0,37,73,110,110};
static const int8_t tmsK9[0x08]  = {-110,-73,-37,0,37,73,110,110};
static const int8_t tmsK10[0x08] = {-110,-73,-37,0,37,73,110,110};

static const int8_t chirp[52] = {
    0,42,(int8_t)212,50,(int8_t)178,18,37,20,
    2,(int8_t)225,(int8_t)197,2,95,90,5,15,
    38,(int8_t)252,(int8_t)165,(int8_t)165,(int8_t)214,(int8_t)221,(int8_t)220,(int8_t)252,
    37,43,34,33,15,(int8_t)255,(int8_t)248,(int8_t)238,
    (int8_t)237,(int8_t)239,(int8_t)247,(int8_t)246,(int8_t)250,0,3,2,
    1,0,0,0,0,0,0,0,0,0,0,0
};

// ---- Talkie synth state --------------------------------------------------

static const uint8_t* WordDataPointer = NULL;
static uint8_t        WordDataBit     = 0;

static uint8_t  synthPeriod = 0;
static uint16_t synthEnergy = 0;
static int16_t  synthK1 = 0, synthK2 = 0;
static int8_t   synthK3=0,synthK4=0,synthK5=0,synthK6=0,
                synthK7=0,synthK8=0,synthK9=0,synthK10=0;

static int16_t  x0=0,x1=0,x2=0,x3=0,x4=0,x5=0,x6=0,x7=0,x8=0,x9=0;
static uint8_t  periodCounter  = 0;
static uint16_t synthRand      = 1;

// ISRCounterToNextData counts down from 200 to 0 between frame loads
static uint16_t ISRCounterToNextData = 0;
static bool     wordDone        = false;

// ---- getBits: exact port of Talkie::getBits ------------------------------

static uint8_t revByte(uint8_t a) {
    a = (a >> 4) | (a << 4);
    a = ((a & 0xcc) >> 2) | ((a & 0x33) << 2);
    a = ((a & 0xaa) >> 1) | ((a & 0x55) << 1);
    return a;
}

static uint8_t getBits(uint8_t bits) {
    uint8_t  value;
    uint16_t data;
    data = (uint16_t)revByte(WordDataPointer[0]) << 8;
    if (WordDataBit + bits > 8) {
        data |= revByte(WordDataPointer[1]);
    }
    data <<= WordDataBit;
    value = data >> (16 - bits);
    WordDataBit += bits;
    if (WordDataBit >= 8) {
        WordDataBit -= 8;
        WordDataPointer++;
    }
    return value;
}

// ---- setNextSynthesizerData: exact port ----------------------------------

static void setNextSynthesizerData() {
    uint8_t energy = getBits(4);
    if (energy == 0) {
        synthEnergy = 0;
    } else if (energy == 0xf) {
        // Stop frame
        synthEnergy = 0;
        synthK1=0; synthK2=0; synthK3=0; synthK4=0;
        synthK5=0; synthK6=0; synthK7=0; synthK8=0;
        synthK9=0; synthK10=0;
        wordDone = true;
    } else {
        synthEnergy = tmsEnergy[energy];
        uint8_t repeat   = getBits(1);
        synthPeriod      = tmsPeriod[getBits(6)];
        if (!repeat) {
            synthK1 = tmsK1[getBits(5)];
            synthK2 = tmsK2[getBits(5)];
            synthK3 = tmsK3[getBits(4)];
            synthK4 = tmsK4[getBits(4)];
            if (synthPeriod) {
                synthK5  = tmsK5 [getBits(4)];
                synthK6  = tmsK6 [getBits(4)];
                synthK7  = tmsK7 [getBits(4)];
                synthK8  = tmsK8 [getBits(3)];
                synthK9  = tmsK9 [getBits(3)];
                synthK10 = tmsK10[getBits(3)];
            }
        }
    }
}

// ---- timerInterrupt: produces one 8kHz sample, exact port ---------------

static int16_t timerInterrupt() {
    int16_t u0,u1,u2,u3,u4,u5,u6,u7,u8,u9,u10;

    if (synthPeriod) {
        if (periodCounter < synthPeriod) {
            periodCounter++;
        } else {
            periodCounter = 0;
        }
        if (periodCounter < (uint8_t)sizeof(chirp)) {
            u10 = ((int16_t)chirp[periodCounter] * (uint16_t)synthEnergy) >> 8;
        } else {
            u10 = 0;
        }
    } else {
        synthRand = (synthRand >> 1) ^ ((synthRand & 1) ? 0xB800 : 0);
        u10 = (synthRand & 1) ? (int16_t)synthEnergy : -(int16_t)synthEnergy;
    }

    // Lattice filter forward path (K1,K2 use int32 path for precision)
    u9 = u10 - (((int16_t)synthK10 * x9) >> 7);
    u8 = u9  - (((int16_t)synthK9  * x8) >> 7);
    u7 = u8  - (((int16_t)synthK8  * x7) >> 7);
    u6 = u7  - (((int16_t)synthK7  * x6) >> 7);
    u5 = u6  - (((int16_t)synthK6  * x5) >> 7);
    u4 = u5  - (((int16_t)synthK5  * x4) >> 7);
    u3 = u4  - (((int16_t)synthK4  * x3) >> 7);
    u2 = u3  - (((int16_t)synthK3  * x2) >> 7);
    u1 = u2  - (int16_t)((((int32_t)synthK2 * x1) << 1) >> 16);
    u0 = u1  - (int16_t)((((int32_t)synthK1 * x0) << 1) >> 16);

    // Lattice filter reverse path
    x9 = x8 + (((int16_t)synthK9  * u8) >> 7);
    x8 = x7 + (((int16_t)synthK8  * u7) >> 7);
    x7 = x6 + (((int16_t)synthK7  * u6) >> 7);
    x6 = x5 + (((int16_t)synthK6  * u5) >> 7);
    x5 = x4 + (((int16_t)synthK5  * u4) >> 7);
    x4 = x3 + (((int16_t)synthK4  * u3) >> 7);
    x3 = x2 + (((int16_t)synthK3  * u2) >> 7);
    x2 = x1 + (int16_t)((((int32_t)synthK2 * u1) << 1) >> 16);
    x1 = x0 + (int16_t)((((int32_t)synthK1 * u0) << 1) >> 16);
    x0 = u0;

    // Advance frame counter, load next frame every 200 samples
    ISRCounterToNextData--;
    if (ISRCounterToNextData == 0) {
        ISRCounterToNextData = 200;
        if (!wordDone) setNextSynthesizerData();
    }

    // 8-bit output as Talkie does: (u0 >> 2) + 0x80, then back to signed
    int16_t pwm = (u0 >> 2) + 0x80;
    if (pwm > 255) pwm = 255;
    if (pwm < 0)   pwm = 0;
    return (int16_t)pwm - 0x80;  // return as signed centred on 0
}

// ---- Word start ----------------------------------------------------------

static void startWord(const uint8_t* data) {
    WordDataPointer     = data;
    WordDataBit         = 0;
    wordDone            = false;
    periodCounter       = 0;
    synthRand           = 1;
    x0=x1=x2=x3=x4=x5=x6=x7=x8=x9 = 0;
    synthPeriod=0; synthEnergy=0;
    synthK1=0; synthK2=0; synthK3=0; synthK4=0;
    synthK5=0; synthK6=0; synthK7=0; synthK8=0; synthK9=0; synthK10=0;
    // Load first frame and start counter
    ISRCounterToNextData = 200;
    setNextSynthesizerData();
}

// ---- Word table ----------------------------------------------------------

struct WordDef { const uint8_t* data; int cat; };
static const WordDef kWords[] = {
    { spTHE,    0 }, { spA,      0 },
    { spSPACE,  1 }, { spTIME,   1 }, { spPOWER,  1 },
    { spWATER,  1 }, { spLAND,   1 }, { spNUMBER, 1 },
    { spPOINT,  1 }, { spSPEED,  1 }, { spMODE,   1 },
    { spLEVEL,  1 }, { spSCORE,  1 }, { spGAME,   1 },
    { spSTAR,   1 }, { spWORLD,  1 }, { spLIFE,   1 },
    { spSECTOR, 1 }, { spSIGNAL, 1 }, { spDATA,   1 },
    { spMEMORY, 1 }, { spWORDS,  1 },
    { spIS,     2 }, { spARE,    2 }, { spWILL,   2 },
    { spGO,     2 }, { spSTOP,   2 }, { spMOVE,   2 },
    { spWAIT,   2 }, { spRUN,    2 }, { spCHECK,  2 },
    { spENTER,  2 }, { spLOAD,   2 }, { spREAD,   2 },
    { spDO,     2 }, { spCOME,   2 }, { spSEE,    2 },
    { spKNOW,   2 }, { spNEED,   2 }, { spMAKE,   2 },
    { spFIND,   2 }, { spLEARN,  2 }, { spHELP,   2 },
    { spPLAY,   2 }, { spCALL,   2 },
    { spGOOD,   3 }, { spBAD,    3 }, { spHIGH,   3 },
    { spLOW,    3 }, { spFAST,   3 }, { spSLOW,   3 },
    { spFULL,   3 }, { spOLD,    3 }, { spNEXT,   3 },
    { spLAST,   3 }, { spLONG,   3 },
};
static const int kWordCount = (int)(sizeof(kWords)/sizeof(kWords[0]));

// ---- Phrase sequencer ----------------------------------------------------

static constexpr int kMaxQueue = 10;
static int  gQueue[kMaxQueue];
static int  gQueueLen   = 0;
static int  gQueuePos   = 0;
static int  gUtterCount = 0;

template<typename T> static inline T tclamp(T v,T lo,T hi){return v<lo?lo:(v>hi?hi:v);}

static uint32_t gRandSeed = 0xDEADBEEF;
static inline float rnd(){
    gRandSeed^=gRandSeed<<13; gRandSeed^=gRandSeed>>17; gRandSeed^=gRandSeed<<5;
    return (float)(gRandSeed>>1)/2147483647.0f;
}

static int pickCat(int cat){
    int idx[64],n=0;
    for(int i=0;i<kWordCount&&n<64;i++) if(kWords[i].cat==cat) idx[n++]=i;
    return n?idx[rand()%n]:0;
}

static void buildPhrase(float c){
    gQueueLen=0; gQueuePos=0;
    bool sw=(c<0.35f)||(c<=0.65f&&gUtterCount%2==0);
    auto push=[&](int cat){ if(gQueueLen<kMaxQueue) gQueue[gQueueLen++]=pickCat(cat); };
    if(sw){
        int cats[3]={1,2,3}; push(cats[rand()%3]);
    } else {
        int d=tclamp(1+(int)(c*4.0f),1,4);
        switch(d){
            case 1:  push(1);push(2); break;
            case 2:  push(0);push(1);push(2); break;
            case 3:  push(0);push(3);push(1);push(2); break;
            default: push(0);push(3);push(1);push(2);push(0);push(1); break;
        }
    }
    ++gUtterCount;
}

// ---- Bela state ----------------------------------------------------------

static float gSampleRate  = 44100.0f;
static constexpr int kTMSRate = 8000;

static float gResPhase = 0.0f;
static float gPrevS    = 0.0f;
static float gCurrS    = 0.0f;

static int   gGapRemain  = 0;
static bool  gInGap      = false;
static int   gPauseSamps = 0;

static float gRingPhase  = 0.0f;
static float gLPState    = 0.0f;
static float gDCx=0, gDCy=0;

static float gCV[8]={0.5f,0.5f,0.5f,0.5f,0.5f,0.5f,0.5f,0.5f};
static constexpr float kCVSmooth = 0.001f;
static float gGlitchTimer = 200.0f;

// ---- Setup ---------------------------------------------------------------

bool setup(BelaContext* context, void* /*u*/) {
    gSampleRate = context->audioSampleRate;
    srand(42);
    buildPhrase(0.5f);
    if (gQueueLen > 0) startWord(kWords[gQueue[0]].data);
    return true;
}

// ---- Render --------------------------------------------------------------

void render(BelaContext* context, void* /*u*/) {
    const int nF = context->audioFrames;

    for (int n = 0; n < nF; n++) {

        int af = n/2;
        if (af < (int)context->analogFrames) {
            for (int ch=0;ch<8;ch++) {
                float raw = analogRead(context,af,ch);
                gCV[ch] += kCVSmooth*(raw-gCV[ch]);
            }
        }
        float cvRate    = gCV[0];
        float cvPitch   = gCV[1];
        float cvGap     = gCV[2];
        float cvRingMod = gCV[3];
        float cvComp    = gCV[4];
        float cvGlitch  = gCV[5];
        float cvLP      = gCV[6];
        float cvGain    = gCV[7];

        if (gPauseSamps > 0) {
            --gPauseSamps;
            audioWrite(context,n,0,0.0f); audioWrite(context,n,1,0.0f);
            continue;
        }

        if (gInGap) {
            if (--gGapRemain <= 0) {
                gInGap = false;
                gQueuePos++;
                if (gQueuePos >= gQueueLen) {
                    gPauseSamps = (int)((0.4f + rnd()*0.8f)*gSampleRate);
                    buildPhrase(cvComp);
                    gQueuePos = 0;
                }
                startWord(kWords[gQueue[gQueuePos]].data);
            }
            audioWrite(context,n,0,0.0f); audioWrite(context,n,1,0.0f);
            continue;
        }

        // Glitch: rewind data pointer slightly
        gGlitchTimer -= 1.0f;
        if (gGlitchTimer <= 0.0f) {
            if (rnd() < cvGlitch*cvGlitch*0.01f && WordDataPointer != NULL) {
                if (WordDataBit >= 8) { WordDataBit -= 8; WordDataPointer--; }
            }
            gGlitchTimer = 80.0f + rnd()*300.0f;
        }

        // Resample TMS 8kHz -> Bela sample rate
        float pitchScale = 0.5f + cvPitch*1.5f;
        float rateScale  = 0.4f + cvRate*1.6f;
        float step = ((float)kTMSRate/gSampleRate) * rateScale * pitchScale;

        gResPhase += step;
        while (gResPhase >= 1.0f) {
            gResPhase -= 1.0f;
            gPrevS = gCurrS;
            if (!wordDone) {
                gCurrS = (float)timerInterrupt() / 128.0f;
            } else {
                gCurrS = 0.0f;
                if (!gInGap) {
                    gInGap = true;
                    int gapMs = (int)(cvGap * 300.0f);
                    gGapRemain = (int)(gapMs * 0.001f * gSampleRate);
                    if (gGapRemain < 1) gGapRemain = 1;
                }
            }
        }

        float out = gPrevS + gResPhase*(gCurrS-gPrevS);

        // Ring modulator
        float rmFreq = 30.0f + cvRingMod*170.0f;
        gRingPhase += 2.0f*(float)M_PI*rmFreq/gSampleRate;
        if (gRingPhase > 2.0f*(float)M_PI) gRingPhase -= 2.0f*(float)M_PI;
        out = out*(1.0f-cvRingMod*0.85f) + out*sinf(gRingPhase)*cvRingMod*0.85f;

        // Low-pass
        float cutoff = 300.0f + cvLP*7700.0f;
        float lpC = 1.0f - expf(-2.0f*(float)M_PI*cutoff/gSampleRate);
        gLPState += lpC*(out-gLPState); out=gLPState;

        // DC block
        float dc = out-gDCx+0.995f*gDCy; gDCx=out; gDCy=dc; out=dc;

        // Output gain
        out = out / (1.0f + fabsf(out));
        out *= 0.7f + cvGain*0.3f;
        out  = tclamp(out,-1.0f,1.0f);

        audioWrite(context,n,0,out);
        audioWrite(context,n,1,out);
    }
}

void cleanup(BelaContext* /*c*/, void* /*u*/) {}
