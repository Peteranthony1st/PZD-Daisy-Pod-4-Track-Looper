#pragma once
#include <cstdint>
#include "daisysp.h"
#include "looper_layer.h" // FilterMode, kFilterMinHz/kFilterMaxHz
#include "fast_fm_voice.h"

// A MIDI-played, 8-voice FM pad instrument -- Pad Synth's sibling, built
// to replace it (mutually exclusive, see Ui::IsPadEnabled()/
// IsFmEnabled()) after real hardware measurement showed a 3-operator
// FastFmVoice per voice costs LESS CPU than PadSynth's own 6-voice
// OscillatorBank engine, while affording more voices at once (8, not 6)
// -- see fast_fm_voice.h's own doc comment for the full measurement
// story (stock daisysp::Fm2's real sinf() calls measured worse than
// PadSynth, a lookup-table 2-operator version measured well under it,
// and 8-voice/3-operator was the chosen "more voices AND still cheaper
// than Pad Synth" balance).
//
// Ownership model, page/knob conventions, and most member names are
// deliberately mirrored 1:1 from pad_synth.h so its own Ui pages/
// KnobContext/save-load plumbing carry over with minimal changes --
// see that header's own comments for the reasoning behind each shared
// piece (ModDestination routing, curved-seconds ADSR, linear pan law,
// etc.), not repeated here.
class FmSynth
{
  public:
    void Init(float sample_rate);

    // --- MIDI-driven transport (identical to PadSynth's own) -----------
    void NoteOn(uint8_t note, uint8_t velocity);
    void NoteOff(uint8_t note);
    void SetPitchBendSemis(float semis);
    float GetPitchBendSemis() const { return pitch_bend_semis_; }

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

    // --- Algorithm -- which of FastFmVoice::Algorithm's operator-routing
    // topologies is active (see that enum's own comment for each one's
    // character). Applied identically to every voice, same as every
    // other instrument-wide setting below. Stored/exposed as a plain int
    // (matching filter_mode's own int32_t convention) so it round-trips
    // through FmPresetData without depending on FastFmVoice's own enum
    // type there.
    void SetAlgorithm(int algo);
    int  GetAlgorithm() const { return (int)algorithm_; }

    // --- Operators -- Op1 is (one of, depending on Algorithm) the
    // carrier, always ratio 1:1 with the held note, not knob-controlled;
    // Op2/Op3/Op4's roles depend on the active Algorithm above (see
    // FastFmVoice::Algorithm's own comment for exactly how each one wires
    // them).
    //
    // Ratio is QUANTIZED to a small table of musically-useful values
    // (kRatioTable in fm_synth.cpp: 0.5, 1, 1.5, 2, 3, 4, 5, 6, 7, 8x),
    // not a continuous sweep -- FM only sounds harmonic/"in tune" when a
    // modulator's frequency is a clean multiple of the carrier's; any
    // ratio in between (1.37x, 2.83x...) makes the modulator
    // non-commensurate with the note, producing inharmonic, clangorous
    // overtones. A continuous knob almost never lands exactly on a clean
    // ratio, which is exactly what real FM hardware avoids by using a
    // stepped "coarse" control instead of a sweep -- same reasoning
    // applied here. SetOp2Ratio01()/etc. still take a plain 0..1 knob
    // position (quantizing it to the nearest table entry internally,
    // bucket-center convention same as every other discretizing setter in
    // this project -- see GetTuneSemitones01()'s own comment for the
    // edge-vs-center bug this avoids); GetOp2Ratio()/etc. return the
    // actual resolved multiplier for display.
    void  SetOp2Ratio01(float v01);
    float GetOp2Ratio01() const;
    float GetOp2Ratio() const { return op2_ratio_; }
    void  SetOp3Ratio01(float v01);
    float GetOp3Ratio01() const;
    float GetOp3Ratio() const { return op3_ratio_; }
    void  SetOp4Ratio01(float v01);
    float GetOp4Ratio01() const;
    float GetOp4Ratio() const { return op4_ratio_; }
    // Index is mapped linearly 0..5, matching FastFmVoice/Fm2's own
    // documented "5 = a full 2*PI radians of phase excursion" ceiling --
    // this is depth/amount, not pitch, so a continuous sweep is exactly
    // right here (no "in tune" concept applies to modulation depth).
    void  SetOp2Index01(float v01);
    float GetOp2Index01() const { return op2_index01_; }
    void  SetOp3Index01(float v01);
    float GetOp3Index01() const { return op3_index01_; }
    void  SetOp4Index01(float v01);
    float GetOp4Index01() const { return op4_index01_; }

    // --- Tune (coarse transpose, -24..+24 semitones) -- identical
    // discretized-01 convention (and bucket-center rounding) as
    // PadSynth::SetTuneSemitones01()/GetTuneSemitones01().
    void  SetTuneSemitones01(float v01);
    float GetTuneSemitones01() const;
    int   GetTuneSemitones() const { return tune_semitones_; }

    // --- Envelope (identical curved-seconds convention as PadSynth) ----
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

    // --- Vibrato (identical to PadSynth's own) --------------------------
    void  SetVibratoDepth01(float v01) { vibrato_depth01_ = v01; }
    void  SetVibratoRate01(float v01) { vibrato_rate01_ = v01; }
    float GetVibratoDepth01() const { return vibrato_depth01_; }
    float GetVibratoRate01() const { return vibrato_rate01_; }

    // --- Bus filter (post-chorus, pre-reverb-send) ----------------------
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
    // Same linear pan law as PadSynth/LooperLayer.
    void  SetPan01(float v01);
    float GetPan01() const { return pan01_; }

    // Renders `size` samples -- same WRITES-not-adds convention as
    // PadSynth::Process() (main.cpp needs this exact signal for more
    // than one consumer), same reverb-send-add convention too.
    void Process(size_t      size,
                 float*      out_l,
                 float*      out_r,
                 float*      reverb_send_l,
                 float*      reverb_send_r);

    // --- Visual accessors (Ui reads these; no other side effects) -------
    // Dropped from 8 to 6 -- real hardware measurement showed 8
    // voices x 4 operators left the main loop's blocking I2C display
    // update visibly sluggish (see fast_fm_voice.h's own ITCM comments
    // for the two real code-placement bugs already fixed along the way);
    // 6 voices keeps every operator/algorithm while cutting Fm's own
    // per-block cost by roughly a quarter, same voice-for-headroom
    // tradeoff already made once for the original 8-voice/3-operator
    // balance.
    static constexpr int kMaxVoices = 6;
    bool IsVoiceActive(int i) const;

    // Flat snapshot of every setting above, for the standalone preset
    // system and for embedding into a saved performance -- same idiom as
    // PadSynth::PadPresetData.
    struct FmPresetData
    {
        int32_t algorithm        = (int32_t)FastFmVoice::Algorithm::Stack;
        float   op2_ratio01      = 0.25f; // -> table index 2 -> 1.5x
        float   op3_ratio01      = 0.35f; // -> table index 3 -> 2x
        float   op4_ratio01      = 0.45f; // -> table index 4 -> 3x
        float   op2_index01      = 0.4f;
        float   op3_index01      = 0.3f;
        float   op4_index01      = 0.3f;
        float   attack01         = 0.2f;
        float   decay01          = 0.3f;
        float   sustain01        = 0.8f;
        float   release01        = 0.4f;
        float   chorus_depth01   = 0.f;
        float   chorus_rate01    = 0.3f;
        int32_t filter_mode      = (int32_t)FilterMode::Off;
        float   filter_cutoff01  = 1.f;
        float   filter_res01     = 0.f;
        float   reverb_send01    = 0.2f;
        float   output_level01   = 0.7f;
        int32_t mod_destination  = (int32_t)ModDestination::Vibrato;
        float   vibrato_depth01  = 0.3f;
        float   vibrato_rate01   = 0.4f;
        float   tune01           = 0.5f;
        float   pan01            = 0.5f;
    };
    void          ApplyPreset(const FmPresetData& p);
    FmPresetData  CapturePreset() const;

    // 36 hand-tuned starting points modeled on classic FM-synth patch
    // categories (electric pianos, bells, mallets, basses, brass, pads,
    // leads -- see fm_synth.cpp's own kFactoryPresetNames for the full
    // list), embedded in firmware (not SD files) so they're always
    // available even on a blank/unformatted card -- same convention as
    // PadSynth::kNumFactoryPresets/GetFactoryPreset(). Index 0 ("Init")
    // is the neutral/default preset -- Init() applies it directly, same
    // "boot state == preset 0, no separate hand-coded defaults to drift
    // out of sync" reasoning as PadSynth's own. Static (no instance
    // needed) so PerformanceStore can look one up without owning an
    // FmSynth itself. GetFactoryPreset()/GetFactoryPresetName() still
    // address these by a single flat 0..kNumFactoryPresets-1 index (same
    // as before, and what PerformanceStore's slot numbering keeps using
    // unchanged) -- the category grouping below is purely a UI browsing
    // aid layered on top, not a different storage shape.
    static constexpr int kNumFactoryPresets = 36;
    static const char*   GetFactoryPresetName(int index);
    static FmPresetData  GetFactoryPreset(int index);

    // Groups of the flat preset list above, purely for Ui's own Preset-
    // page folder browsing ("open folder" / "back", see Ui::OnButton1Short()'s
    // Screen::Fm case) -- categories are contiguous ranges of the flat
    // array (see fm_synth.cpp's own kFactoryCategories), so
    // GetFactoryCategorySlot(cat, local_index) resolves straight back to
    // the same 1-based slot number GetFactoryPreset()/LoadFmPreset() take
    // everywhere else.
    static constexpr int kNumFactoryCategories = 8;
    static const char*   GetFactoryCategoryName(int cat);
    static int           GetFactoryCategoryCount(int cat); // presets within that category
    static int           GetFactoryCategorySlot(int cat, int local_index); // -> 1-based slot

  private:
    struct Voice
    {
        FastFmVoice   fm;
        daisysp::Adsr adsr;
        int           held_note    = -1;
        float         base_hz      = 0.f;
        uint32_t      triggered_at = 0;
    };

    int  FindVoiceForNote(uint8_t note);
    void ApplyOperatorsToAllVoices();
    void ApplyEnvelopeTimesToAllVoices();
    // Recomputes just this voice's carrier frequency (and therefore, via
    // FastFmVoice::SetFrequency()'s own recompute, every operator's
    // increment against whatever ratios ApplyOperatorsToAllVoices() last
    // set) -- called on NoteOn and, only while Vibrato is the live mod
    // destination, every sample (see Process()'s own comment).
    void ApplyFrequencyToVoice(Voice& v);

    Voice    voices_[kMaxVoices];
    uint32_t trigger_seq_ = 0;

    float sample_rate_ = 48000.f;

    float          pitch_bend_semis_ = 0.f;
    float          bend_ratio_       = 1.f;
    ModDestination mod_dest_         = ModDestination::Vibrato;
    float          mod_wheel01_      = 0.f;
    float          lfo_phase_        = 0.f;
    float vibrato_depth01_ = 0.3f;
    float vibrato_rate01_  = 0.4f;
    static constexpr float kVibratoMaxDepthFraction = 0.06f;
    static constexpr float kVibratoMinRateHz = 0.5f;
    static constexpr float kVibratoMaxRateHz = 8.f;

    // Index into kRatioTable (fm_synth.cpp) -- see SetOp2Ratio01()'s own
    // doc comment for why this is a table lookup, not a continuous value.
    int   op2_ratio_idx_ = 2;
    float op2_ratio_     = 1.5f; // kRatioTable[2]
    int   op3_ratio_idx_ = 3;
    float op3_ratio_     = 2.f; // kRatioTable[3]
    int   op4_ratio_idx_ = 4;
    float op4_ratio_     = 3.f; // kRatioTable[4]
    float op2_index01_ = 0.4f, op3_index01_ = 0.3f, op4_index01_ = 0.3f;
    static constexpr float kMaxIndex = 5.f; // see SetOp2Index01()'s own comment
    FastFmVoice::Algorithm algorithm_ = FastFmVoice::Algorithm::Stack;

    int   tune_semitones_ = 0;
    float tune_rate_      = 1.f;

    float attack01_  = 0.2f;
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
    float output_level_   = 1.f;

    float pan01_  = 0.5f;
    float pan_l_gain_ = 0.5f;
    float pan_r_gain_ = 0.5f;
};
