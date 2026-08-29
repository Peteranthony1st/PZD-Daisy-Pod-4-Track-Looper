#pragma once
#include <cstddef>
#include <cstdint>

// TempoClock is the single, shared timing reference for the whole looper.
//
// It owns:
//   - BPM and bars-per-loop (this defines the "shared master length" that
//     every layer's recordings are locked to once the first layer is
//     recorded)
//   - the audible metronome click (including a 1-bar count-in before a
//     recording actually starts)
//   - a sample-accurate beat/bar grid that layers use to know exactly when
//     a count-in finishes and recording should begin
//
// It does NOT know anything about audio buffers, layers, or the mixer --
// it just ticks. One instance is shared by all 5 LooperLayers.
//
// Threading / call model: call Process() exactly once per audio sample,
// for the whole engine (not once per layer). Typical use in the audio
// callback:
//
//   for (size_t i = 0; i < size; i++)
//   {
//       TempoTick tick = tempo.Process();
//       float     click = tempo.RenderClick(tick);
//       for (auto& layer : layers) layer.ProcessSample(i, tick, ...);
//       out[0][i] += click; out[1][i] += click;
//   }
//
// (LooperLayer actually takes a whole precomputed tick array per block --
// see looper_layer.h -- but the per-sample contract is the same.)

class TempoClock
{
  public:
    // What happened on this particular sample, handed to every layer so
    // they can all agree on the same beat/bar grid.
    struct TempoTick
    {
        bool beat;          // a beat boundary lands on this sample
        bool downbeat;      // beat==true and it's beat 1 of the bar
        bool bar;           // a bar boundary (loop point) lands on this sample
        bool count_in_beat; // a count-in click should sound this sample
        bool record_start;  // recording should begin on THIS sample
                             // (immediately if metronome is off, or after
                             // the count-in finishes if it's on)
    };

    void Init(float sample_rate);

    // --- Tempo / loop length -------------------------------------------
    // BPM and Bars can only be changed while Unlocked (i.e. before any
    // layer holds a recording). Once the first layer finishes recording,
    // call Lock() so every subsequent layer records to the exact same
    // sample count and they all stay in phase. Call Unlock() only after
    // every layer has been cleared back to empty.
    void  SetBpm(float bpm);   // clamped to [40, 240]
    float GetBpm() const { return bpm_; }
    void  SetBars(int bars);   // clamped to [1, 16]
    int   GetBars() const { return bars_; }
    void  Lock() { locked_ = true; }
    void  Unlock() { locked_ = false; }
    bool  IsLocked() const { return locked_; }

    // Length, in samples, of the shared master loop at the current
    // BPM/Bars (4/4 time is assumed -- see kBeatsPerBar below).
    size_t GetLoopLengthSamples() const;

    // Live position within the bar/loop, for the on-screen beat-dot
    // indicator (see Ui::DrawBeatIndicator()) -- both free-running,
    // ticking regardless of whether the metronome click is audible.
    // beat_in_bar_/bar_in_loop_ are incremented at the same instant the
    // CURRENT beat/bar's click fires (see Process()), so by the time
    // anything reads them they already reflect the UPCOMING beat/bar,
    // not the one actually sounding -- subtract 1 (with wraparound) to
    // report what's audible right now, matching what a human watching
    // the dots while listening expects "beat N" to mean.
    int GetBeatInBar() const // 0..kBeatsPerBar-1
    {
        return (beat_in_bar_ - 1 + kBeatsPerBar) % kBeatsPerBar;
    }
    int GetBarInLoop() const // 0..bars_-1
    {
        return (bar_in_loop_ - 1 + bars_) % bars_;
    }

    // --- Metronome --------------------------------------------------
    // Turns the audible click on/off for regular beats during normal
    // playback. Recording always gets a 1-bar count-in regardless of
    // this setting -- the count-in click is always audible (scaled by
    // SetMetronomeVolume01()) so a quantized record-start is actually
    // audible even with the regular per-beat click toggled off.
    void SetMetronomeEnabled(bool on) { metronome_enabled_ = on; }
    void ToggleMetronome() { metronome_enabled_ = !metronome_enabled_; }
    bool IsMetronomeEnabled() const { return metronome_enabled_; }
    void SetMetronomeVolume01(float v); // 0..1
    float GetMetronomeVolume01() const { return metronome_vol_; }

    // Called by whichever layer is arming to record. Always starts a
    // 1-bar count-in; TempoTick::record_start fires true on the sample
    // the count-in finishes (see kCountInBeats).
    void RequestRecordStart();

    // Resets the clock's *phase* -- sample-within-beat, beat-in-bar,
    // bar-in-loop, count-in state, and the click envelope -- back to a
    // clean "sample 0 of bar 1" starting point, without touching bpm_/
    // bars_/locked_/metronome_enabled_/metronome_vol_. For
    // PerformanceStore::Load(): every restored layer's play_pos_ starts
    // at 0 (see LooperLayer::RestoreRecordedLength()), so the tempo's
    // own phase needs the same reset or the metronome/beat-indicator
    // would resume wherever it happened to be instead of aligned with
    // the newly loaded audio's downbeat.
    void ResetPhase();

    // Advances the master clock by one sample and returns what happened.
    // Call this exactly once per sample, for the whole engine.
    TempoTick Process();

    // Renders the current sample of the metronome/count-in click. Call
    // this once per sample (every sample, not just on ticks -- it needs
    // to ring out the decay of a click that was just triggered). Returns
    // 0 if the metronome is disabled.
    float RenderClick(const TempoTick& tick);

    static constexpr int kBeatsPerBar  = 4; // 4/4 time is assumed throughout
    static constexpr int kCountInBeats = 4; // 1 bar count-in

  private:
    float SamplesPerBeat() const;

    float  sample_rate_ = 48000.f;
    float  bpm_         = 100.f;
    int    bars_        = 4;
    bool   locked_      = false;

    bool   metronome_enabled_ = true;
    float  metronome_vol_     = 0.5f;

    // Free-running phase, in samples, since the last beat.
    double phase_samples_ = 0.0;
    int    beat_in_bar_   = 0; // 0..kBeatsPerBar-1
    int    bar_in_loop_   = 0; // 0..bars_-1

    // Count-in state machine.
    enum class CountState
    {
        Idle,          // nothing pending
        WaitingForBar, // RequestRecordStart() was called; waiting for the
                       // next downbeat so the count-in starts cleanly
        CountingIn     // playing the count-in beats
    };
    CountState count_state_                    = CountState::Idle;
    int        count_in_beats_remaining_       = 0;
    bool       record_pending_after_count_in_  = false; // record_start
                                                          // fires on the
                                                          // beat right
                                                          // after the
                                                          // last count-in
                                                          // click

    // Click envelope (simple decaying sine "tick").
    float  click_env_   = 0.f;
    float  click_phase_ = 0.f;
    float  click_freq_  = 1200.f;
    float  click_decay_coeff_ = 0.f; // computed in Init()
    bool   click_audible_      = false; // latched per-click: count-in
                                         // clicks are always audible;
                                         // regular beat clicks only if
                                         // metronome_enabled_ was set
                                         // when this click was triggered
};
