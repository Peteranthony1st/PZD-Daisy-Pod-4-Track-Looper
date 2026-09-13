#pragma once
#include <cstdint>
#include "daisysp.h"
#include "looper_layer.h" // FilterMode, kFilterMinHz/kFilterMaxHz

// A MIDI-played, 8-voice pad synth living alongside the looper -- meant for
// warm/lush sustained sounds (chords held via a MIDI keyboard), not a
// percussive/plucked instrument. Owns its own DSP state directly as members,
// same ownership model LooperLayer already uses for its own Svf filter pair:
// a single self-contained "instrument" with its own filter/level/send, not a
// cross-cutting bus effect like main.cpp's master filter.
//
// Architecture proven cheap on real hardware before this class was written
// (see the DWT-cycle-counter measurements taken directly in main.cpp during
// this project's development): 8x daisysp::OscillatorBank (Plaits'
// "string synth"/divide-down organ oscillator -- no transcendental calls at
// all in its Process(), see oscillatorbank.cpp) each with its own
// daisysp::Adsr envelope (also pure arithmetic per sample), plus ONE shared
// daisysp::Chorus on the summed bus, cost 49% worst-case CPU together with a
// full 4-layer loop + reverb sends already playing. daisysp::StringVoice
// (Karplus-Strong) was measured and ruled out instead: a SINGLE voice alone
// cost 68% (two powf() + one atanf() every sample, unconditionally).
class PadSynth
{
  public:
    void Init(float sample_rate);

    // --- MIDI-driven transport ---------------------------------------
    void NoteOn(uint8_t note, uint8_t velocity);
    void NoteOff(uint8_t note);
    // Called once per incoming MIDI PitchBend message (control-rate, NOT
    // per sample) -- computes bend_ratio_ = powf(2, semis/12) once. This
    // is the only powf() in the whole per-block path; everything inside
    // Process() itself is pure arithmetic.
    void SetPitchBendSemis(float semis);
    float GetPitchBendSemis() const { return pitch_bend_semis_; }

    // Mod wheel (MIDI CC1) -- destination is user-assignable (see
    // ModDestination) rather than fixed, per this project's own design
    // decision. Routes ADDITIVELY on top of whichever page's own knob set
    // as the base value for FilterCutoff/ChorusDepth; Vibrato has no
    // separate "base" (a still wheel means no vibrato at all).
    enum class ModDestination
    {
        Vibrato,
        FilterCutoff,
        ChorusDepth
    };
    void            SetModDestination(ModDestination d) { mod_dest_ = d; }
    ModDestination  GetModDestination() const { return mod_dest_; }
    void            SetModWheel01(float v01);
    float           GetModWheel01() const { return mod_wheel01_; }

    // --- Tone / registration -------------------------------------------
    // Single-knob morph between a hand-tuned "dark" (mostly 8'/4') and
    // "bright" (weighted toward 2'/1') OscillatorBank registration vector
    // -- see oscillatorbank.h's SetAmplitudes() doc for what the 7 slots
    // mean. Applied to every voice identically (registration is a
    // property of the instrument's overall character, not a per-note
    // thing).
    void  SetRegistration01(float v01);
    float GetRegistration01() const { return registration01_; }
    void  SetOscGain01(float v01);
    float GetOscGain01() const { return osc_gain01_; }

    // --- Tune (coarse transpose, -24..+24 semitones) --------------------
    // Same discretized-01 convention as GranularEngine's own
    // SetGrainTuneSemitones01()/GetGrainTuneSemitones01() -- a fixed
    // multiplier on top of every voice's note-derived frequency, recomputed
    // at control-rate (like SetPitchBendSemis()'s bend_ratio_), not per
    // sample.
    void  SetTuneSemitones01(float v01);
    float GetTuneSemitones01() const;
    int   GetTuneSemitones() const { return tune_semitones_; }

    // --- Envelope --------------------------------------------------------
    // Curved-seconds convention (kMinAdsrSeconds..kMaxAdsrSeconds,
    // exponential) matching this project's existing ADSR-shape visual
    // math, so a knob's physical travel doesn't visually "do nothing" for
    // most of its range the way a raw-01-to-seconds mapping would.
    void  SetAttack01(float v01);
    void  SetDecay01(float v01);
    void  SetSustain01(float v01);
    void  SetRelease01(float v01);
    float GetAttack01() const { return attack01_; }
    float GetDecay01() const { return decay01_; }
    float GetSustain01() const { return sustain01_; }
    float GetRelease01() const { return release01_; }
    float GetAttackSeconds() const;
    float GetDecaySeconds() const;
    float GetReleaseSeconds() const;

    // --- Chorus (one shared instance on the summed voice bus) -----------
    void  SetChorusDepth01(float v01);
    void  SetChorusRate01(float v01);
    float GetChorusDepth01() const { return chorus_depth01_; }
    float GetChorusRate01() const { return chorus_rate01_; }

    // --- Vibrato (mod wheel destination Vibrato scales 0..this ceiling
    // by wheel position; see ModDestination's own comment) --------------
    void  SetVibratoDepth01(float v01) { vibrato_depth01_ = v01; }
    void  SetVibratoRate01(float v01) { vibrato_rate01_ = v01; }
    float GetVibratoDepth01() const { return vibrato_depth01_; }
    float GetVibratoRate01() const { return vibrato_rate01_; }

    // --- Bus filter (post-chorus, pre-reverb-send; independent of
    // main.cpp's master bus filter, same FilterMode/curve it uses) -------
    void       SetFilterMode(FilterMode m) { filter_mode_ = m; }
    FilterMode GetFilterMode() const { return filter_mode_; }
    void       SetFilterCutoff01(float v01) { filter_cutoff01_ = v01; }
    void       SetFilterResonance01(float v01) { filter_res01_ = v01; }
    float      GetFilterCutoff01() const { return filter_cutoff01_; }
    float      GetFilterResonance01() const { return filter_res01_; }

    // --- Mix --------------------------------------------------------------
    void  SetReverbSend01(float v01) { reverb_send01_ = v01; }
    float GetReverbSend01() const { return reverb_send01_; }
    void  SetOutputLevel01(float v01);
    float GetOutputLevel01() const { return output_level01_; }
    // Linear pan law (panL = 1-v, panR = v), same as LooperLayer::SetPan01
    // -- kept consistent so Plaits pans the same way the loop layers do at
    // the same knob position, not some "nicer" equal-power curve that
    // would make it behave differently from everything else in the mix.
    // Applied to both the dry output and the reverb send (post-pan),
    // again matching LooperLayer's own Process().
    void  SetPan01(float v01);
    float GetPan01() const { return pan01_; }

    // Renders `size` samples. WRITES (overwrites, not adds) the final
    // post-chorus/post-filter/post-output-level dry pad signal into
    // out_l/out_r -- main.cpp needs this exact signal for three separate
    // consumers (master mix, recording-input sum, its own oscilloscope
    // buffer), unlike LooperLayer::Process()'s "add directly into caller's
    // out[]" convention. ADDS its Send-scaled contribution into
    // reverb_send_l/r (those must already be zeroed for this block by the
    // caller, same as every other reverb-send contributor in main.cpp).
    void Process(size_t      size,
                 float*      out_l,
                 float*      out_r,
                 float*      reverb_send_l,
                 float*      reverb_send_r);

    // --- Visual accessors (Ui reads these; no other side effects) -------
    // Reduced from 8 -> 6 to cut Plaits' share of the worst-case CPU
    // budget (measured at ~94% with everything -- Plaits, Grains, all 4
    // loop layers, reverb -- genuinely maxed out at once) -- a direct,
    // proportional trade-off of polyphony for headroom, not a free
    // optimization. Every loop/array in this class already keys off this
    // one constant, so this is the only line that needs to change.
    static constexpr int kMaxVoices = 6;
    bool IsVoiceActive(int i) const;

    // Flat snapshot of every setting above, for the standalone preset
    // system and for embedding into a saved performance (see
    // PerformanceStore) -- one place that knows how to capture/restore
    // the whole instrument's state.
    struct PadPresetData
    {
        float registration01     = 0.5f;
        float osc_gain01         = 0.8f;
        float attack01           = 0.3f;
        float decay01            = 0.3f;
        float sustain01          = 0.8f;
        float release01          = 0.4f;
        float chorus_depth01     = 0.f;
        float chorus_rate01      = 0.3f;
        int32_t filter_mode      = (int32_t)FilterMode::Off;
        float filter_cutoff01    = 1.f;
        float filter_res01       = 0.f;
        float reverb_send01      = 0.2f;
        float output_level01     = 0.7f;
        int32_t mod_destination  = (int32_t)ModDestination::Vibrato;
        float vibrato_depth01    = 0.3f;
        float vibrato_rate01     = 0.4f;
        // Added after all existing factory presets below were written --
        // trailing field, defaulted to 0.5f (0 semitones) so every one of
        // those positional initializer lists (which only list the fields
        // that existed at the time) still gets a neutral transpose via
        // this default member initializer, with no need to touch them.
        float tune01             = 0.5f;
        // Added for the new Screen::Mixer -- same trailing-field/default-
        // member-initializer reasoning as tune01 just above (0.5f = dead
        // center, so every pre-existing factory preset stays unpanned).
        float pan01              = 0.5f;
    };
    void           ApplyPreset(const PadPresetData& p);
    PadPresetData  CapturePreset() const;

    // ~10 hand-tuned starting points, embedded in firmware (not SD files)
    // so they're always available even on a blank/unformatted card.
    // Index 0 ("New") is the neutral/default preset -- Init() applies it
    // directly, so what boots is always exactly preset 0, not a second,
    // separately-hand-coded set of defaults that could quietly drift out
    // of sync with it. Static (no instance needed) so PerformanceStore
    // can look one up without owning a PadSynth itself.
    static constexpr int kNumFactoryPresets = 14;
    static const char*   GetFactoryPresetName(int index);
    static PadPresetData GetFactoryPreset(int index);

  private:
    struct Voice
    {
        daisysp::OscillatorBank osc;
        daisysp::Adsr           adsr;
        int                     held_note   = -1;
        float                   base_hz     = 0.f;
        uint32_t                triggered_at = 0;
    };

    int FindVoiceForNote(uint8_t note);
    void ApplyRegistrationToAllVoices();
    void ApplyEnvelopeTimesToAllVoices();

    Voice    voices_[kMaxVoices];
    uint32_t trigger_seq_ = 0;

    float sample_rate_ = 48000.f;

    float          pitch_bend_semis_ = 0.f;
    float          bend_ratio_       = 1.f; // powf(2, pitch_bend_semis_/12), block-rate
    ModDestination mod_dest_         = ModDestination::Vibrato;
    float          mod_wheel01_      = 0.f;
    // Slow triangle-ish LFO driving Vibrato -- a cheap phase accumulator,
    // not a daisysp::Oscillator instance (this project already has the
    // exact same "phase_ += inc; wrap" pattern used elsewhere, e.g.
    // TempoClock, no need for a second abstraction just for this).
    float lfo_phase_ = 0.f;
    // Depth/rate are real knobs (PadParamPage::Vibrato), not a hardcoded
    // guess -- an earlier fixed 0.006 fractional-FM depth turned out to
    // be a ~0.1 semitone wobble, inaudible under a full pad texture. The
    // mod wheel still gates/scales it (0 = no vibrato regardless of
    // depth01_, 1 = the full depth01_-set amount) -- vibrato has no
    // meaningful "always-on" base the way Filter/Chorus's knobs do, so
    // this is a ceiling-and-scale relationship, not the additive one
    // those two use.
    float vibrato_depth01_ = 0.3f;
    float vibrato_rate01_  = 0.4f;
    static constexpr float kVibratoMaxDepthFraction = 0.06f; // fractional FM at depth01_==1 (~1 semitone)
    static constexpr float kVibratoMinRateHz = 0.5f;
    static constexpr float kVibratoMaxRateHz = 8.f;

    float registration01_ = 0.5f;
    float osc_gain01_     = 0.8f;

    int   tune_semitones_ = 0;
    float tune_rate_      = 1.f; // powf(2, tune_semitones_/12), control-rate like bend_ratio_

    float attack01_  = 0.3f;
    float decay01_   = 0.3f;
    float sustain01_ = 0.8f;
    float release01_ = 0.4f;
    static constexpr float kMinAdsrSeconds = 0.005f;
    static constexpr float kMaxAdsrSeconds = 3.f;

    daisysp::Chorus chorus_;
    float           chorus_depth01_ = 0.f;
    float           chorus_rate01_  = 0.3f;

    daisysp::Svf filter_l_, filter_r_;
    FilterMode   filter_mode_      = FilterMode::Off;
    float        filter_cutoff01_  = 1.f;
    float        filter_res01_     = 0.f;

    float reverb_send01_  = 0.2f;
    float output_level01_ = 0.7f;
    float output_level_   = 1.f; // curved, see SetOutputLevel01()

    float pan01_  = 0.5f;
    float pan_l_gain_ = 0.5f; // 1-pan01_, control-rate cache like tune_rate_
    float pan_r_gain_ = 0.5f; // pan01_
};
