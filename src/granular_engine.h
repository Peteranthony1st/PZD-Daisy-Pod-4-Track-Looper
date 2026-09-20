#pragma once
#include <cstdint>
#include <cstddef>
#include "daisysp.h"
#include "looper_layer.h" // FilterMode, kFilterMinHz/kFilterMaxHz

// A monophonic granular voice -- deliberately much simpler than this
// project's earlier 8-voice granular engine (shelved on the `granular`
// git branch): one held note at a time, no oscillator layer (PadSynth
// covers that role now), and TWO overlapping-grain layers sharing the
// same Size/Fill/Gap scheduling instead of one polyphonic grain cloud
// plus a separate, thinner "scan grain":
//
//   - Grain layer: reads from a fixed Position anchor.
//   - Scan layer: reads from a continuously-sweeping anchor (bounces
//     between ScanStart/ScanEnd at a speed/direction set by Scan).
//
// Both layers are the SAME GrainCluster mechanism (see below) -- the old
// engine's Scan was a single retriggered grain on its own timer, and even
// after several rounds of tuning it stayed thinner/choppier than the main
// grain cloud, because one voice retriggering periodically is inherently
// less smooth than several overlapping ones. Giving Scan the identical
// multi-slot scheduling the Grain layer already uses fixes that by
// construction rather than by tuning constants further.
//
// Does NOT own the captured buffer -- SetSource() just points at whatever
// buffer currently holds a capture (SDRAM, owned by main.cpp), same
// convention the old engine used.
class GranularEngine
{
  public:
    void Init(float sample_rate);

    // len == 0 means "nothing captured yet" -- NoteOn() is a no-op then.
    // Silences both grain clusters outright (a grain's position was
    // computed against the OLD src_len_, and a shorter new capture could
    // otherwise leave it out of bounds) -- same reasoning the old engine's
    // SetSource() used.
    void SetSource(const float* buf_l, const float* buf_r, size_t len);
    // Live re-trim of the SAME already-loaded capture (Screen::Granular's
    // Trim page) -- updates the readable range like SetSource() does,
    // but deliberately skips its hard reset (clearing both grain
    // clusters, cancelling the held note) AND its waveform-peaks
    // recompute. The reset isn't actually needed for safety here: every
    // grain read already clamps to src_len_ independently on every
    // sample (see RenderGrain()), so a currently-playing note simply
    // keeps sounding smoothly through a live trim adjustment instead of
    // being cut on every single knob tick. The peaks recompute is left
    // to the caller (see RecomputeWaveformPeaks() below) specifically so
    // it can be throttled to a real display-refresh rate instead of
    // running full-speed on every control-loop tick -- it's an O(full
    // capture length) scan (up to ~480,000 samples), genuinely too
    // costly to redo at knob-polling rate; doing so was a second real,
    // user-reported cause of "still jumpy, slow to update" on this page
    // even after the reset fix above.
    void SetTrimRange(const float* buf_l, const float* buf_r, size_t len);
    // Rebuilds waveform_peaks_ from whatever src_l_/src_len_ currently
    // point at -- called automatically by SetSource() (a rare, one-time
    // event, fine to always do immediately), and separately, throttled,
    // by Trim's own live-adjustment path (see SetTrimRange()'s comment
    // for why that one can't just call this every tick too).
    void RecomputeWaveformPeaks();
    bool   HasSource() const { return src_len_ > 0; }
    size_t GetSourceLen() const { return src_len_; }

    // Monophonic: a NoteOn always retriggers the single voice (last-note
    // priority), no legato/portamento. NoteOff() only releases if it
    // matches the currently-held note, so releasing an old note after a
    // new one has already retriggered doesn't cut the new one short.
    void NoteOn(uint8_t note, uint8_t velocity);
    void NoteOff(uint8_t note);
    bool IsNoteActive() const { return held_note_ >= 0; }

    // --- Size/Gap -- shared by BOTH the Grain and Scan layers (they're
    // reading the same kind of grains, just from different anchors) --
    // Fill is NOT shared -- each layer has its own (see GetScanFill01()
    // below) so e.g. a dense Grain texture can sit under a sparse single
    // Scan grain, or vice versa. -------------------------------------
    void  SetSize01(float v01);
    float GetSize01() const { return size01_; }
    // Quantized to [0, kGrainsPerVoice] active slots -- 0 is silence (no
    // grains scheduled at all), matching the old engine's GrainCount
    // convention, just renamed to the reference device's "Fill"
    // vocabulary. This is the Grain layer's own count.
    void  SetFill01(float v01);
    float GetFill01() const { return (float)fill_count_ / (float)kGrainsPerVoice; }
    int   GetFill() const { return fill_count_; }
    // Scan layer's own independent grain count, same [0, kGrainsPerVoice]
    // quantization and meaning as Fill above, just for the Scan cluster
    // instead of the Grain cluster -- previously hardcoded to always
    // match Fill exactly (one shared fill_count_ used by both clusters),
    // split out per a real user request (dense Grain texture, sparse
    // single Scan grain, or any other independent combination).
    void  SetScanFill01(float v01);
    float GetScanFill01() const { return (float)scan_fill_count_ / (float)kGrainsPerVoice; }
    int   GetScanFill() const { return scan_fill_count_; }
    // 0 = grains packed with no gap (hop = grain length / Fill), 1 =
    // maximally sparse (real silence between them) -- old engine's
    // Density01, renamed AND polarity-flipped (Density 1.0 was "packed",
    // this project's own reference device labels the sparse end "Gap"
    // going up, not down). This is the Grain layer's own gap.
    void  SetGap01(float v01);
    float GetGap01() const { return gap01_; }
    // Scan layer's own independent gap, same 0..1 meaning as Gap above
    // -- previously hardcoded to always match Gap exactly (one shared
    // gap01_ used by both clusters' hop timing), split out per a real
    // user request, same reasoning as SetScanFill01() above.
    void  SetScanGap01(float v01);
    float GetScanGap01() const { return scan_gap01_; }

    // --- Rhythm -- gates WHICH of the Grain layer's already-scheduled
    // hops actually spawn a grain, rather than changing the timing
    // itself -- a skipped hop is a real silence, not a re-timed one,
    // which is what makes the gaps genuinely uneven instead of just
    // quantized-but-still-flat. Discrete, Button1-cycled (see
    // CycleRhythm()): Off (every hop fires -- exactly this engine's
    // original behaviour), a small named bank of fixed grooves, then two
    // fixed Euclidean stops. Only gates the Grain layer (Position
    // anchor) -- Scan is untouched. The step clock resets to 0 on every
    // NoteOn so a pattern always starts the same way.
    void        CycleRhythm();
    int         GetRhythmIndex() const { return rhythm_index_; }
    const char* GetRhythmName() const { return rhythm_name_cached_; }

    // --- Speed -- how often a Rhythm pattern's steps actually happen,
    // completely decoupled from Fill (see CycleGrainSpeed()'s own
    // comment for why that decoupling matters). Only takes effect once a
    // Rhythm pattern is selected -- has no effect at Rhythm=Off.
    enum class GrainSpeed
    {
        Slow,
        Medium,
        Fast,
        Sync // locks to the Looper's live tempo, see SetExternalBpm()
    };
    void        CycleGrainSpeed();
    GrainSpeed  GetGrainSpeed() const { return grain_speed_; }
    const char* GetGrainSpeedName() const;
    // Pushed in live from Ui (which already owns the TempoClock) once
    // per main-loop tick -- this engine has no tempo concept of its own
    // otherwise, see ComputeRhythmStepSamples()'s Sync case.
    void SetExternalBpm(float bpm) { external_bpm_ = bpm; }

    // --- Position (Grain layer's fixed anchor) -----------------------
    void  SetPosition01(float v01);
    float GetPosition01() const { return position01_; }

    // --- Jitter -- random offset around Position's fixed anchor, applied
    // fresh to each Grain-layer retrigger (see TriggerGrainInCluster()'s
    // own doc comment for why this exists: a fixed anchor replays the
    // exact same buffer offset every single time, so a real feature
    // sitting inside that grain's window -- not at its very edge, where
    // the Hann window silences it -- repeats identically forever,
    // audible as a perfectly periodic click on sustained/hot source
    // material). 0 = no jitter (today's exact behavior, unchanged).
    void  SetJitter01(float v01);
    float GetJitter01() const { return jitter01_; }

    // --- Scan -- TWO separate controls, on two different pages, not one
    // knob wearing two labels:
    //
    //   - Scan (Scan page's own K1): speed + direction, dead-zone
    //     centered on 50% -- center is off (genuinely silent, not just
    //     frozen-but-audible -- see SetScan01()'s own comment for the
    //     real bug reports that shaped this twice), either side sets
    //     bounce speed and which way it heads first. Same shape this
    //     engine originally had. The Scan page's own K2 is Scan Fill
    //     (see SetScanFill01()) -- the Scan layer's own independent
    //     grain-density control.
    //   - Scan Start / Scan End (own "Scan Range" page, right after
    //     Position): the sweep's own range, fully adjustable at both
    //     ends -- default Start 0 (buffer start), End 1 (buffer end),
    //     matching the original fixed-to-buffer-end behavior exactly.
    //     A fresh note's sweep always begins at Start; the live bounce/
    //     wrap in Process() runs between whichever of Start/End is
    //     actually lower and higher (so setting Start above End just
    //     works, rather than producing an inverted/broken range).
    //     Neither carries any mute meaning of its own -- that's Scan
    //     (above)'s job.
    //
    void  SetScan01(float v01);
    float GetScan01() const { return scan01_; }
    void  SetScanPosition01(float v01);
    float GetScanPosition01() const { return scan_start01_; }
    void  SetScanEnd01(float v01);
    float GetScanEnd01() const { return scan_end01_; }
    bool  IsScanMuted() const { return scan_speed_ == 0.f || scan_volume01_ <= 0.001f; }
    // true (default) = bounce (reverse at each edge, all prior behavior);
    // false = wrap (jump straight back to the other edge, same
    // direction) -- see scan_bounce_'s own comment.
    void ToggleScanBounce() { scan_bounce_ = !scan_bounce_; }
    bool GetScanBounce() const { return scan_bounce_; }

    // --- Tune / Map to Note -- both restored from the old engine
    // unchanged: Tune is a fixed per-grain pitch offset independent of
    // note, Map to Note additionally tracks the held note's own pitch
    // (12-TET against middle C) on top of Tune. ------------------------
    void  SetGrainTuneSemitones01(float v01); // quantized to whole semitones, +-24
    float GetGrainTuneSemitones01() const;
    int   GetGrainTuneSemitones() const { return grain_tune_semitones_; }
    void  SetGrainFollowsNote(bool on) { grain_follows_note_ = on; }
    bool  GetGrainFollowsNote() const { return grain_follows_note_; }

    // --- Direction (per-grain read direction; Spray dropped this
    // redesign -- was a source of real jitter bugs in the old engine) --
    enum class Direction
    {
        Forward,
        Reverse,
        Random
    };
    void      SetDirection01(float v01); // quantized to the 3 states above
    Direction GetDirection() const { return direction_; }
    float     GetDirection01() const;

    // Scan layer's own independent read direction -- previously each
    // Scan grain silently inherited whichever way the sweep itself
    // happened to be currently bouncing (forward while sweeping up,
    // backward after a bounce), with no way to just always play forward,
    // always reversed, or randomized regardless of sweep direction. This
    // is a real, deliberate decoupling: the sweep's own bounce/wrap
    // direction (Process()'s scan_direction_sign_, still governs where
    // the anchor moves) and a Scan grain's own playback direction are
    // now two separate things, exactly mirroring how Direction (above)
    // already works for the Grain layer. Button-cycled (Scan Range
    // page's own Button1), not knob-quantized, since both of that page's
    // knobs are already Start/End.
    void      CycleScanDirection() { scan_direction_ = (Direction)(((int)scan_direction_ + 1) % 3); }
    Direction GetScanDirection() const { return scan_direction_; }

    // --- Note-level ADSR -- shapes the whole voice's loudness across a
    // held note, layered ON TOP of (not instead of) the fixed per-grain
    // Hann window below (that only prevents clicks within a grain, it
    // has no swell-in/tail-off of its own). Same curved-seconds
    // convention as PadSynth's own ADSR. -------------------------------
    void  SetAttack01(float v01);
    float GetAttack01() const { return attack01_; }
    float GetAttackSeconds() const;
    void  SetDecay01(float v01);
    float GetDecay01() const { return decay01_; }
    float GetDecaySeconds() const;
    void  SetSustain01(float v01);
    float GetSustain01() const { return sustain01_; }
    void  SetRelease01(float v01);
    float GetRelease01() const { return release01_; }
    float GetReleaseSeconds() const;

    // --- Bus filter (post-mix, same shape as PadSynth's own) ----------
    void       SetFilterMode(FilterMode m) { filter_mode_ = m; }
    FilterMode GetFilterMode() const { return filter_mode_; }
    void       SetFilterCutoff01(float v01) { filter_cutoff01_ = v01; }
    float      GetFilterCutoff01() const { return filter_cutoff01_; }
    void       SetFilterResonance01(float v01) { filter_res01_ = v01; }
    float      GetFilterResonance01() const { return filter_res01_; }

    // --- Grain/Scan mixer -- independent level for each layer before
    // they sum, so Scan can be blended in under the main texture (or
    // soloed, or muted) rather than always at a fixed ratio. -----------
    void  SetGrainVolume01(float v01) { grain_volume01_ = v01; }
    float GetGrainVolume01() const { return grain_volume01_; }
    void  SetScanVolume01(float v01) { scan_volume01_ = v01; }
    float GetScanVolume01() const { return scan_volume01_; }

    // --- Overall output level -- applied AFTER the Grain/Scan mix above,
    // a separate control from it (this is "how loud is Grains in the
    // master mix", not "how loud is Scan relative to Grain"). Needed for
    // the Global Mixer page, which controls Pad's and Grains' levels
    // side by side the same way PadSynth::SetOutputLevel01() already
    // does for Pad. -----------------------------------------------------
    void  SetOutputLevel01(float v01);
    float GetOutputLevel01() const { return output_level01_; }

    // --- Reverb send -- was previously an unconditional full send (no
    // control at all); now a real continuous knob, reachable from both
    // this engine's own Mix page and Global:Mixer -- same underlying
    // value either way (this field), so changing it in one place updates
    // the other with no extra sync needed, same as PadSynth's own
    // reverb send.
    void  SetReverbSend01(float v01) { reverb_send01_ = v01; }
    float GetReverbSend01() const { return reverb_send01_; }

    // Own independent send into the shared delay bus (main.cpp's
    // fx_delay_l/r) -- same relationship to it Reverb Send above has to
    // fx_reverb_shared, added on Grains' own new FX page. Same session-
    // level (not saved in presets) treatment as Reverb Send.
    void  SetDelaySend01(float v01) { delay_send01_ = v01; }
    float GetDelaySend01() const { return delay_send01_; }

    // --- Pan -- same linear law as LooperLayer/PadSynth (panL = 1-v,
    // panR = v), applied to the already-stereo captured signal (Grains
    // reads real L/R from whatever was captured/imported, unlike Pad's
    // mono voice sum) right before writing out_l/out_r. A session-level
    // mixer setting, not part of the captured sound's own identity --
    // same reasoning that already excludes output_level01_/reverb_send01_
    // from GranularPresetData below, kept consistent with those.
    void  SetPan01(float v01);
    float GetPan01() const { return pan01_; }

    // Renders `size` samples, WRITES into out_l/out_r (see PadSynth's own
    // Process() doc comment for why -- main.cpp needs this exact signal
    // for more than one consumer). ADDS into reverb_send_l/r (already
    // zeroed by the caller for this block, same convention every other
    // reverb-send contributor in main.cpp uses).
    void Process(size_t      size,
                 float*      out_l,
                 float*      out_r,
                 float*      reverb_send_l,
                 float*      reverb_send_r,
                 float*      delay_send_l,
                 float*      delay_send_r);

    // --- Visuals ---------------------------------------------------
    // Live anchor position (0..1 across the source), for the grain-marker
    // display -- the Grain layer's is fixed (== Position) while a note
    // isn't sweeping anything; the Scan layer's moves every block.
    float GetGrainAnchor01() const;
    float GetScanAnchor01() const;
    // What fraction of the whole loaded/captured buffer one Grain-layer
    // grain actually spans, given the current Size and this buffer's own
    // length -- lets the waveform display draw the Grain anchor as a
    // real span (grows/shrinks with Size) instead of a single point.
    float GetGrainSizeFraction01() const;
    // Same "cached once in SetSource(), cheap to redraw from" convention
    // as LooperLayer::GetWaveformPeaks() -- the whole-sample display
    // needs the FULL buffer's peaks, unlike the old engine's zoomed-window
    // view which only ever needed a small slice.
    static constexpr int kWaveformCols = 63;
    const float* GetWaveformPeaks() const { return waveform_peaks_; }

    // Read-only access to whatever SetSource() currently points at --
    // needed so a preset save can write the actual captured audio out
    // (this engine doesn't own that buffer, see SetSource()'s own doc
    // comment, so it can't save it itself).
    const float* GetSourceL() const { return src_l_; }
    const float* GetSourceR() const { return src_r_; }

    // Flat snapshot of every knob-adjustable setting above (NOT the
    // captured audio itself -- that's saved/loaded separately, see
    // PerformanceStore::SaveGranularPreset()/LoadGranularPreset()), same
    // "one place that knows how to capture/restore the whole instrument's
    // state" idiom as PadSynth::PadPresetData.
    struct GranularPresetData
    {
        float   size01            = 0.4f;
        float   fill01            = 0.667f; // matches the class's own default fill_count_=2
        float   scan_fill01       = 0.667f; // matches the class's own default scan_fill_count_=2
        float   gap01             = 0.1f;
        float   scan_gap01        = 0.1f; // matches the class's own default scan_gap01_
        float   position01        = 0.f;
        float   scan01            = 0.5f; // Scan speed/direction, center = off
        float   scan_start01      = 0.f;  // Scan range Start, buffer start
        float   scan_end01        = 1.f;  // Scan range End, buffer end
        float   grain_tune01      = 0.5f; // 0 semitones
        bool    grain_follows_note = false;
        float   direction01       = 0.1667f; // Forward
        int32_t scan_direction    = (int32_t)Direction::Forward;
        float   attack01          = 0.3f;
        float   decay01           = 0.3f;
        float   sustain01         = 0.8f;
        float   release01         = 0.4f;
        int32_t filter_mode       = (int32_t)FilterMode::Off;
        float   filter_cutoff01   = 1.f;
        float   filter_res01      = 0.f;
        float   grain_volume01    = 0.8f;
        float   scan_volume01     = 0.5f;
        int32_t rhythm_index      = 0; // 0 = Off, see CycleRhythm()
        int32_t grain_speed       = (int32_t)GrainSpeed::Slow;
        float   jitter01          = 0.f; // 0 = off, see SetJitter01()
        bool    scan_bounce       = true; // matches the class's own default
    };
    void               ApplyPreset(const GranularPresetData& p);
    GranularPresetData CapturePreset() const;

  private:
    // Max concurrent grains per cluster (Grain and Scan each get their
    // own budget of this many) -- raised from the original 3 per a real
    // user request for denser textures. Fill's own 0..1 range already
    // scales relative to this constant (SetFill01()/GetFill01() both go
    // through fill_count_ = round(v01 * kGrainsPerVoice)), so existing
    // presets' stored Fill fraction lands at a proportionally denser
    // absolute count automatically, no migration needed. Real cost, not
    // free: at Fill all the way up this roughly triples the worst-case
    // RenderGrain() calls per sample per cluster (both the active-grain
    // loop and the release-fade loop in RenderCluster() scale with this
    // constant) -- costs nothing extra at lower Fill settings, since
    // only as many grains as Fill actually calls for ever render.
    static constexpr int kGrainsPerVoice = 10;

    struct Grain
    {
        bool  active    = false;
        float phase     = 0.f; // 0..1 through the grain's own Hann window
        float phase_inc = 0.f;
        float read_pos  = 0.f; // absolute sample index into src_l_/src_r_
        float read_inc  = 1.f; // signed -- direction + pitch combined
        float gain      = 1.f;
    };

    // One of these per layer (Grain, Scan) -- identical mechanism, only
    // the anchor each one is triggered against differs (see Process()).
    struct GrainCluster
    {
        Grain grains[kGrainsPerVoice];
        // Steal-safe hand-off, same convention as the old engine's
        // Voice::release_grain -- a grain overwritten mid-envelope fades
        // out here instead of an audible instant cut. One release slot
        // PER grain slot (not just one shared slot) so ChokeGranularNote()
        // can fade out every simultaneously-active grain independently
        // when a new note interrupts a still-held one, not just whichever
        // single grain happens to be getting stolen at that instant.
        Grain release_grains[kGrainsPerVoice];
        float release_fades[kGrainsPerVoice] = {};
        float next_grain_countdown           = 0.f;
    };

    // fill_count is which cluster's own count to use (fill_count_ for
    // the Grain cluster, scan_fill_count_ for Scan) -- passed explicitly
    // rather than read from a shared member since the two are now
    // independent (see SetScanFill01()'s own doc comment).
    void  TriggerGrainInCluster(GrainCluster& c, float read_pos, float read_inc, float gain,
                                 int fill_count);
    void  RenderGrain(Grain& g, float extra_gain, float& out_l, float& out_r) const;
    // Sums a cluster's active grains + its release-fade grains into L/R,
    // advancing/deactivating each as it goes. fill_count: see
    // TriggerGrainInCluster()'s own comment.
    void  RenderCluster(GrainCluster& c, float overlap_gain, int fill_count, float& out_l,
                         float& out_r);
    // Moves every currently-active grain in a cluster into its own
    // release-fade slot (silencing the cluster's live grains without an
    // audible click) -- called from NoteOn() when a genuinely different
    // note interrupts one that's still sounding, so fast chord-like
    // playing doesn't stack overlapping grain bursts from several notes
    // at once (monophonic in more than just pitch-tracking).
    void  ChokeCluster(GrainCluster& c);
    // fill_count/gap01: see TriggerGrainInCluster()'s own comment -- Size
    // is still shared, Fill and Gap are each cluster's own now.
    float ComputeHopSamples(int fill_count, float gap01) const;
    float ReadHann(float phase01) const;
    float ResolveDirectionSign();
    float ResolveScanDirectionSign(); // same shape as above, for scan_direction_

    const float* src_l_   = nullptr;
    const float* src_r_   = nullptr;
    size_t       src_len_ = 0;
    float        sample_rate_ = 48000.f;

    int   held_note_ = -1;
    float note_rate_ = 1.f; // 2^((note-60)/12), read live if grain_follows_note_
    float note_gain_ = 1.f; // sqrt(velocity/127) taper, same as the old engine

    GrainCluster grain_cluster_;
    GrainCluster scan_cluster_;

    float size01_          = 0.4f;
    int   fill_count_      = 2; // Grain cluster's own count
    int   scan_fill_count_ = 2; // Scan cluster's own count, see SetScanFill01()
    float gap01_      = 0.1f; // Grain cluster's own gap
    float scan_gap01_ = 0.1f; // Scan cluster's own gap, see SetScanGap01()
    static constexpr float kMinGrainMs      = 0.f;
    static constexpr float kMaxGrainMs      = 1000.f;
    static constexpr float kMaxSparseFactor = 6.f; // same as the old engine's Density range

    // --- Rhythm pattern gate (Grain layer only) + Speed ---------------
    // rhythm_mask_cached_/rhythm_name_cached_ are resolved from
    // rhythm_index_ by RecomputeRhythmPattern() whenever CycleRhythm()
    // changes it, NOT recomputed per-hop -- the per-hop check in
    // Process() is just one bit test. kRhythmSteps is fixed (not a user
    // control any more) now that Speed owns timing independently of
    // Fill -- see CycleGrainSpeed()'s own .cpp comment.
    static constexpr int kRhythmSteps     = 16;
    static constexpr int kNumRhythmStates = 7; // Off + 4Floor/Tresillo/OffBeat/Sparse + 2 Euclid
    int         rhythm_index_        = 0;      // 0 = Off
    int         rhythm_step_         = 0;      // which step the NEXT hop lands on
    uint16_t    rhythm_mask_cached_  = 0xFFFF; // Off = every bit set = every hop fires
    const char* rhythm_name_cached_  = "Off";
    void        RecomputeRhythmPattern();

    GrainSpeed  grain_speed_   = GrainSpeed::Slow;
    float       external_bpm_ = 120.f;
    float       ComputeRhythmStepSamples() const;
    float       CurrentGrainIntervalSamples() const;

    float position01_ = 0.f;
    float jitter01_    = 0.f;

    // Scan: two independent values (see the public API's own comment) --
    // scan01_/scan_speed_/scan_initial_sign_ are the Scan page's own
    // "Scan" (speed+direction+dead-zone-mute), scan_start01_/scan_end01_
    // are the Scan Range page's own Start/End (see SetScanEnd01()'s own
    // doc comment). scan_position_samples_/scan_direction_sign_ are the
    // live, per-sample sweep state both feed into.
    float scan01_              = 0.5f; // center = off, see SetScan01()
    float scan_speed_          = 0.f;
    float scan_initial_sign_   = 1.f;
    static constexpr float kMaxScanFractionPerSecond = 1.f;
    float scan_start01_        = 0.f;  // sweep's starting point -- 0 = buffer start
    float scan_end01_          = 1.f;  // sweep's other bound -- 1 = buffer end (original fixed behavior)
    float scan_position_samples_ = 0.f; // live sweep position, advanced in Process()
    float scan_direction_sign_   = 1.f;
    // true (default, matches all prior behavior exactly) = reverse
    // direction at each edge; false = wrap straight back to the other
    // edge and keep heading the same way, so the sweep always resumes
    // from the same point instead of ping-ponging. See ToggleScanBounce().
    bool  scan_bounce_ = true;

    int   grain_tune_semitones_ = 0;
    float grain_tune_rate_      = 1.f; // 2^(semitones/12)
    bool  grain_follows_note_   = false;

    Direction direction_      = Direction::Forward;
    Direction scan_direction_ = Direction::Forward; // Scan layer's own, see CycleScanDirection()
    uint32_t  rng_state_ = 0x9E3779B9u; // xorshift32 seed, same as the old engine

    daisysp::Adsr adsr_;
    float attack01_ = 0.3f, decay01_ = 0.3f, sustain01_ = 0.8f, release01_ = 0.4f;
    static constexpr float kMinAdsrSeconds = 0.005f;
    static constexpr float kMaxAdsrSeconds = 3.f;

    daisysp::Svf filter_l_, filter_r_;
    FilterMode   filter_mode_     = FilterMode::Off;
    float        filter_cutoff01_ = 1.f;
    float        filter_res01_    = 0.f;

    float grain_volume01_  = 0.8f;
    float scan_volume01_   = 0.5f;
    float output_level01_  = 0.8f;
    float output_level_    = 1.f; // powf(output_level01_, 2.5f)*1.4f, cached by the setter
    float reverb_send01_   = 0.f; // was an unconditional full send before this existed
    float delay_send01_    = 0.f;

    float pan01_      = 0.5f;
    float pan_l_gain_ = 0.5f; // 1-pan01_, control-rate cache
    float pan_r_gain_ = 0.5f; // pan01_

    static constexpr int kHannTableSize = 256;
    float hann_table_[kHannTableSize];

    float waveform_peaks_[kWaveformCols] = {};
};
