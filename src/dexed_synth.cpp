#include "dexed_synth.h"
#include "itcm.h"
#include "msfa/exp2.h"
#include "msfa/sin.h"
#include "msfa/freqlut.h"
#include "msfa/env.h"
#include "msfa/pitchenv.h"
#include "msfa/porta.h"
#include <cmath>
#include <cstring>

namespace
{
// Q24 fixed-point -- msfa/sin.cpp builds its lookup table to a full-scale
// sine amplitude of exactly 1<<24 (see its own table-fill code: a 1<<30
// scale right-shifted by 6), and fm_op_kernel.cpp's compute()/
// compute_pure() apply per-operator gain as a Q24 multiply-then->>24.
// Derived directly from the Apache-2.0 msfa source this project vendors
// (sin.cpp/fm_op_kernel.cpp), not from dexed.cpp's own (excluded) code.
//
// A single Q24 unit alone is NOT actual full-scale for a real rendered
// note, though -- msfa's internal envelope/gain-staging deliberately
// carries headroom well above nominal unity (the same "room for FM
// modulation to push past simple +-1 waveshaping" design every fixed-
// point FM engine like this needs), and this project's own test patch
// sums 6 fully additive carriers on top of that. Measured directly on
// real hardware (temporary peak-diagnostic instrumentation, since
// removed): a single note peaks at ~1.74x this naive Q24-only scale, a
// 3-note chord ~4.41x. kHeadroomScale is sized from that real number
// (not a guess) so a 3-note chord lands right at the edge of unity,
// leaving tanhf()'s own soft limiter in Process() to only ever have to
// handle genuinely rare, bigger peaks (bigger chords, unusual phase
// alignment) rather than routinely saturating hard on ordinary playing.
constexpr float kQ24Scale      = 1.f / (float)(1 << 24);
constexpr float kHeadroomScale = 1.f / 4.41f;
} // namespace

void DexedSynth::Init(float sample_rate)
{
    sample_rate_ = sample_rate;

    // One-time msfa engine setup -- every one of these is idempotent
    // (each guards itself with its own static initDone flag), so calling
    // this more than once is harmless, but Init() is only ever called
    // once from main() in practice.
    Exp2::init();
    Sin::init();
    Freqlut::init(sample_rate_);
    Env::init_sr(sample_rate_);
    PitchEnv::init(sample_rate_);
    Porta::init_sr(sample_rate_);
    Lfo::init(sample_rate_);

    ctrls_.core          = &engine_;
    ctrls_.values_[kControllerPitch]        = 0x2000; // centered, see Dx7Note::compute()'s pb math
    ctrls_.values_[kControllerPitchRange]   = 2; // +-2 semitones, a reasonable default
    ctrls_.values_[kControllerPitchStep]    = 0; // continuous bend, not stepped
    ctrls_.values_[kControllerPortamentoGlissando] = 0;
    ctrls_.masterTune    = 0;
    ctrls_.opSwitch      = 0x3f; // all 6 operators enabled
    ctrls_.aftertouch_cc = 0;
    ctrls_.breath_cc     = 0;
    ctrls_.foot_cc       = 0;
    ctrls_.modwheel_cc   = 0;
    ctrls_.portamento_enable_cc = false;
    ctrls_.portamento_cc        = 0;
    ctrls_.portamento_gliss_cc  = false;
    // Mod wheel -> vibrato (pitch), the standard default routing real
    // DX7 hardware/most patches use -- Controllers::wheel (an FmMod) is
    // otherwise default-constructed with range 0 and every target
    // false, i.e. completely disabled regardless of modwheel_cc's own
    // value, which is a genuinely separate, independent setting from
    // the patch's own pitch-mod-sensitivity byte (see patch_[143]
    // below) -- both have to be configured for the wheel to actually
    // move anything.
    ctrls_.wheel.setRange(50);
    ctrls_.wheel.setTarget(1); // bit0 = pitch
    ctrls_.refresh();

    // Hardcoded test patch (Phase 2 only -- Phase 6 adds real preset
    // save/load, Phase 7 real SysEx import): DX7 algorithm 32 (index 31,
    // see msfa/fm_core.cpp's own algorithms table) -- all 6 operators are
    // independent carriers summed straight into the main output, no
    // modulation chains and no feedback, i.e. a plain 6-partial additive/
    // organ-style patch. Chosen for Phase 2 specifically because it's
    // the algorithm that exercises all 6 operators' real per-sample cost
    // every single note, the same worst-case-per-voice shape this
    // phase's CPU measurement needs -- not because it's a good-sounding
    // patch (Phase 6+ ships real, well-known factory patches instead).
    std::memset(patch_, 0, sizeof(patch_));
    const int   kCoarse[6]   = {1, 2, 3, 4, 5, 6}; // harmonic series
    // A natural-looking additive taper -- DX7 output-level units are
    // logarithmic, not linear, so hand-tuning these against real-
    // hardware clipping/loudness reports turned out unpredictable (small
    // unit changes near the top barely move perceived loudness, larger
    // ones further down cut it drastically). Left at a normal musical
    // shape; real headroom safety now comes from Process()'s own soft
    // limiter instead of fighting these units -- see its own comment.
    const int   kOutLevel[6] = {99, 70, 55, 45, 35, 25}; // decaying with harmonic number
    const int   kEgRate[4]   = {99, 60, 35, 50};
    const int   kEgLevel[4]  = {99, 99, 60, 0};
    for(int op = 0; op < 6; op++)
    {
        int off = op * 21;
        for(int i = 0; i < 4; i++)
        {
            patch_[off + i]     = (uint8_t)kEgRate[i];
            patch_[off + 4 + i] = (uint8_t)kEgLevel[i];
        }
        patch_[off + 8]  = 39; // break point, unused (depths below are 0)
        patch_[off + 9]  = 0;  // left depth
        patch_[off + 10] = 0;  // right depth
        patch_[off + 11] = 0;  // left curve
        patch_[off + 12] = 0;  // right curve
        patch_[off + 13] = 0;  // rate scaling
        patch_[off + 14] = 0;  // amp mod sensitivity
        patch_[off + 15] = 0;  // velocity sensitivity
        patch_[off + 16] = (uint8_t)kOutLevel[op];
        patch_[off + 17] = 0; // mode: 0 = ratio (not fixed Hz)
        patch_[off + 18] = (uint8_t)kCoarse[op];
        patch_[off + 19] = 0; // fine
        patch_[off + 20] = 7; // detune, 7 = centered/no detune
    }
    // Pitch EG: flat (no pitch envelope movement).
    for(int i = 0; i < 4; i++)
    {
        patch_[126 + i] = 99;
        patch_[130 + i] = 50;
    }
    patch_[134] = 31; // algorithm 32 (0-based index)
    patch_[135] = 0;  // feedback off
    patch_[136] = 0;  // osc key sync, unused by msfa's own compute path
    // LFO speed: must be genuinely nonzero for the LFO's own phase to
    // advance at all (0 froze it entirely) -- the mod wheel's own pitch
    // contribution (Dx7Note::compute()'s pmod_2) is scaled by the LFO's
    // CURRENT phase (senslfo), not a fixed value, so a frozen LFO turned
    // moving the wheel into a static pitch offset instead of oscillating
    // vibrato. 35 is a moderate, typical vibrato rate (~4-5Hz).
    patch_[137] = 35;
    patch_[138] = 0;  // LFO delay
    patch_[139] = 0;  // LFO pitch mod depth -- LFO itself still has no AUTOMATIC vibrato of its own (that's this byte, separate from the wheel's own contribution above); only moving the mod wheel introduces any pitch modulation
    patch_[140] = 0;  // LFO amp mod depth
    patch_[141] = 0;  // LFO sync
    patch_[142] = 0;  // LFO waveform
    // LFO pitch mod SENSITIVITY -- despite the name, this is the note's
    // overall susceptibility to ANY pitch modulation source (both the
    // LFO's own depth above AND the mod wheel's own contribution via
    // ctrls_.wheel -- see Dx7Note::compute()'s senslfo, which gates
    // both through the same multiply). Left at 0 initially, this
    // silently killed mod wheel vibrato even with wheel routing
    // correctly configured in Init(); pitchmodsenstab[3] == 33, a
    // moderate default depth once the wheel is actually moved.
    patch_[143] = 3;
    patch_[144] = 24; // transpose, unused by msfa's own compute path

    lfo_.reset(&patch_[137]);

    // Every voice's Dx7Note gets a real init() up front -- RenderQuantum()
    // below calls compute() on every voice slot unconditionally every
    // quantum, even ones never yet NoteOn()'d, and a Dx7Note that's never
    // had init() called has an uninitialized algorithm_ index (undefined
    // behavior the moment render() looks it up in fm_core.cpp's
    // algorithms[32] table) -- this is what keeps that always-safe rather
    // than relying on every call site remembering to check first.
    //
    // Pre-warmed against an all-zero SILENT patch, not patch_ itself at
    // velocity 0 -- Env::init()'s own attack always begins ramping toward
    // the patch's outlevel the instant init() runs regardless of
    // velocity, and per-operator velocity sensitivity is 0 in the test
    // patch above, so ScaleVelocity(0, 0) == ScaleVelocity(127, 0) == 0:
    // velocity has NO attenuating effect at all with this patch, and
    // "silent because velocity 0" was a real bug -- every pre-warmed
    // voice rang out at full volume from the moment of boot. An all-
    // zero patch's outlevel is genuinely 0 for every operator regardless
    // of velocity, so this is silent (and, via msfa's own kLevelThresh
    // gate in EngineMsfa::render(), cheap) no matter what the real patch
    // above does with velocity sensitivity.
    uint8_t silent_patch[156];
    std::memset(silent_patch, 0, sizeof(silent_patch));
    for(int i = 0; i < kMaxVoices; i++)
        voices_[i].note.init(silent_patch, 60, 0, 60, -1, &ctrls_);

    SetOutputLevel01(output_level01_);
}

void DexedSynth::SetOutputLevel01(float v01)
{
    output_level01_ = v01 < 0.f ? 0.f : (v01 > 1.f ? 1.f : v01);
    // Same curve every other engine's own Output Level uses (see
    // GranularEngine::SetOutputLevel01()), so this instrument's level
    // knob feels consistent with the rest of the mixer.
    output_level_ = powf(output_level01_, 2.5f) * 1.4f;
}

bool DexedSynth::VoiceIsIdle(int voice_index)
{
    VoiceStatus st;
    voices_[voice_index].note.peekVoiceStatus(st);
    for(int op = 0; op < 6; op++)
        if(st.ampStep[op] < 4)
            return false;
    return true;
}

int DexedSynth::FindVoiceForNote(uint8_t note)
{
    for(int i = 0; i < kMaxVoices; i++)
        if(voices_[i].held_note == (int)note)
            return i;

    int  best_free    = -1;
    bool best_is_idle = false;
    for(int i = 0; i < kMaxVoices; i++)
    {
        if(voices_[i].held_note != -1)
            continue;
        bool idle = VoiceIsIdle(i);
        if(best_free < 0 || (idle && !best_is_idle))
        {
            best_free    = i;
            best_is_idle = idle;
        }
    }
    if(best_free >= 0)
        return best_free;

    int oldest = 0;
    for(int i = 1; i < kMaxVoices; i++)
        if(voices_[i].triggered_at < voices_[oldest].triggered_at)
            oldest = i;
    return oldest;
}

void DexedSynth::NoteOn(uint8_t note, uint8_t velocity)
{
    int    vi = FindVoiceForNote(note);
    Voice& v  = voices_[vi];

    // srcnote == note, porta = -1: no portamento transition (see
    // Dx7Note::init()'s own porta handling -- a real portamento path is
    // future incremental-editing scope, not this phase's). A fresh
    // init() (not update()) every time, whether this voice was free,
    // stolen, or already sounding this exact note -- unlike DaisySP's
    // Adsr::Retrigger() convenience the removed FmSynth used, Dx7Note's
    // own init() already gives a full fresh envelope attack on its own,
    // so no separate retrigger-vs-fresh-strike branch is needed here.
    v.note.init(patch_, note, velocity, note, -1, &ctrls_);
    v.held_note    = note;
    v.triggered_at = ++trigger_seq_;
}

void DexedSynth::NoteOff(uint8_t note)
{
    for(int i = 0; i < kMaxVoices; i++)
    {
        if(voices_[i].held_note == (int)note)
        {
            voices_[i].note.keyup();
            // Freed immediately (see NoteOn()'s own free-voice-
            // preference logic) -- the voice keeps rendering its own
            // real release tail regardless (see RenderQuantum(), which
            // computes every voice unconditionally).
            voices_[i].held_note = -1;
        }
    }
}

DSY_ITCM_TEXT
void DexedSynth::RenderQuantum()
{
    std::memset(stage_i32_, 0, sizeof(stage_i32_));

    ctrls_.refresh();
    int32_t lfo_val   = lfo_.getsample();
    int32_t lfo_delay = lfo_.getdelay();

    // Every voice, unconditionally -- see the class doc comment and
    // Init()'s own comment for why this is both correct (every voice is
    // pre-init()'d, never garbage) and already cheap for a silent one
    // (msfa's own internal gain threshold collapses its real per-sample
    // cost, no bookkeeping needed here to get that).
    for(int i = 0; i < kMaxVoices; i++)
        voices_[i].note.compute(stage_i32_, lfo_val, lfo_delay, &ctrls_);

    stage_pos_ = 0;
}

DSY_ITCM_TEXT
void DexedSynth::Process(size_t size, float* out_l, float* out_r, float* reverb_send_l,
                        float* reverb_send_r)
{
    const float send = reverb_send01_;
    for(size_t i = 0; i < size; i++)
    {
        if(stage_pos_ >= kMsfaBlock)
            RenderQuantum();

        float s = (float)stage_i32_[stage_pos_] * kQ24Scale * kHeadroomScale * output_level_;
        stage_pos_++;

        // Soft limiter, on top of kHeadroomScale's own real-measured
        // correction above -- catches genuinely rare peaks beyond what
        // that fixed scale already accounts for (bigger chords, unusual
        // phase alignment) so those compress gracefully instead of
        // hard-clipping, without needing this fixed scale to chase every
        // last possible worst case on its own. tanhf() is near-
        // transparent for normal signal levels (tanhf(0.5) attenuates
        // only ~8%) and only compresses hard once a peak actually
        // approaches/exceeds unity.
        s = tanhf(s);
        out_l[i] = s;
        out_r[i] = s;
        if(send > 0.f)
        {
            reverb_send_l[i] += s * send;
            reverb_send_r[i] += s * send;
        }
    }
}
