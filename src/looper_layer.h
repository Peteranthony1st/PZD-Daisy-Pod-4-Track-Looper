#pragma once
#include "daisy_seed.h"
#include "daisysp.h"
#include "tempo_clock.h"

using namespace daisy;

// One stereo loop track: recording/playback state machine, per-layer
// filter, and one selectable "character" effect.
//
// Design notes (read this before changing the state machine):
//
//  - LooperLayer no longer reads any hardware directly (no Switch*,
//    no DaisySeed* for ADC reads). All button/knob handling lives in the
//    UI layer (ui.h/.cpp) and main.cpp, which call the small, explicit
//    API below. This keeps the DSP testable on a desktop compiler and
//    keeps "what does this button do right now" entirely a UI concern,
//    which matters a lot now that both buttons and both knobs change
//    meaning depending on the on-screen menu.
//
//  - All 5 layers share ONE master loop length (TempoClock::
//    GetLoopLengthSamples()), captured at the moment a layer starts
//    recording. That's what keeps every layer's downbeat lined up.
//    TempoClock::Lock() should be called by main.cpp the first time any
//    layer finishes recording, so BPM/Bars can't drift out from under
//    already-recorded layers; Unlock() once every layer is cleared.
//
//  - Effects are applied on the PLAYBACK path only, never baked into
//    the recorded buffer. That means dialing in a filter sweep or
//    switching effect type never destroys your take, and it means
//    overdubbing captures clean dry material every time (effects would
//    otherwise stack irreversibly take after take).
//
//  - Signal chain per layer, applied to the looped-back sample before
//    it's mixed to the bus: Filter -> character effect -> volume/pan
//    -> this layer's own reverb (send amount + size both per-layer, see
//    SetReverbSend01()/SetReverbSize01()) -- each layer owns an
//    independent ReverbSc instance (externally owned in SDRAM, like
//    fx_phaser_/fx_pitchshift_), not a shared bus.

enum class LayerState
{
    Empty,
    ArmedCountIn, // waiting out the metronome's count-in before recording
    Recording,
    Playing,
    Paused,
    Overdubbing
};

enum class FilterMode
{
    Off,
    LowPass,
    HighPass,
    BandPass,
    kNumModes
};

// Shared filter-cutoff curve (log-ish taper) -- used by both the
// per-layer filters below and main.cpp's master-bus filter, so both
// feel consistent. Public (not in looper_layer.cpp's anonymous
// namespace) specifically so main.cpp can reuse the exact same curve.
constexpr float kFilterMinHz = 20.f;
constexpr float kFilterMaxHz = 9000.f;

enum class LayerEffect
{
    Off,
    Drive,
    Bitcrush,
    Chorus,
    Tremolo,
    Phaser,
    AutoWah,
    Flanger,
    kNumEffects
};

struct LooperLayer
{
    // --- Setup -----------------------------------------------------
    // fx_phaser/fx_pitchshift/fx_reverb are externally-owned (must
    // outlive this object) and must already be default-constructed but
    // NOT yet Init()'d -- this call does that. They're pointers rather
    // than direct members because they're too big to live in a
    // LooperLayer array in ordinary SRAM: Phaser is ~75KB, PitchShifter
    // ~128KB, and ReverbSc ~386KB *per instance*, so main.cpp places
    // one of each per layer in SDRAM (like buffer_l/buffer_r) and hands
    // in the pointer. Every other effect is small enough to live
    // directly as a member below.
    void Init(float*             buf_l,
              float*             buf_r,
              size_t             buffer_size,
              float              sample_rate,
              daisysp::Phaser*       fx_phaser,
              daisysp::PitchShifter* fx_pitchshift,
              daisysp::ReverbSc*     fx_reverb);
    void Reset(); // hard reset to Empty, clears controls to defaults

    // --- Transport (call from the UI layer on button edges) --------
    // Short press: Empty->(nothing, use long press) / Playing<->Paused /
    //              Overdubbing is ended by releasing, not by a short press.
    void OnRecordButtonPressed(TempoClock& tempo);
    // Long press (~400ms held): Empty -> arm to record (via tempo's
    // count-in, or immediately if the metronome is off); Playing or
    // Paused -> start Overdubbing.
    void OnRecordButtonLongPress(TempoClock& tempo);
    // Release after a long-press-triggered overdub ends it.
    void OnRecordButtonReleased();
    // Wipes this layer back to Empty. UI is responsible for requiring a
    // deliberate hold-to-confirm gesture before calling this.
    void Clear();

    LayerState GetState() const { return state_; }
    bool       HasContent() const { return record_len_ > 0; }
    size_t     GetRecordedLength() const { return record_len_; } // samples, for save

    // --- Continuous controls (0..1 unless noted), set every control
    // tick from whichever knob is currently mapped to them -------------
    void SetVolume01(float v);
    float GetVolume01() const { return volume01_; } // raw 0..1, for save/restore
    void SetPan01(float v);
    float GetPan01() const { return pan_; } // already raw 0..1
    void SetSpeed01(float v); // 0..1, deadzone-centered -> 0.3x..2.0x, 1.0x at center
    float GetSpeed() const { return speed_; } // for the Speed page's live readout
    float GetSpeed01() const { return speed01_; } // raw 0..1, for save/restore

    // Input gain applied to both channels at record/overdub time (not
    // just at playback like Volume) -- 1x..4x (0..+12dB), for sources
    // that come in quiet with no analog preamp available (see
    // DESIGN.md's input front-end notes: the Pod's jacks are plain
    // line-level with no mic/Hi-Z gain stage).
    void  SetInputGain01(float v);
    float GetInputGain() const { return input_gain_; } // the actual multiplier, for display
    float GetInputGain01() const { return input_gain01_; } // raw 0..1, for save/restore

    void SetFilterMode(FilterMode m) { filter_mode_ = m; }
    FilterMode GetFilterMode() const { return filter_mode_; }
    void SetFilterCutoff01(float v);   // -> ~80Hz..9kHz (log-ish taper)
    float GetFilterCutoff01() const { return filter_cutoff01_; }
    void SetFilterResonance01(float v); // -> 0..0.9 (kept shy of self-osc)
    float GetFilterResonance01() const { return filter_res01_; }

    // Switching effects resets a short fade-in (see Process()) so the
    // new effect's cold internal state (e.g. PitchShifter's delay line
    // starting silent) doesn't produce an audible pop/glitch on the
    // instant of the switch.
    void SetEffect(LayerEffect e);
    LayerEffect GetEffect() const { return effect_; }
    void SetEffectParamA01(float v); // meaning depends on `effect_`
    float GetEffectParamA01() const { return effect_param_a_; }
    void SetEffectParamB01(float v);
    float GetEffectParamB01() const { return effect_param_b_; }

    // This layer's own independent reverb (see fx_reverb_ and Init()).
    // Send is how much of this layer's (post-filter/effect, post-pan)
    // signal feeds it; Size is that reverb instance's feedback/decay.
    void  SetReverbSend01(float v);
    float GetReverbSend01() const { return reverb_send_; }
    void  SetReverbSize01(float v);
    float GetReverbSize01() const { return reverb_size01_; }

    // Pitch shift, independent of (and can run alongside) the character
    // effect above -- unlike SetEffect()'s mutually-exclusive slot, this
    // is a real on/off toggle, same idea as FilterMode::Off: while
    // disabled, the knob values don't matter, playback is untouched.
    // Amount is 0..1 -> -12..+12 semitones, unison at the knob's center
    // (0.5) -- see Process() for why 0 can't mean "off" for this one.
    void  SetPitchEnabled(bool on);
    bool  GetPitchEnabled() const { return pitch_enabled_; }
    void  SetPitchAmount01(float v);
    float GetPitchAmount01() const { return pitch_amount01_; }
    void  SetPitchFun01(float v);
    float GetPitchFun01() const { return pitch_fun01_; }

    // DaisySP's PitchShifter is a time-domain shifter built on a delay
    // line, and its default size (16384 samples, ~341ms at 48kHz) is a
    // real, audible processing latency -- the pitched signal genuinely
    // lags the loop position by that much, which reads as "out of sync".
    // Smaller sizes cut that latency but raise the internal crossfade
    // rate for the same pitch amount, which shows up as more audible
    // warble on bigger shifts -- this is a real trade-off, not a free
    // fix, hence 3 selectable presets rather than just picking one.
    static constexpr int kNumPitchDelayPresets = 3; // Fast / Med / Smooth
    void SetPitchDelayPreset(int preset); // clamped to 0..kNumPitchDelayPresets-1
    int  GetPitchDelayPreset() const { return pitch_delay_preset_; }

    // Cheap 0..1 read-back for VU-style meters on the OLED.
    float GetLevel() const { return meter_; }

    // Downsampled waveform for the Status page display: max abs(left
    // channel) seen in each of kWaveformCols buckets spanning the
    // recorded length, updated incrementally per-sample during
    // Recording/Overdubbing (see Process()) rather than rescanned from
    // the buffer on every redraw -- buffers can hold up to ~33s of
    // audio, so a full rescan every ~30Hz UI redraw would be wasteful.
    // Left channel only, same convention meter_ already uses.
    static constexpr int kWaveformCols = 63; // 1px bar + 1px gap fits 125 of the display's 126 usable px
    const float* GetWaveformPeaks() const { return waveform_peaks_; }
    // Normalized 0..1 playhead position, for a moving cursor over the
    // waveform. Same lockless cross-thread read as GetLevel()/GetState()
    // above -- see the real-time-rule comment in ui.h for why that's
    // fine for this kind of "slow" data.
    float GetPlayPos01() const
    {
        return record_len_ > 0 ? play_pos_ / (float)record_len_ : 0.f;
    }

    // Raw play_pos_ in samples (not normalized). Export-only: lets
    // PerformanceStore::ExportWav() snapshot the real position, force it
    // to 0 (the true loop downbeat) for a clean render, then restore it
    // afterward -- not intended for general use.
    float GetPlayPosRaw() const { return play_pos_; }
    void  SetPlayPosRaw(float p) { play_pos_ = p; }

    // --- Save/load (see performance_store.h) ------------------------
    // Raw buffer access + a way to declare a buffer "recorded" without
    // going through the normal record transport -- used only when
    // loading a saved performance's audio back from the SD card.
    // Non-const because PerformanceStore::Load() reads sample data
    // directly off the SD card into these buffers with f_read(), which
    // needs a plain float* (no layer-side copy/staging buffer -- the
    // loop buffers are already exactly the right size and location).
    float* GetBufferL() { return buffer_l_; }
    float* GetBufferR() { return buffer_r_; }
    size_t GetBufferSize() const { return buffer_size_; }
    // Sets record_len_ and switches to Playing (or Empty if len==0),
    // for after PerformanceStore::Load() has filled the buffers itself.
    void RestoreRecordedLength(size_t len);

    // --- Audio ------------------------------------------------------
    // ticks[i] must be the SAME TempoClock tick array shared by every
    // layer this block (computed once per block in main.cpp, not once
    // per layer -- see tempo_clock.h).
    void Process(AudioHandle::InputBuffer  in,
                 AudioHandle::OutputBuffer out,
                 size_t                    size,
                 const TempoClock::TempoTick* ticks,
                 TempoClock&                  tempo);

  private:
    void ProcessEffectsChain(float dry_l, float dry_r, float& out_l, float& out_r);

    float* buffer_l_ = nullptr;
    float* buffer_r_ = nullptr;
    size_t buffer_size_ = 0;
    float  sample_rate_ = 48000.f;

    LayerState state_ = LayerState::Empty;

    size_t write_idx_   = 0;
    size_t record_len_  = 0; // fixed once recording finishes
    size_t target_len_  = 0; // this recording's target length (from tempo)
    float  play_pos_    = 0.f;
    float  input_gain_  = 1.f; // multiplier applied at record/overdub time
    float  input_gain01_ = 0.f; // raw 0..1 last passed to SetInputGain01(), for save/restore

    float speed_    = 1.f;
    float speed01_  = 0.5f; // raw 0..1 last passed to SetSpeed01(), for save/restore
    float volume_   = 1.f;
    float volume01_ = 1.f;  // raw 0..1 last passed to SetVolume01(), for save/restore
    float pan_      = 0.5f;

    FilterMode filter_mode_      = FilterMode::Off;
    float      filter_cutoff_hz_ = 1200.f;
    float      filter_cutoff01_  = 0.5f; // raw 0..1, for save/restore
    float      filter_res_       = 0.2f;
    float      filter_res01_     = 0.22f; // raw 0..1, for save/restore
    daisysp::Svf filter_l_, filter_r_;

    LayerEffect effect_        = LayerEffect::Off;
    float       effect_param_a_ = 0.5f;
    float       effect_param_b_ = 0.5f;
    float       effect_fade_    = 1.f; // 0..1, ramps up after SetEffect() to declick
    // One instance per channel for the effects that process L/R
    // independently; Chorus is fed the left channel only and generates
    // its own stereo spread via GetLeft()/GetRight(). Phaser and
    // PitchShifter are also mono-in (fed a summed L+R, output to both
    // channels) -- not for the same reason as Chorus, but because a
    // stereo pair of either would cost too much SDRAM per layer (see
    // fx_phaser_/fx_pitchshift_/fx_reverb_ below and Init()'s comment).
    daisysp::Overdrive  fx_drive_[2];
    daisysp::Decimator  fx_bitcrush_[2];
    daisysp::Chorus     fx_chorus_;
    daisysp::Tremolo    fx_tremolo_[2];
    daisysp::Flanger    fx_flanger_[2];
    daisysp::Autowah    fx_autowah_[2];
    daisysp::Phaser*       fx_phaser_     = nullptr; // externally owned, see Init()
    daisysp::PitchShifter* fx_pitchshift_ = nullptr; // externally owned, see Init()
    daisysp::ReverbSc*     fx_reverb_     = nullptr; // externally owned, see Init()

    float reverb_send_   = 0.f;  // 0 = this layer's reverb hears nothing
    float reverb_size01_ = 0.6f; // this layer's own reverb feedback/decay

    bool  pitch_enabled_  = false;
    float pitch_amount01_ = 0.5f; // raw 0..1, save/restore; 0.5 = unison
    float pitch_fun01_    = 0.f;  // raw 0..1, save/restore
    float pitch_fade_     = 1.f;  // 0..1, ramps up after enabling to declick
    int   pitch_delay_preset_ = 0; // 0=Fast (lowest latency), see SetPitchDelayPreset()

    float meter_ = 0.f;
    float waveform_peaks_[kWaveformCols] = {};
};
