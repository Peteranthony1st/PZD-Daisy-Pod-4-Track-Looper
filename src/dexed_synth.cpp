#include "dexed_synth.h"
#include "itcm.h"
#include "dexed_sysex.h"
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
// point FM engine like this needs). Measured directly on real hardware
// (temporary peak-diagnostic instrumentation, since removed) against
// this project's own worst-case 6-fully-additive-carrier test patch: a
// single note peaked at ~1.74x this naive Q24-only scale, a 3-note
// chord ~4.41x.
//
// An initial kHeadroomScale of 1/4.41 (landing that 3-note chord right
// at the edge of unity) turned out to leave no real margin once real
// factory patches (this project's own hand-tuned test patch was, if
// anything, unusually mild -- many real patches use feedback, which
// self-modulates far harder) and real polyphonic chords beyond 3 notes
// entered the picture: tanhf()'s own soft limiter in Process() was
// routinely engaging hard enough to audibly compress/distort a plain
// single real patch playing alone, not just catching rare outlier
// peaks the way it's meant to. Doubled again here for real margin --
// tanhf() should stay a rare safety net for genuine outliers (bigger
// chords, unusual phase alignment, hotter-than-average patches), not
// something routine playing runs into; overall loudness is what the
// master volume knob (already reaching up to 1.43x at 100%) is for.
constexpr float kQ24Scale      = 1.f / (float)(1 << 24);
constexpr float kHeadroomScale = 1.f / 8.82f;
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

    // LFO pitch mod SENSITIVITY note: despite the name, patch byte 143
    // is the note's overall susceptibility to ANY pitch modulation
    // source (both the LFO's own depth AND the mod wheel's own
    // contribution via ctrls_.wheel above -- see Dx7Note::compute()'s
    // senslfo, which gates both through the same multiply) -- a patch
    // with this at 0 will not respond to the mod wheel at all
    // regardless of ctrls_.wheel's own routing, same as every other
    // real DX7 patch parameter (nothing special is done here to force
    // it on for factory/user patches that were authored with it off).

    // Boot default: real factory preset 0 (first patch in the first
    // category), same "always exactly factory preset 0, no separately
    // hand-coded default to drift out of sync" convention the removed
    // FmSynth::Init() used. lfo_.reset() happens inside ApplyPreset()'s
    // own SetPatch() call below.
    ApplyPreset(GetFactoryPreset(0));

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
    v.velocity     = velocity;
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

void DexedSynth::ApplyPatchToHeldVoices()
{
    for(int i = 0; i < kMaxVoices; i++)
    {
        Voice& v = voices_[i];
        if(v.held_note != -1)
            v.note.update(patch_, v.held_note, v.velocity, -1, &ctrls_);
    }
}

void DexedSynth::SetPatch(const uint8_t new_patch[156], float reverb_send01, float output_level01)
{
    std::memcpy(patch_, new_patch, 156);
    std::memcpy(patch_baseline_, new_patch, 156);
    brightness01_ = 0.5f; // re-center -- "as stored" for the freshly loaded patch
    env_speed01_  = 0.5f;
    lfo_.reset(&patch_[137]);
    SetReverbSend01(reverb_send01);
    SetOutputLevel01(output_level01);
    ApplyPatchToHeldVoices();
}

void DexedSynth::SetBrightness01(float v01)
{
    brightness01_ = v01 < 0.f ? 0.f : (v01 > 1.f ? 1.f : v01);
    // 0x at v01=0, 1x (unchanged) at v01=0.5, 2x at v01=1.
    float scale = brightness01_ >= 0.5f ? 1.f + (brightness01_ - 0.5f) * 2.f
                                          : brightness01_ * 2.f;

    uint8_t carrier_mask = FmCore::get_carrier_operators(patch_baseline_[134]);
    for(int op = 0; op < 6; op++)
    {
        if(carrier_mask & (1 << op))
            continue; // carriers untouched -- only modulators shape brightness
        int off        = op * 21;
        int base_level = patch_baseline_[off + 16];
        int new_level  = (int)((float)base_level * scale + 0.5f);
        new_level      = new_level < 0 ? 0 : (new_level > 99 ? 99 : new_level);
        patch_[off + 16] = (uint8_t)new_level;
    }
    ApplyPatchToHeldVoices();
}

void DexedSynth::SetEnvSpeed01(float v01)
{
    env_speed01_ = v01 < 0.f ? 0.f : (v01 > 1.f ? 1.f : v01);
    float scale  = env_speed01_ >= 0.5f ? 1.f + (env_speed01_ - 0.5f) * 2.f : env_speed01_ * 2.f;

    for(int op = 0; op < 6; op++)
    {
        int off = op * 21;
        for(int r = 0; r < 4; r++)
        {
            int base_rate = patch_baseline_[off + r];
            int new_rate  = (int)((float)base_rate * scale + 0.5f);
            new_rate      = new_rate < 0 ? 0 : (new_rate > 99 ? 99 : new_rate);
            patch_[off + r] = (uint8_t)new_rate;
        }
    }
    ApplyPatchToHeldVoices();
}

DexedSynth::DexedPresetData DexedSynth::CapturePreset() const
{
    DexedPresetData p;
    std::memcpy(p.patch, patch_, 156);
    p.reverb_send01  = reverb_send01_;
    p.output_level01 = output_level01_;
    return p;
}

namespace
{
// Resolves a flat 0-based factory preset index to its category and the
// voice index within it -- mirrors the inverse of the removed
// FmSynth::GetFactoryCategorySlot()'s own "walk category counts" idiom.
bool ResolveFactoryIndex(int flat_index, const DexedFactoryCategory** out_cat, int* out_local)
{
    if(flat_index < 0)
        return false;
    for(int c = 0; c < kDexedNumFactoryCategories; c++)
    {
        if(flat_index < kDexedFactoryCategories[c].count)
        {
            *out_cat   = &kDexedFactoryCategories[c];
            *out_local = flat_index;
            return true;
        }
        flat_index -= kDexedFactoryCategories[c].count;
    }
    return false;
}
} // namespace

int DexedSynth::GetNumFactoryPresets()
{
    int total = 0;
    for(int c = 0; c < kDexedNumFactoryCategories; c++)
        total += kDexedFactoryCategories[c].count;
    return total;
}

const char* DexedSynth::GetFactoryCategoryName(int cat)
{
    if(cat < 0 || cat >= kDexedNumFactoryCategories)
        return "?";
    return kDexedFactoryCategories[cat].name;
}

int DexedSynth::GetFactoryCategoryCount(int cat)
{
    if(cat < 0 || cat >= kDexedNumFactoryCategories)
        return 0;
    return kDexedFactoryCategories[cat].count;
}

int DexedSynth::GetFactoryCategorySlot(int cat, int local_index)
{
    if(cat < 0 || cat >= kDexedNumFactoryCategories)
        return -1;
    if(local_index < 0 || local_index >= kDexedFactoryCategories[cat].count)
        return -1;
    int start = 0;
    for(int c = 0; c < cat; c++)
        start += kDexedFactoryCategories[c].count;
    return start + local_index + 1; // slots are 1-based
}

DexedSynth::DexedPresetData DexedSynth::GetFactoryPreset(int flat_index)
{
    DexedPresetData              p;
    const DexedFactoryCategory* cat = nullptr;
    int                          local = 0;
    if(ResolveFactoryIndex(flat_index, &cat, &local))
        DexedSysex::UnpackVoice(cat->packed_data + local * 128, p.patch);
    return p;
}

const char* DexedSynth::GetFactoryPresetName(int flat_index)
{
    static char                 name_buf[11];
    const DexedFactoryCategory* cat   = nullptr;
    int                          local = 0;
    if(!ResolveFactoryIndex(flat_index, &cat, &local))
        return "?";
    uint8_t unpacked[156];
    DexedSysex::UnpackVoice(cat->packed_data + local * 128, unpacked);
    std::memcpy(name_buf, unpacked + 145, 10);
    name_buf[10] = '\0';
    for(int i = 9; i >= 0 && name_buf[i] == ' '; i--)
        name_buf[i] = '\0';
    return name_buf;
}
