#pragma once
#include <cstdint>
#include <cstddef>
#include "daisysp.h"
#include "looper_layer.h" // FilterMode, kFilterMinHz/kFilterMaxHz
#include "msfa/dx7note.h"
#include "msfa/controllers.h"
#include "msfa/EngineMsfa.h"
#include "msfa/fm_core.h"
#include "msfa/lfo.h"
#include "dexed_factory_data.h"

// DX7/msfa port (see the approved plan). Phase 2 measured real per-voice
// CPU cost on hardware (1/2/8/10 simultaneous voices, ~7% fixed + ~2.3%/
// voice, no cache-thrashing surprise even at 10) before committing to a
// voice count -- 10 was chosen from that real measurement (~76%
// estimated combined worst case alongside the looper's own real worst
// case), matching this project's own established "measure before
// committing" convention. Phase 4 wires real MIDI dispatch/engine-select
// gating; Phase 5 adds UI. Still exactly one hardcoded test patch
// (Phase 6 adds real preset save/load, Phase 7 real SysEx import).
//
// Owns Dx7Note instances directly (msfa's real Apache-2.0 note engine,
// see msfa/dx7note.h) -- everything else in this class is original code
// written against that engine's public interface, not ported from the
// original Dexed plugin's own (GPLv3, excluded) voice-pool/SysEx layer.
// The oldest-note-steal allocation below mirrors the SHAPE of the
// removed FmSynth::FindVoiceForNote()'s own original logic (same
// project, no license concern -- not from any GPL'd source), adapted to
// Dx7Note's own envelope-position query (VoiceStatus::ampStep) in place
// of DaisySP's Adsr::GetCurrentSegment().
class DexedSynth
{
  public:
    void Init(float sample_rate);

    // Polyphonic voice allocation: retriggers the same voice if `note`
    // is already held there; otherwise claims a free voice (preferring
    // one that's fully released/silent over one still ringing out from
    // a recent NoteOff -- see VoiceIsIdle()); otherwise steals the
    // oldest-triggered voice outright. NoteOff() marks a voice free
    // immediately (so it's eligible for reuse right away) but leaves it
    // rendering its own real release tail -- see RenderQuantum(), which
    // always computes every voice unconditionally: msfa's own internal
    // per-operator gain threshold (see EngineMsfa::render()) already
    // collapses a fully decayed voice's real per-sample cost to nearly
    // nothing, so no extra bookkeeping is needed here to get that for
    // free.
    void NoteOn(uint8_t note, uint8_t velocity);
    void NoteOff(uint8_t note);

    // Raw 14-bit MIDI pitch bend (0..16383, 8192 = centered) -- this is
    // exactly the CC value Dx7Note::compute() itself expects (see
    // Controllers::values_[kControllerPitch] there), unlike a simplified
    // "bend amount in semitones" input (what the removed FastFmVoice
    // used): msfa's own pitch-bend math already combines this raw value
    // with a separate bend-range-in-semitones setting (hardcoded to +-2
    // in Init() for now -- a real Global/Dexed page control is future
    // incremental-editing scope, not this phase's), so re-deriving a
    // semitone offset here would just be undone downstream.
    void SetPitchBend14bit(uint16_t raw14) { ctrls_.values_[kControllerPitch] = raw14; }
    // 0..1, same convention as every other engine's own mod-wheel-style
    // input in this project.
    void SetModWheel01(float v01)
    {
        v01 = v01 < 0.f ? 0.f : (v01 > 1.f ? 1.f : v01);
        ctrls_.modwheel_cc = (uint8_t)(v01 * 127.f + 0.5f);
    }

    // Same WRITES-not-adds / reverb-send-ADDS convention as every other
    // engine's own Process() in this project (see the removed FmSynth::
    // Process()'s doc comment) -- main.cpp needs this exact out_l/out_r
    // signal for more than one consumer (dry mix + the master scope).
    //
    // Internally decouples this project's 48-sample audio block from
    // msfa's native 64-sample rendering quantum (_N_ in msfa/synth.h,
    // not a multiple of 48) via a small staging buffer -- see
    // dexed_synth.cpp's own comment.
    void Process(size_t size, float* out_l, float* out_r, float* reverb_send_l,
                 float* reverb_send_r);

    void  SetReverbSend01(float v01) { reverb_send01_ = v01; }
    float GetReverbSend01() const { return reverb_send01_; }
    void  SetOutputLevel01(float v01);
    float GetOutputLevel01() const { return output_level01_; }

    // Post-mix bus filter, same shape as GranularEngine's own (a plain
    // Svf pair, not part of the real DX7's own signal path -- this
    // project's own addition, consistent with every other engine here
    // having one).
    void       SetFilterMode(FilterMode m) { filter_mode_ = m; }
    FilterMode GetFilterMode() const { return filter_mode_; }
    void       SetFilterCutoff01(float v01) { filter_cutoff01_ = v01; }
    float      GetFilterCutoff01() const { return filter_cutoff01_; }
    void       SetFilterResonance01(float v01) { filter_res01_ = v01; }
    float      GetFilterResonance01() const { return filter_res01_; }

    // Bipolar macro knobs, centered at "as stored in the preset" (0.5),
    // matching the soft-pickup convention every other knob in this
    // project uses -- both scale linearly from 0x at v01=0 to 1x
    // (unchanged) at v01=0.5 to 2x at v01=1. Directly mutate patch_
    // (relative to patch_baseline_, the pristine values captured at the
    // last SetPatch() call) and push the result to any currently-held
    // voice via Dx7Note::update() (not init()), so turning either while
    // holding a note doesn't click or retrigger it. Saving a preset
    // captures patch_ as-is -- these are real, persistent edits, not a
    // transient overlay that resets on save (matches how every other
    // knob/preset in this project already works).
    //
    // Brightness scales the output level of every operator NOT in
    // FmCore::get_carrier_operators(patch_baseline_[134])'s mask
    // (i.e. every modulator, for whichever of the 32 algorithms is
    // currently loaded) -- carriers are untouched, since boosting a
    // carrier only changes volume, not timbre.
    void  SetBrightness01(float v01);
    float GetBrightness01() const { return brightness01_; }
    // Scales all 6 operators' 4 EG rates together -- does NOT touch the
    // pitch EG (Dx7Note::update() never re-applies patch bytes 126-133
    // regardless, see dexed_synth.cpp's own comment).
    void  SetEnvSpeed01(float v01);
    float GetEnvSpeed01() const { return env_speed01_; }

    // Direct single-byte patch edit -- writes both patch_ and
    // patch_baseline_ (so Brightness/EnvSpeed's own baseline stays
    // consistent with whatever's actually playing) and pushes the
    // change to any currently-held voice via Dx7Note::update(), same
    // click-free mechanism as SetPatch(). byte_index is a raw offset
    // into the 156-byte unpacked layout (see dexed_synth.cpp's own
    // comment for the full byte map) -- deliberately low-level rather
    // than one named setter per parameter, since the UI needs this same
    // shape for several independent single-byte fields (algorithm,
    // feedback, LFO speed/depth) and a named setter per one would just
    // be boilerplate.
    void    SetPatchByte(int byte_index, uint8_t value);
    uint8_t GetPatchByte(int byte_index) const { return patch_[byte_index]; }

    // A flat POD snapshot of the currently-loaded sound -- everything
    // needed to reproduce it via ApplyPreset(), same "preset != session
    // mix" split GranularEngine::GranularPresetData/the removed
    // FmSynth::FmPresetData already use in this project.
    struct DexedPresetData
    {
        uint8_t patch[156]  = {};
        float   reverb_send01  = 0.2f;
        // 0.6 (not 1.0) so a freshly-loaded factory/new preset lands at a
        // safe, reasonable level by default -- factory presets never set
        // this themselves (GetFactoryPreset() only fills in patch[]), so
        // this default is literally what every one of the 700+ factory
        // presets loads at.
        float   output_level01 = 0.6f;
        int32_t filter_mode     = (int32_t)FilterMode::Off;
        float   filter_cutoff01 = 1.f;
        float   filter_res01    = 0.f;
    };
    void ApplyPreset(const DexedPresetData& p)
    {
        SetPatch(p.patch, p.reverb_send01, p.output_level01);
        SetFilterMode((FilterMode)p.filter_mode);
        SetFilterCutoff01(p.filter_cutoff01);
        SetFilterResonance01(p.filter_res01);
    }
    DexedPresetData CapturePreset() const;

    // Factory presets: real DX7 patches, embedded from freely-
    // distributed real SysEx bank data (see dexed_factory_data.h/.cpp's
    // own doc comment for the source) -- organized into named
    // categories by real sound type (Synth/Piano/E.Piano/Bass/Strings/
    // Woodwind/Brass/Organ/Percussion/Choir/Bells), addressed by a flat
    // 0-based index resolved against that category table. Same
    // {name,count}-contiguous-range shape the removed FmSynth used for
    // its own (much smaller) factory bank, generalized to more/larger
    // categories. All `static` -- no instance needed, matching FmSynth's
    // own convention (PerformanceStore calls these directly).
    static int             GetNumFactoryPresets();
    static const char*     GetFactoryCategoryName(int cat);
    static int              GetFactoryCategoryCount(int cat);
    static int              GetFactoryCategorySlot(int cat, int local_index); // -> flat 1-based slot
    static DexedPresetData  GetFactoryPreset(int flat_index); // 0-based; unpacks on demand
    static const char*      GetFactoryPresetName(int flat_index); // reads the patch's own real name bytes
    static constexpr int kNumFactoryCategories = kDexedNumFactoryCategories;

    // The real, committed voice count -- chosen from Phase 2's own
    // hardware CPU measurement (see the class doc comment above), not a
    // guess or a value carried over from any other engine.
    static constexpr int kMaxVoices = 10;

  private:
    void RenderQuantum(); // fills stage_i32_ with one fresh 64-sample msfa quantum

    // Real oldest-note-steal allocation -- see NoteOn()'s own doc
    // comment for the 3-tier shape (retrigger / free / steal).
    int  FindVoiceForNote(uint8_t note);
    // True once every operator's envelope has fully reached its final
    // rest level (VoiceStatus::ampStep -- see msfa/env.cpp's own ix_
    // semantics: ix_ >= 4 means the envelope is no longer advancing at
    // all), i.e. genuinely safe to reassign with no audible cut -- vs.
    // merely "released" (NoteOff already called, but still ringing out).
    bool VoiceIsIdle(int voice_index);

    // Copies new_patch into patch_ and patch_baseline_ (the reference
    // point SetBrightness01()/SetEnvSpeed01() scale relative to,
    // re-centering both macros back to 0.5), then pushes it to every
    // currently-held voice via Dx7Note::update() (not init()) so
    // switching/previewing a preset doesn't retrigger or click an
    // already-held note. Used by ApplyPreset() and (in a later phase)
    // the Preset page's load/preview flow.
    void SetPatch(const uint8_t new_patch[156], float reverb_send01, float output_level01);
    // Shared by SetPatch()/SetBrightness01()/SetEnvSpeed01() -- pushes
    // the current patch_ to every voice with held_note != -1 via
    // Dx7Note::update(), using that voice's own cached velocity.
    void ApplyPatchToHeldVoices();

    struct Voice
    {
        Dx7Note note;
        // -1 = free (eligible for reuse -- see NoteOff(), which frees a
        // voice immediately rather than waiting for its release tail to
        // finish). Otherwise the MIDI note currently assigned here.
        int      held_note   = -1;
        // Cached at NoteOn() time -- Dx7Note::update() (unlike init())
        // still needs a velocity to recompute ScaleVelocity()-based
        // output level, but doesn't take one as an already-tracked
        // implicit voice property the way real hardware would.
        uint8_t  velocity     = 100;
        uint32_t triggered_at = 0;
    };
    Voice       voices_[kMaxVoices];
    // Monotonic, so "oldest" is always a plain min() over this -- same
    // idiom the removed FmSynth used for its own oldest-note-steal.
    uint32_t    trigger_seq_ = 0;
    Controllers ctrls_;
    EngineMsfa  engine_;
    // ONE shared LFO, not per-voice -- matches real DX7 hardware (the
    // LFO is global across every simultaneously-sounding voice, not an
    // independent oscillator per note).
    Lfo     lfo_;
    uint8_t patch_[156];
    // Pristine copy of patch_ as of the last SetPatch() call -- the
    // reference point Brightness/EnvSpeed scale relative to (so
    // repeatedly nudging a macro knob back and forth doesn't compound
    // rounding drift against its own previous output).
    uint8_t patch_baseline_[156] = {};
    // Centered at 0.5 ("as stored") -- see SetBrightness01()/
    // SetEnvSpeed01()'s own doc comment for the bipolar scale shape.
    float brightness01_ = 0.5f;
    float env_speed01_  = 0.5f;

    float sample_rate_     = 48000.f;
    float reverb_send01_   = 0.f;
    // Same default every other engine in this project uses -- real
    // headroom safety against the additive test patch's occasional
    // constructive-interference peaks comes from Process()'s own soft
    // limiter now, not from permanently quieting this default (a
    // genuine reduction here just made real playing too quiet without
    // actually fixing the clipping, which was really a peak-headroom
    // problem, not an average-loudness one).
    float output_level01_  = 0.8f;
    float output_level_    = 1.f; // powf(output_level01_, 2.5f)*1.4f, cached by the setter

    daisysp::Svf filter_l_, filter_r_;
    FilterMode   filter_mode_     = FilterMode::Off;
    float        filter_cutoff01_ = 1.f;
    float        filter_res01_    = 0.f;

    // msfa's native quantum -- Dx7Note::compute()/EngineMsfa::render()
    // always produce exactly this many samples per call (hardcoded via
    // _N_ throughout fm_op_kernel.cpp/env.cpp, see msfa/synth.h).
    static constexpr int kMsfaBlock = 64;
    int32_t stage_i32_[kMsfaBlock];
    // Index of the next not-yet-drained sample in stage_i32_ -- starts
    // at kMsfaBlock so the very first Process() call renders a fresh
    // quantum immediately rather than reading uninitialized data.
    int stage_pos_ = kMsfaBlock;
};
