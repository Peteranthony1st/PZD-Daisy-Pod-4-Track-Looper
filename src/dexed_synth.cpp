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
#include <cstdio>

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
//
// Even doubled, this fixed scale alone still isn't enough margin for
// the hottest real factory patches (confirmed on a real ROM1A patch
// using max feedback=7 and near-unity output levels on every operator
// -- fine alone, audibly compressing tanhf() once a second note
// stacked on top). See voice_headroom_scale_ (dexed_synth.h) for the
// per-block voice-count compensation layered on top of this fixed
// scale to handle that case without turning down single-note loudness
// on any patch.
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

    filter_l_.Init(sample_rate_);
    filter_r_.Init(sample_rate_);

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
    ApplyModWheelTarget(); // defaults to Pitch (bit0) -- same as the old hardcoded setTarget(1)
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

void DexedSynth::SetPan01(float v01)
{
    pan01_      = v01 < 0.f ? 0.f : (v01 > 1.f ? 1.f : v01);
    pan_l_gain_ = 1.f - pan01_;
    pan_r_gain_ = pan01_;
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
    //
    // Brief critical section around the actual mutation: this runs from
    // ControlTimerCallback (TIM5, a genuinely LOWER NVIC priority than
    // the audio DMA ISR -- see that function's own doc comment), and
    // Dx7Note::init() is a real multi-step write across several of this
    // voice's internal fields (env_[], basepitch_[], algorithm_, ...),
    // not an atomic flag set. RenderQuantum() (the audio ISR) reads this
    // exact same voice's state every quantum and, being higher priority,
    // can genuinely preempt this call mid-mutation -- real hardware
    // playtesting reported dropped/glitched notes specifically under
    // fast playing, consistent with landing in that window. Disabling
    // IRQs for this one voice's own init() call (a small, fixed amount
    // of work, never a loop over unbounded data) is enough to make it
    // atomic with respect to the audio ISR without meaningfully risking
    // that ISR's own timing.
    __disable_irq();
    v.note.init(patch_, note, velocity, note, -1, &ctrls_);
    v.held_note    = note;
    v.velocity     = velocity;
    v.triggered_at = ++trigger_seq_;
    __enable_irq();
}

void DexedSynth::NoteOff(uint8_t note)
{
    for(int i = 0; i < kMaxVoices; i++)
    {
        if(voices_[i].held_note == (int)note)
        {
            // Same race as NoteOn()'s own -- see its comment.
            // Dx7Note::keyup() also writes multiple operators' env_[]
            // state, not a single atomic flag.
            __disable_irq();
            voices_[i].note.keyup();
            // Freed immediately (see NoteOn()'s own free-voice-
            // preference logic) -- the voice keeps rendering its own
            // real release tail regardless (see RenderQuantum(), which
            // computes every voice unconditionally).
            voices_[i].held_note = -1;
            __enable_irq();
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
    int held = 0;
    for(int i = 0; i < kMaxVoices; i++)
    {
        voices_[i].note.compute(stage_i32_, lfo_val, lfo_delay, &ctrls_);
        if(voices_[i].held_note != -1)
            held++;
    }

    // See voice_headroom_scale_'s own comment -- a held note is untouched
    // (1.0), a stacked chord tapers down smoothly via the standard
    // "equal-power" 1/sqrt(N) curve on top of kHeadroomScale's own fixed
    // margin.
    voice_headroom_scale_ = held > 1 ? 1.f / sqrtf((float)held) : 1.f;

    stage_pos_ = 0;
}

DSY_ITCM_TEXT
void DexedSynth::Process(size_t size, float* out_l, float* out_r, float* reverb_send_l,
                        float* reverb_send_r, float* delay_send_l, float* delay_send_r)
{
    // Block-rate filter cutoff/res -- same curve/guard every other
    // engine's own bus filter uses (see GranularEngine::Process()).
    if(filter_mode_ != FilterMode::Off)
    {
        float cutoff_hz = kFilterMinHz * powf(kFilterMaxHz / kFilterMinHz, filter_cutoff01_);
        float nyquist_guard = sample_rate_ / 3.f - 1.f;
        cutoff_hz = cutoff_hz < kFilterMinHz ? kFilterMinHz
                    : cutoff_hz > nyquist_guard ? nyquist_guard
                                                : cutoff_hz;
        filter_l_.SetFreq(cutoff_hz);
        filter_l_.SetRes(filter_res01_ * 0.9f);
        filter_r_.SetFreq(cutoff_hz);
        filter_r_.SetRes(filter_res01_ * 0.9f);
    }

    const float send       = reverb_send01_;
    const float delay_send = delay_send01_;
    for(size_t i = 0; i < size; i++)
    {
        if(stage_pos_ >= kMsfaBlock)
            RenderQuantum();

        float s = (float)stage_i32_[stage_pos_] * kQ24Scale * kHeadroomScale
                  * voice_headroom_scale_;
        stage_pos_++;

        // Post-mix bus filter, same shape/position as GranularEngine's
        // own (before the soft limiter, so a resonant peak still gets
        // caught by it) -- both filter_l_/filter_r_ process the same
        // mono input, since every voice is already summed to one mono
        // signal well before this point (Pan, added later, only splits
        // this same mono value into two differently-scaled channels
        // further down -- it doesn't make the signal itself stereo).
        // Kept as a real pair rather than one shared filter instance
        // purely to match the convention every other engine here uses,
        // so genuine per-channel filtering is a smaller change later if
        // this ever does become truly stereo.
        if(filter_mode_ != FilterMode::Off)
        {
            filter_l_.Process(s);
            filter_r_.Process(s);
            switch(filter_mode_)
            {
                case FilterMode::LowPass: s = filter_l_.Low(); break;
                case FilterMode::HighPass: s = filter_l_.High(); break;
                case FilterMode::BandPass: s = filter_l_.Band(); break;
                default: break;
            }
        }

        // Soft limiter, on top of kHeadroomScale's own real-measured
        // correction above -- catches genuinely rare peaks beyond what
        // that fixed scale already accounts for (bigger chords, unusual
        // phase alignment, or a resonant filter peak) so those compress
        // gracefully instead of hard-clipping, without needing this
        // fixed scale to chase every last possible worst case on its
        // own. tanhf() is near-transparent for normal signal levels
        // (tanhf(0.5) attenuates only ~8%) and only compresses hard once
        // a peak actually approaches/exceeds unity.
        s = tanhf(s) * output_level_;
        // Pan applied last, same position GranularEngine's own Process()
        // uses (post-limiter/output-level, feeding both the dry output
        // and the reverb send) -- both channels fed the same mono s
        // pre-pan, since Dexed sums every voice to one mono signal
        // before this point (no per-voice stereo positioning).
        float sl = s * pan_l_gain_;
        float sr = s * pan_r_gain_;
        out_l[i] = sl;
        out_r[i] = sr;
        if(send > 0.f)
        {
            reverb_send_l[i] += sl * send;
            reverb_send_r[i] += sr * send;
        }
        if(delay_send > 0.f)
        {
            delay_send_l[i] += sl * delay_send;
            delay_send_r[i] += sr * delay_send;
        }
    }
}

void DexedSynth::ApplyPatchToHeldVoices()
{
    // Called from the main loop (preset load/preview, macro knobs) --
    // also a lower NVIC priority than the audio ISR, same race as
    // NoteOn()/NoteOff()'s own (see NoteOn()'s comment). One short
    // critical section per voice rather than one covering the whole
    // loop, keeping each individual disable window as small as possible.
    for(int i = 0; i < kMaxVoices; i++)
    {
        Voice& v = voices_[i];
        if(v.held_note != -1)
        {
            __disable_irq();
            v.note.update(patch_, v.held_note, v.velocity, -1, &ctrls_);
            __enable_irq();
        }
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

void DexedSynth::SetPatchByte(int byte_index, uint8_t value)
{
    if(byte_index < 0 || byte_index >= 156)
        return;
    patch_[byte_index]          = value;
    patch_baseline_[byte_index] = value;
    if(byte_index == 137) // LFO speed -- Lfo::reset() reads params[6] starting here
        lfo_.reset(&patch_[137]);
    ApplyPatchToHeldVoices();
}

void DexedSynth::ApplyModWheelTarget()
{
    uint8_t bits = mod_wheel_target_ == ModWheelTarget::Pitch    ? 1  // bit0 = pitch
                   : mod_wheel_target_ == ModWheelTarget::Amp    ? 2  // bit1 = amp
                                                                  : 4; // bit2 = eg
    ctrls_.wheel.setTarget(bits);
}

DexedSynth::DexedPresetData DexedSynth::CapturePreset() const
{
    DexedPresetData p;
    std::memcpy(p.patch, patch_, 156);
    p.reverb_send01   = reverb_send01_;
    p.output_level01  = output_level01_;
    p.filter_mode     = (int32_t)filter_mode_;
    p.filter_cutoff01 = filter_cutoff01_;
    p.filter_res01    = filter_res01_;
    p.pan01           = pan01_;
    p.delay_send01    = delay_send01_;
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

namespace
{
void UnpackTrimmedVoiceName(const uint8_t* packed_voice, char out[11])
{
    uint8_t unpacked[156];
    DexedSysex::UnpackVoice(packed_voice, unpacked);
    memcpy(out, unpacked + 145, 10);
    out[10] = '\0';
    for(int i = 9; i >= 0 && out[i] == ' '; i--)
        out[i] = '\0';
}
} // namespace

const char* DexedSynth::GetPresetDataName(const DexedPresetData& p)
{
    static char name_buf[11];
    memcpy(name_buf, p.patch + 145, 10);
    name_buf[10] = '\0';
    for(int i = 9; i >= 0 && name_buf[i] == ' '; i--)
        name_buf[i] = '\0';
    return name_buf[0] != '\0' ? name_buf : "?";
}

const char* DexedSynth::GetFactoryPresetName(int flat_index)
{
    static char                 name_buf[11];
    const DexedFactoryCategory* cat   = nullptr;
    int                          local = 0;
    if(!ResolveFactoryIndex(flat_index, &cat, &local))
        return "?";
    UnpackTrimmedVoiceName(cat->packed_data + local * 128, name_buf);

    // Real factory banks occasionally have a long run of voices someone
    // saved without ever renaming from a generic default (e.g. a whole
    // stretch of bare "BELL") -- indistinguishable from each other while
    // scrolling the preset browser (confirmed by real hardware testing:
    // "a file named bells appears the same during a large portion of the
    // K1 sweep"). Disambiguate with a running " N" suffix whenever a
    // name repeats anywhere in this category, numbering every occurrence
    // (including the first) so no two presets in the same category ever
    // display identically.
    int total_matches = 0;
    int this_rank      = 0;
    for(int i = 0; i < cat->count; i++)
    {
        char other[11];
        UnpackTrimmedVoiceName(cat->packed_data + i * 128, other);
        if(strcmp(other, name_buf) == 0)
        {
            total_matches++;
            if(i <= local)
                this_rank = total_matches;
        }
    }
    if(total_matches > 1)
    {
        char suffix[6];
        snprintf(suffix, sizeof(suffix), " %d", this_rank);
        size_t name_len   = strlen(name_buf);
        size_t suffix_len = strlen(suffix);
        if(name_len + suffix_len <= 10)
            strcat(name_buf, suffix);
    }
    return name_buf;
}
