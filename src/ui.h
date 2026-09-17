#pragma once
#include "daisy_pod.h"
#include "dev/oled_ssd130x.h"
#include "tempo_clock.h"
#include "looper_layer.h"
#include "font_tomthumb.h"

class GranularEngine;
class DexedSynth;

// The whole "one encoder + push button, two knobs, two buttons, one small
// OLED" menu system.
//
// IMPORTANT real-time rule: Ui::Update() talks to the OLED over I2C using
// *blocking* transfers (that's how libDaisy's SSD130x I2C transport
// works -- see dev/oled_ssd130x.h). A full-screen redraw takes on the
// order of a few milliseconds. That is completely fine to call from
// main()'s while(1) loop, and absolutely NOT fine to call from the audio
// callback (it would starve the real-time audio budget and you'd hear
// clicks/dropouts). So:
//
//   - main.cpp's AudioCallback() only touches TempoClock::Process() and
//     LooperLayer::Process() -- pure per-sample DSP, no hardware polling.
//   - main()'s while(1) loop calls pod.ProcessAllControls() and
//     Ui::Update() at a modest, steady rate (see main.cpp). This is where
//     encoder/button/knob reading AND the OLED redraw happen.
//
// The two sides only ever touch plain float/enum fields on TempoClock and
// LooperLayer (SetVolume01, SetBpm, etc.) -- no locks are used, same as
// the original firmware's approach to knobs/switches. That's fine for
// this kind of "slow" control data; it would not be fine for anything
// requiring sample-accurate consistency.
//
// Encoder/button EDGE events (as opposed to level state like Pressed())
// come in via UiControlEvents rather than being read straight off
// daisy::Encoder/Switch here. Reason: Draw()'s blocking OLED I2C writes
// can stall the main loop for tens of ms, and Encoder::Debounce() only
// resolves ONE quadrature step per call (plus self-limits to 1kHz) --
// Switch::RisingEdge()/FallingEdge() are similarly a single-tick pulse.
// If those were serviced only when the (potentially stalled) main loop
// got around to it, a fast encoder turn or a quick tap landing during a
// redraw would be silently dropped, not just delayed. main.cpp instead
// drives Debounce() from a 1kHz timer ISR and accumulates ticks/edges
// into monotonic counters; UiControlEvents is the delta-since-last-call
// computed from those. See main.cpp's ControlTimerCallback().
struct UiControlEvents
{
    int32_t encoder_delta      = 0; // net encoder steps since the last Update()
    bool    encoder_click_fell = false; // encoder switch released since the last Update()
    bool    btn1_released      = false; // button1 released since the last Update()
    bool    btn2_released      = false; // button2 released since the last Update()
    // How long button1/2 was held for, valid whenever btn1_released/
    // btn2_released is true -- captured inside ControlTimerCallback()'s
    // own 1kHz ISR (see its comment), NOT read from Switch::TimeHeldMs()
    // here in the main loop. TimeHeldMs() itself reads 0 the instant
    // Pressed() goes false, i.e. the exact same tick a release is
    // detected -- if a press/hold/release cycle completes while the main
    // loop happens to be busy elsewhere (a slow OLED redraw, a blocking
    // SD read, or just an unlucky scheduling gap), it never gets a
    // chance to observe "still held past threshold" while the button was
    // down, and by the time it checks again TimeHeldMs() already reads 0
    // -- silently misclassifying a long hold as a short tap. These
    // fields exist so every long-press site can fall back to a value
    // that's immune to that race (see HandleButton1()/HandleButton2()).
    float   btn1_held_ms       = 0.f;
    float   btn2_held_ms       = 0.f;
};

class Ui
{
  public:
    void Init(daisy::DaisyPod* pod,
              daisy::OneBitGraphicsDisplay* display,
              TempoClock*                   tempo,
              LooperLayer*                  layers,
              int                           num_layers,
              GranularEngine*               granular,
              const float*                  granular_scope_buf,
              size_t                        granular_scope_capacity,
              float*                        granular_capture_buf_l,
              float*                        granular_capture_buf_r,
              size_t                        granular_capture_capacity,
              volatile bool*                granular_capturing,
              volatile size_t*              granular_capture_write_pos,
              const float*                  master_scope_buf,
              size_t                        master_scope_capacity,
              DexedSynth*                   dexed);

    // Call once from main(), right after Init() -- applies the user's
    // saved startup defaults (see PerformanceStore::LoadPrefs()) on top
    // of Init()'s own hardcoded ones. A no-op if nothing's been saved
    // yet (no SD card, or the user's never used Global:File's Button2
    // tap to save one).
    void ApplyStartupDefaults();

    // Call once per main-loop iteration (NOT from the audio callback).
    void Update(const UiControlEvents& events);

    // Read by main.cpp's audio callback every block.
    float GetMasterVolume() const { return master_volume_; }
    bool  IsBypassed() const { return bypass_; }
    // Master-bus filter, applied once to the full mix (all layers plus
    // their reverb) rather than per-layer -- see main.cpp's
    // fx_master_filter_l/r. Cutoff/Resonance are raw 0..1; main.cpp
    // converts them using the same curve LooperLayer's per-layer filter
    // uses (kFilterMinHz/kFilterMaxHz in looper_layer.h).
    FilterMode GetMasterFilterMode() const { return master_filter_mode_; }
    float GetMasterFilterCutoff01() const { return master_filter_cutoff01_; }
    float GetMasterFilterResonance01() const { return master_filter_res01_; }
    // Shared reverb bus's Size/decay (see main.cpp's fx_reverb_shared) --
    // every layer's independent Send feeds this ONE instance now, not a
    // per-layer ReverbSc any more (see LooperLayer::SetReverbSend01()'s
    // comment for why). Raw 0..1, applied via SetFeedback() directly
    // (same range DaisySP's ReverbSc expects), no curve needed.
    float GetReverbSize01() const { return reverb_size01_; }
    // How much of the Bypass live-monitor signal feeds the shared reverb
    // bus above -- independent of every layer's own Send, same shared
    // bus though (see main.cpp's AudioCallback()). 0 (default) = none,
    // matching every per-layer Send's own default.
    float GetBypassReverbSend01() const { return bypass_reverb_send01_; }
    // Gain applied to the Bypass dry monitor mix -- uses whichever layer
    // is currently selected on the Home screen, since Bypass is mainly
    // used to check levels right before recording into that layer (see
    // LooperLayer::SetInputGain01(), the same control used at actual
    // record time).
    float GetBypassGain() const { return layers_[cursor_layer_].GetInputGain(); }
    // Screen::Mixer's own Bypass channel -- a genuine mix-level fader
    // multiplied ON TOP of GetBypassGain() above, not a replacement for
    // it: GetBypassGain() is deliberately about gain-staging/checking
    // levels before recording (see its own comment), while this is about
    // how loud Bypass sits in the overall final mix once you're past
    // that -- two different jobs on the same live signal, so this is
    // additive rather than reusing/repurposing the existing knob.
    // Defaults to 0.8f (matching every other engine's own Output Level
    // default in this codebase) since this is a brand new control with
    // no prior loudness to preserve -- same curve as every other Volume-
    // type knob (LooperLayer::SetVolume01(), GranularEngine's own
    // SetOutputLevel01()).
    float GetBypassMixVolume01() const { return bypass_mix_volume01_; }
    float GetBypassMixVolume() const { return bypass_mix_volume_; }
    // Defined in ui.cpp, not inline here -- needs powf(), same reasoning
    // every other curved setter in this header already avoids inlining.
    void  SetBypassMixVolume01(float v01);
    // Linear pan law, same as every other engine's own Pan (see
    // GranularEngine::SetPan01()'s comment) -- main.cpp computes panL/panR
    // from this directly (0.5 = center), same "no cached gain members
    // needed, this is UI-owned global state not a DSP object" treatment
    // as GetMasterVolume()'s own curve being applied inline there.
    float GetBypassPan01() const { return bypass_pan01_; }
    void  SetBypassPan01(float v01)
    {
        bypass_pan01_ = v01 < 0.f ? 0.f : (v01 > 1.f ? 1.f : v01);
    }
    // Block-rate: tape-style multiplier applied on top of every layer's
    // own Speed and TempoClock's own tick rate -- see main.cpp's
    // AudioCallback(), TempoClock::Process(), LooperLayer::Process().
    // 1.0 (default) = normal.
    float GetProjectSpeed() const { return project_speed_; }

    // Per-engine on/off (Global:Granular, Button1 tap) -- pulled forward
    // from the granular-rewrite plan's Stage 4 once real hardware
    // measurement showed CPU headroom was tight enough that a real way
    // to free up budget was needed now, not just once Granular's own UI/
    // persistence stages were done. main.cpp's AudioCallback() skips a
    // disabled engine's Process() call entirely (writing silence
    // instead) -- a real CPU saving, not just a mute.
    bool IsGranularEnabled() const { return granular_enabled_; }
    // Same real-CPU-saving on/off gate as Granular's own, for the new
    // Dexed engine (see main.cpp's AudioCallback()) -- no mutex with any
    // other instrument needed (unlike the old Pad/Fm pairing), Dexed is
    // independent of Granular the same way Granular is independent of
    // the looper.
    bool IsDexedEnabled() const { return dexed_enabled_; }
    // Global:Looper, Button1 tap -- a REAL stop, not TogglePauseAll()'s
    // own phase-locked pause: main.cpp's AudioCallback() skips every
    // LooperLayer::Process() call entirely while this is false, so
    // someone using only Plaits/Grains gets that CPU back rather than 4
    // idle-but-still-processing layers. Defaults true (unlike Pad/
    // Granular's own false default) since the loop is this project's
    // original core feature, not an add-on someone opts into.
    bool IsLooperEnabled() const { return looper_enabled_; }

  private:
    enum class Screen
    {
        Home,
        Layer,
        Global,
        // A real top-level screen, not a Global page -- entered from
        // Global's own Granular entry-point page (encoder click), exits
        // back to Home via the same long-press-from-any-non-Home-screen
        // path every other screen uses. See GranularParamPage for its
        // own pages.
        Granular,
        // Same treatment as Granular above, entered from Global:Mixer.
        // Unlike every other screen, encoder rotate here doesn't select a
        // page -- it scrolls through one continuous, endlessly-wrapping
        // sequence of "stops" (see mixer_position_): the 8 real channels
        // (each showing a Detail page with real Vol/Pan/Rev bars for the
        // group of 4 it belongs to), then a Scope stop (the final post-
        // fader mix), then back to channel 0 -- Global:Mixer's own page
        // already covers the "all 8 at a glance" job, so there's no
        // separate Overview stop in here too. Encoder click is unused
        // here, same as every other Pad/Granular-style screen.
        Mixer,
        // Same treatment as Granular above, entered from Global:Dexed.
        // See DexedParamPage for its own pages.
        Dexed
    };
    // Shared save/load interaction state for Global:File, Pad Preset, and
    // Grains Preset -- only one of those three pages is ever visible at
    // once, so one set of fields covers all three (see save_load_mode_'s
    // own comment for the reset-on-leave rule that keeps that safe).
    // Idle: just shows current status, Button1/Button2 say "Save"/"Load".
    // ChoosingSave: Button1 was tapped -- Knob1 picks Overwrite (only
    // offered when something's actually loaded) vs Save New, Button1's
    // own label becomes "Hold=Save" to commit whichever's picked.
    // BrowsingLoad: Button2 was tapped -- Knob1 browses the slot list
    // (same as it already did unconditionally before this), Button2's
    // label becomes "Hold=Load" to commit.
    enum class SaveLoadMode
    {
        Idle,
        ChoosingSave,
        BrowsingLoad
    };
    enum class LayerPage
    {
        Status,
        Speed,
        Filter,
        Effect,
        Reverb,
        Gain,
        kCount
    };
    enum class GlobalPage
    {
        Tempo,
        Filter,
        Reverb,
        Speed,
        File,
        // File management: Knob1 picks which of the 2 save folders
        // (Performances/Grains Presets) then Button1 tap drills in; once
        // inside, Knob1 browses that folder's own files (reusing the
        // exact same list Global:File/Grains Preset already scan and
        // cache), Button1 tap goes back up, Button1 hold Duplicates the
        // browsed file, Button2 hold Deletes it. See
        // SdMgmtFolder/sd_mgmt_in_folder_.
        SdMgmt,
        Export,
        // Entry point into Screen::Granular (not built yet -- Stage 2 of
        // the granular rewrite). For now this page only exposes the
        // on/off toggle pulled forward from Stage 4, same idiom as Pad's
        // own toggle below.
        Granular,
        // Entry point into Screen::Dexed, plus the on/off toggle -- same
        // Button1-tap convention as Granular's own above.
        Dexed,
        // On/off toggle only, same treatment as Pad/Granular above --
        // but for the whole 4-layer loop system, and a REAL stop (see
        // IsLooperEnabled()'s doc comment), not TogglePauseAll()'s own
        // phase-locked pause. Lets someone using only Plaits/Grains skip
        // all 4 LooperLayer::Process() calls entirely for the CPU back,
        // not just silence.
        Looper,
        // Knob1 = Plaits' overall output level, Knob2 = Grains' overall
        // output level -- both engines' own SetOutputLevel01(), so this
        // is genuinely "how loud is each in the master mix" rather than
        // a separate/competing level control.
        Mixer,
        kCount
    };
    enum class SdMgmtFolder
    {
        Performances,
        GranularPresets,
        kCount
    };

    // Screen::Granular's own pages -- Capture/Preset follow once real
    // capture/persistence logic exists (Stage 3); everything else here
    // just needed knobs wired to GranularEngine methods already built in
    // Stage 1.
    enum class GranularParamPage
    {
        // The showcase page: full-sample waveform + live grain-anchor
        // markers, Size/Fill/Gap/Scan values shown live -- Button1 maps
        // the knobs to Size+Fill, Button2 maps them to Gap+Scan (see
        // granular_grain_target_gap_scan_), same toggle idiom as Pad's
        // own merged ADSR page.
        Grain,
        Position, // Knob1 only, same single-knob convention as LayerPage::Speed
        // Knob1 = Tune (semitones), Knob2 = Direction; Button1 cycles
        // Map-to-Note on/off.
        TuneDirection,
        // Attack/Decay/Sustain/Release graph, same merged-page idiom as
        // Grain's Size/Fill/Gap/Scan and Pad's own ADSR page.
        ADSR,
        Filter, // Cutoff + Resonance, mode cycled by Button1; live oscilloscope
        Mix,    // Grain layer volume + Scan layer volume
        // Button1 cycles the source (Direct Record / From the currently
        // selected loop layer); Button2 held either records live input
        // for as long as it's held (Direct) or, past an 800ms threshold,
        // fires an instant copy (From Layer) -- see
        // Ui::TriggerGranularCaptureFromLayer() and HandleButton2()'s own
        // handling of this page. No continuous knobs.
        Capture,
        // Knob1 = trim start, Knob2 = trim end -- operates on whatever
        // SetSource() currently points at (Direct Record, From Layer, or
        // a loaded Preset), not just Direct Record specifically. Shows
        // the FULL original capture's waveform (granular_trim_full_peaks_,
        // a separate cache from GranularEngine's own -- see its comment)
        // with the trim points as the two markers, reusing
        // DrawGranularWaveform() for that -- same drawing code, just
        // start/end instead of Grain/Scan anchors. Non-destructive: the
        // underlying buffer is untouched, only the sub-range passed to
        // SetSource() changes, so re-opening this page and trimming again
        // always works from the original full capture.
        Trim,
        // Save/Copy(save-as-new)/Hold-to-Load, identical gesture set to
        // Pad's own Preset page -- but unlike Pad, saving/loading here
        // also streams the captured audio itself to/from the SD card
        // (see PerformanceStore::SaveGranularPreset()/LoadGranularPreset()),
        // so this one shows a live progress bar during the transfer
        // (Ui::OnSaveLoadProgress(), same as a whole-performance Save/Load).
        Preset,
        kCount
    };

    // Screen::Dexed's own pages -- a deliberately small "simple tier"
    // (see the approved UI plan) for quick sound-shaping; deep per-
    // operator editing lives in a separate advanced editor (a later
    // phase), reached by an encoder click from the Preset page below.
    enum class DexedParamPage
    {
        Algo,       // Button1 cycles the 32 real DX7 algorithms; no knobs
        Feedback,   // K1 only, 0-7
        // K1 = LFO speed, K2 = LFO pitch mod depth -- the patch's own
        // baked-in automatic vibrato, distinct from the mod wheel's own
        // separate ctrls_.wheel routing (already wired in Phase 4).
        Vibrato,
        // Bipolar macro, K1 only -- see DexedSynth::SetBrightness01()'s
        // own doc comment for the scaling shape.
        Brightness,
        // Bipolar macro, K1 only -- see DexedSynth::SetEnvSpeed01()'s own
        // doc comment.
        EnvSpeed,
        Filter, // Cutoff + Resonance, mode cycled by Button1
        Mix,    // Reverb Send + Output Level
        // Save/Load the whole Dexed sound as a preset, folder-browsing
        // 150+ factory presets by real sound-type category plus a
        // "User" folder of SD saves -- same two-level browsing shape
        // the removed FmSynth's own Preset page used, generalized to
        // more/larger categories. Encoder click here drills into the
        // advanced per-operator editor (a later phase).
        Preset,
        kCount
    };

    // One entry per distinct (screen, page) knob assignment -- NOT per
    // layer, because cursor_layer_ can only change while screen_==Home,
    // and entering a Layer page always passes through Home first (which
    // is its own context), so a context change is always detected on the
    // way in regardless of which layer got selected. See ApplyKnobs().
    enum class KnobContext
    {
        Home,
        LayerStatus,
        LayerSpeed,
        LayerFilter,
        LayerEffect,
        LayerReverb,
        LayerGain,
        GlobalTempo,
        GlobalFilter,
        GlobalReverb,
        GlobalSpeed,
        GlobalFile,
        GlobalExport,
        GlobalGranular, // entry point only -- see Screen::Granular instead
        GlobalLooper, // no continuous knobs -- Button1 toggle only
        GlobalMixer, // entry point only -- see Screen::Mixer instead
        GlobalSdMgmt, // browses a list directly, no pickup used -- see GlobalPage::SdMgmt
        GlobalDexed, // entry point only -- see Screen::Dexed instead
        // Screen::Granular's Grain page -- Button1/Button2 toggle which
        // pair the knobs reach (see granular_grain_target_gap_scan_).
        GranularGrainSizeFill,
        GranularGrainGapScan,
        GranularPosition,
        GranularTuneDirection,
        GranularEnvAD,
        GranularEnvSR,
        GranularFilter,
        GranularMix,
        GranularMixReverb,
        GranularCapture, // no continuous knobs -- Button1/Button2 only
        GranularTrim,
        GranularPreset, // browses a list directly, no pickup used
        // Screen::Mixer -- shared across all 7 non-Master channels (see
        // mixer_position_), same "one context, branch internally on which
        // channel" idiom as LayerStatus already uses for Cur()/
        // cursor_layer_ -- except unlike LayerStatus, mixer_position_ CAN
        // change while this exact context stays active (rotating between
        // channels doesn't change context), so HandleEncoder()'s own
        // rotate handling must explicitly re-run the pickup reset/reseed
        // this enum's own change would normally trigger automatically
        // (see its own comment there).
        MixerVolPan,
        MixerReverb,
        MixerMaster, // Master has no toggle -- Volume + Reverb Size, always
        MixerNoKnobs, // Overview/Scope stops -- no continuous knobs
        // Screen::Dexed's own pages -- see DexedParamPage for what each
        // one's knobs do.
        DexedAlgo, // no continuous knobs -- Button1 cycles it
        DexedFeedback,
        DexedVibrato,
        DexedBrightness,
        DexedEnvSpeed,
        DexedFilter,
        DexedMix,
        DexedPreset, // browses a list directly, no pickup used
        kCount
    };
    KnobContext CurrentKnobContext() const;
    // Soft pickup/takeover: a knob only starts driving its parameter once
    // its physical position is within kKnobPickupEpsilon of `stored_raw`
    // (the raw reading last applied for this (context, knob)), so
    // switching screens/pages doesn't yank a value to wherever the knob
    // physically happens to be sitting. ApplyKnobs() re-arms this
    // (engaged=false) every time the (screen, page) context changes --
    // that's not as heavy-handed as it sounds: the very first tick back
    // on a context re-checks proximity immediately (no actual wiggle
    // needed) and re-engages right away if the knob genuinely didn't
    // move while you were elsewhere. It only stays "caught" -- requiring
    // a real sweep -- when the knob DID move (e.g. you turned knob 1 to
    // set a layer's Volume, then went back to Home, where knob 1 also
    // controls Master Volume): that's exactly the case this exists to
    // guard against. DrawLayerScreen()/DrawHome() show each page's live
    // value specifically so it's visible what you're sweeping the knob
    // *to* -- same idea as a Pocket Operator's knob catch behavior.
    // Returns true if the caller should apply `raw` this tick.
    static bool KnobPickUp(float raw, float& stored_raw, bool& engaged);
    // Shared by Global:Speed's knob path (ApplyKnobs()) and its Button2
    // reset-to-1.0x -- two call sites, unlike most other global knobs
    // which only ever get set from one place.
    void SetProjectSpeed01(float v);
    // Seeds k1_pickup_raw_/k2_pickup_raw_[ctx] with this context's actual
    // current value(s) (e.g. Cur().GetFilterCutoff01() for LayerFilter),
    // called right where ApplyKnobs() re-arms `engaged` on a context
    // change. Without this, `stored_raw` is left at whatever it was last
    // set to for a *different* layer sharing this same context (or its
    // default of 0 if never engaged this session) -- so the knob would
    // need sweeping to that stale/zero position to re-engage instead of
    // to the value actually shown on screen.
    void SyncPickupTargets(KnobContext ctx);

    void HandleEncoder(const UiControlEvents& events);
    void HandleButton1(const UiControlEvents& events);
    void HandleButton2(const UiControlEvents& events);
    void OnButton1Short();
    void OnButton1Long();
    void OnButton1Release();
    void OnButton2Short();
    void ApplyKnobs();
    void UpdateLeds();
    void Draw();
    void DrawHome();
    void DrawLayerScreen();
    void DrawGlobalScreen();
    void DrawFileScreen();
    void DrawSdMgmtScreen();
    static const char* SdMgmtFolderName(SdMgmtFolder f);
    void DrawExportScreen();
    void DrawSpeedScreen();
    void DrawGranularScreen();
    void DrawMixerScreen();
    void DrawDexedScreen();
    // Screen::Mixer's own per-channel accessors -- channel index 0..3 is
    // Layer 1..4, 4 is Grains, 5 is Bypass, 6 is Master.
    // Kept as small indexed switches rather than a polymorphic interface
    // since there are only 7 cases and they already read/write each
    // engine's own real getters/setters directly -- no new state of its
    // own, this is purely another view onto values that already exist.
    static constexpr int kNumMixerChannels = 8;
    // Total encoder "stops" in Screen::Mixer -- the 8 real channels above
    // plus the Scope stop (mixer_position_ == kNumMixerChannels). See
    // Screen::Mixer's own comment.
    static constexpr int kNumMixerPositions = kNumMixerChannels + 1;
    const char* MixerChannelName(int ch) const;
    float       MixerGetVolume01(int ch) const;
    float       MixerGetPan01(int ch) const; // Master (ch 7) has none -- returns 0.5f
    float       MixerGetSend01(int ch) const; // Master (ch 7) has none -- returns 0.f
    void        MixerSetVolume01(int ch, float v01);
    void        MixerSetPan01(int ch, float v01);   // no-op on Master
    void        MixerSetSend01(int ch, float v01);  // no-op on Master
    // True for every channel except Master and DXD (ch 5) -- Dexed has no
    // Pan control yet (a later increment), so its Mixer channel shows/
    // edits Volume and Reverb Send like any other channel but leaves Pan
    // out entirely instead of displaying a value that doesn't do anything.
    bool        MixerChannelHasPan(int ch) const;
    // Small helper for the Detail page's vertical bars.
    void DrawMixerVBar(int x0, int y0, int w, int h, float v01);
    // All 8 channels' names across one row, each with a real vertical
    // Volume bar (no numeric readout) -- GlobalPage::Mixer's at-a-glance
    // summary (see DrawGlobalScreen()). `top_y` is the name row's
    // baseline.
    void DrawMixerOverviewGrid(int top_y);
    // Attack/decay/sustain/release graph, four fixed-equal-width zones
    // (so turning one knob never visibly shifts another stage that
    // didn't change) -- shared by GranularParamPage::EnvAD/EnvSR, given
    // the actual curved seconds and sustain01 rather than reading an
    // engine pointer directly, so this stays reusable if anything else
    // ever wants the same graph.
    void DrawAdsrShape(int   top,
                        int   bottom,
                        float attack_s,
                        float decay_s,
                        float sustain01,
                        float release_s);
    // Live auto-scaled waveform trace from a small ring buffer -- shared
    // by whichever page wants a live timbre reference (Grains' own
    // Filter page shows its actual output changing as you turn its
    // knobs). Takes the buffer/capacity explicitly rather than reading a
    // fixed member, same reusability reasoning as DrawAdsrShape above.
    void DrawOscilloscope(int top, int bottom, const float* buf, size_t capacity);
    // Downsampled peak-bar waveform + optional moving playhead, shared by
    // Layer:Status (one layer's own GetWaveformPeaks()/GetPlayPos01()) and
    // Global:Speed (a composite max across all 4 layers) so the drawing
    // code exists exactly once.
    void DrawWaveform(const float* peaks, bool draw_playhead, float playhead_pos01);
    // Full-sample waveform for Screen::Granular's Grain page -- same
    // downsampled peak-bar technique as DrawWaveform() above, but with
    // TWO live markers instead of one (the Grain layer's fixed anchor
    // and the Scan layer's sweeping one) and its own vertical geometry,
    // since this page also needs room above the waveform for the live
    // Size/Fill/Gap/Scan readout that DrawWaveform()'s other callers
    // don't have. A separate function rather than extending DrawWaveform()
    // itself so Layer:Status/Global:Speed's existing, working display
    // can't regress.
    void DrawGranularWaveform(const float* peaks,
                                float        grain_anchor01,
                                float        scan_anchor01,
                                bool         has_source);
    // Encoder rotation while Global:Speed's scrub mode is on (see
    // scrub_mode_active_) -- nudges every non-empty layer's play_pos_ by
    // the same raw-sample amount, keeping them all pointing at the same
    // shared timeline position, like scratching a physical tape loop.
    void ScrubBy(int32_t inc);
    // Encoder click anywhere on the Global screen: toggles every layer
    // currently Playing to Paused, or every layer currently Paused back
    // to Playing (Empty/Recording/ArmedCountIn/Overdubbing layers are
    // left alone) -- a single-control pause/resume for the whole
    // performance, e.g. to freeze everything mid-song. Reuses each
    // layer's existing Paused state (LooperLayer::SetPaused()), which
    // already keeps play_pos_ advancing silently so resuming stays
    // phase-locked to the beat grid instead of drifting.
    void TogglePauseAll();

    // --- SD save/load (Global:File page) -----------------------------
    // Refreshes file_slots_/file_slot_count_ from the card -- called
    // whenever the File page is entered and after a Save (a fresh slot
    // may now exist). Cheap (kMaxSlots f_stat() calls, only on a real SD
    // card access, not every frame).
    void RefreshFileSlots();
    // Button1 short: save the current in-memory performance. Overwrites
    // loaded_slot_ if a performance is currently loaded/was just saved;
    // otherwise saves to the next free slot and adopts it as
    // loaded_slot_ (see loaded_slot_'s comment). Button2 short
    // (force_new=true): always saves to a brand new slot regardless of
    // loaded_slot_, without touching whatever's currently loaded -- same
    // "never overwrites/destroys anything, plain tap is safe" class as
    // TriggerExport(), so this needs no hold-to-confirm either.
    void TriggerSave(bool force_new = false);
    // Global:File's own Load chooser confirmed with "New" highlighted
    // (see TriggerLoad() and load_new_selected_'s own comment) -- clears
    // every layer's audio (keeping tempo/global/per-layer settings, same
    // as the per-layer Clear()) and forgets loaded_slot_, so the next
    // Save lands in a new slot rather than overwriting whatever was
    // loaded -- the prior save on the card is untouched either way.
    void TriggerNew();
    // Button2 long-hold (800ms): confirms whatever the Load chooser has
    // settled on -- delegates to TriggerNew() if "New" is highlighted,
    // otherwise loads file_slots_[file_cursor_] (drilled into via
    // Button1, see load_browsing_files_'s own comment), replacing every
    // layer's audio and all settings with the saved performance's.
    void TriggerLoad();

    // --- SD MGMT (Global:SdMgmt page) ------------------------------------
    // Returns whichever of file_slots_/granular_preset_user_slots_
    // applies to sd_mgmt_folder_ (refreshing it first if that folder's
    // own dirty flag is set) and writes its count to *out_count.
    const int* SdMgmtSlots(int* out_count);
    // Button1 hold (800ms) while browsing a folder's files: duplicates
    // the browsed file via PerformanceStore::Duplicate*() -- same
    // "WORKING..." full-screen progress overlay as Save/Load for
    // whichever category can actually take real time (Performances,
    // Grains Presets).
    void TriggerSdMgmtDuplicate();
    // Button2 hold (see kSdMgmtDeleteHoldMs -- longer than the usual
    // 800ms, since unlike every other hold-to-confirm gesture in this
    // project, delete has no "just re-save/re-load" undo).
    void TriggerSdMgmtDelete();
    // Global:Tempo's Button2 held 800ms: saves the current global
    // settings (BPM/Bars/Volume/Metro/Filter/Reverb size/Bypass --
    // deliberately no per-layer settings) as the startup default -- see
    // ApplyStartupDefaults() and PerformanceStore::SavePrefs(). A hold,
    // not a tap, since this is a deliberate write, same weight as
    // Global:File's hold-to-Load/Layer:Status's hold-to-Clear.
    void TriggerSaveDefaults();
    // Button1 short press on Global:Export: renders one full loop of the
    // current in-memory performance (every layer's real filter/effect/
    // reverb chain, plus the master filter) to a new WAV/EXPnnn.wav
    // file at the Pod's native 48kHz. Simple tap, no hold-to-confirm --
    // unlike Save, this never overwrites anything, it only ever creates a
    // new numbered file.
    void TriggerExport();
    // Button2 short press on Global:Export: same render as TriggerExport()
    // but resampled to 44100 Hz and written under custom/EXPnnn.wav
    // instead of WAV/EXPnnn.wav -- see PerformanceStore::ExportWav()'s
    // for_microdexed param. Plain tap, no hold-to-confirm, same reasoning
    // as TriggerExport() (never overwrites anything).
    void TriggerExportMicroDexed();
    static void OnSaveLoadProgress(float progress01); // PerformanceStore::ProgressFn

    // Instant copy of whichever loop layer granular_capture_source_
    // currently names (independent of Home's own cursor_layer_) into
    // Grains' own capture buffer, up to granular_capture_capacity_, then
    // points the engine at it via SetSource(). Runs from the main loop
    // (HandleButton2()), not the audio ISR -- a few hundred microseconds
    // to low milliseconds of copying is fine there, same reasoning as
    // SetSource()'s own waveform-peak rebuild.
    void TriggerGranularCaptureFromLayer();
    // Re-scans IMPORT/ on the SD card for .wav files -- called once on
    // entry to the Capture page's Import source (mirrors
    // RefreshGranularPresetSlots()'s own "dirty flag, refresh on entry"
    // idiom).
    void RefreshGranularImportFiles();
    // Loads/converts whichever file granular_import_cursor_ is currently
    // browsing into Grains' capture buffer (see
    // PerformanceStore::ImportWav()). Runs from the main loop with a live
    // progress overlay (Ui::OnSaveLoadProgress()), same as a Preset
    // load -- this involves real SD reads and per-sample conversion
    // (and possibly resampling), not an instant copy.
    void TriggerGranularImport();
    // Call whenever a new capture/load completes (Direct Record
    // finalizing, TriggerGranularCaptureFromLayer(),
    // TriggerLoadGranularPreset()) -- records the just-captured length as
    // the trim reference, rebuilds granular_trim_full_peaks_ over it, and
    // resets the trim points to 0..1 (no trim) so an old trim range never
    // silently carries over onto an unrelated new capture.
    void OnNewGranularCapture(size_t full_len);
    // Recomputes the sub-range passed to granular_->SetSource() from
    // granular_trim_start01_/end01_ against granular_capture_full_len_
    // (enforcing a minimum gap so the two points can't cross), and
    // applies it. The underlying buffer is untouched -- purely a matter
    // of which sub-range SetSource() currently points into -- so this can
    // be called repeatedly as the knobs move without losing anything.
    void ApplyGranularTrim();
    void RefreshGranularPresetSlots();
    // Button1 short tap (force_new=false): smart save -- overwrites
    // granular_loaded_preset_slot_ if it names a real slot, else a new
    // one. Button2 short tap (force_new=true): always a new slot. Both
    // save the CURRENTLY LOADED capture audio (via granular_->GetSourceL/
    // R()/GetSourceLen()) alongside the parameters -- same non-destructive
    // reasoning as TriggerSave()'s own force_new.
    void TriggerSaveGranularPreset(bool force_new = false);
    // Button2 long-hold (800ms): confirms whatever the Load chooser has
    // settled on -- delegates to TriggerNewGranularPreset() if "New" is
    // highlighted, otherwise loads whichever preset
    // granular_preset_cursor_ is currently browsing (drilled into via
    // Button1, see load_browsing_files_'s own comment), replacing both
    // the live parameters AND the currently loaded capture audio.
    void TriggerLoadGranularPreset();
    // Grains Preset's own Load chooser confirmed with "New" highlighted
    // -- clears the captured audio and resets every param to the
    // engine's own defaults, same fresh-start spirit as Global:File's
    // own TriggerNew().
    void TriggerNewGranularPreset();

    // --- Dexed presets (DexedParamPage::Preset) --------------------------
    // Two-level folder browsing (factory categories by real sound type,
    // plus a trailing "User" folder of SD saves) -- same shape as the
    // removed FmSynth's own preset browser (see
    // dexed_preset_folder_cursor_/dexed_preset_folder_open_'s own
    // comments), generalized to DexedSynth::kNumFactoryCategories
    // (11, real sound-type folders) instead of a fixed 8.
    void RefreshDexedPresetSlots();
    int  ResolveDexedPresetSlot() const;
    // Button1 short tap (force_new=false): smart save -- overwrites
    // dexed_loaded_preset_slot_ if it names a real (non-factory) slot,
    // else a new one. Button2 short tap (force_new=true): always a new
    // slot. Same non-destructive reasoning as TriggerSave()'s own
    // force_new.
    void TriggerSaveDexedPreset(bool force_new = false);
    // Load chooser confirmed with "New" highlighted -- applies factory
    // preset 0, same fresh-start convention as the removed
    // TriggerNewFmPreset().
    void TriggerNewDexedPreset();
    // Short tap while a preset is highlighted inside an open folder:
    // applies it immediately to the live engine WITHOUT leaving the
    // browser (live preview, scrolling K1 + tapping B2 auditions one
    // preset after another) -- the SAME function as the hold-to-commit
    // gesture, which additionally exits back to Idle. Confirmed as the
    // real behavior of the removed FmSynth's own preset browser (not a
    // separate non-committing "audition" primitive), reused as-is here.
    void TriggerLoadDexedPreset();
    // Two-row control legend, drawn at the bottom of every screen in
    // Tom Thumb (see font_tomthumb.h): a knob row (circle icon) and a
    // button row (square icon), each with a label flush to the screen
    // edge on either side and, for the knob row, a live value hugging the
    // icon. `left_value`/`right_value` may be "" to mean that side's
    // control does nothing on this page/state -- draw nothing rather than
    // a fake value (e.g. Speed/Gain/File pages only drive knob 1).
    // `square_icon` false draws a circle (knob row), true a square
    // (button row). `row_y` is the row's top pixel -- see kFooterRow1Y/
    // kFooterRow2Y below for the two fixed positions every screen uses.
    // `divider_y` draws a 1px horizontal rule at that y (pass -1 for no
    // divider). The two footer dividers use different gaps -- the
    // body/footer separator sits immediately above the knob row (0px
    // gap), while the knob/button row separator sits centered in a 3px
    // gap (1px blank, the line, 1px blank) -- so this takes the already-
    // resolved y-coordinate rather than deriving one from row_y, which
    // would get the second case wrong. See the k*Y constants below.
    // Every Font_6x8 label/value/status string on screen goes through
    // this instead of disp_->WriteString() directly, so it all renders
    // uppercase -- non-letters (digits, ':', '.', '%', ...) pass through
    // toupper() unchanged. Tom Thumb (the footer legend) is untouched --
    // it goes straight through DrawControlRow()/TomThumbDrawText(), never
    // this. Truncates past 63 chars (fine -- nothing drawn on this
    // 128px-wide display is remotely that long).
    void WriteUpper(const char* text);
    void DrawControlRow(int         row_y,
                         bool       square_icon,
                         int        divider_y,
                         const char* left_label,
                         const char* left_value,
                         const char* right_value,
                         const char* right_label);
    // Fixed footer geometry, verified against the real Tom Thumb metrics
    // (row_h=6, baseline at the row's bottom pixel, icon vertically
    // centered on the rows text actually occupies -- see DrawControlRow()
    // for why these specific numbers). On a 128x64 display: divider line
    // y46, 1px gap, knob row y48-53, divider line y55, button row y57-62,
    // 1px empty margin y63. Both dividers sit with a 1px blank row on
    // either side of them, same convention -- kFooterDividerY is NOT
    // kFooterRow1Y-1 for that reason (confirmed on real hardware: that
    // adjacent placement reads as the line touching the row, no gap).
    static constexpr int kFooterDividerY      = 46; // body/footer separator (row1's divider)
    static constexpr int kFooterRow1Y         = 48; // knob row
    static constexpr int kFooterInterRowDividerY = 55; // row1/row2 separator (row2's divider)
    static constexpr int kFooterRow2Y         = 57; // button row
    // Live "where am I" indicator, drawn on every screen: one dot per
    // beat in the bar (current beat filled), plus a "B<n>" bar-in-loop
    // label when Bars > 1. Reads TempoClock::GetBeatInBar()/
    // GetBarInLoop() directly -- both tick regardless of whether the
    // metronome click is audible. `x`/`y` is the top-left of the "B<n>"
    // label's reserved space (a fixed width for up to "B16", so the
    // dots never shift as the bar-number digit count changes);
    // `dot_size` controls how large each beat square is (bigger/more
    // prominent on Home, compact when sharing a title row elsewhere).
    void DrawBeatIndicator(int x, int y, int dot_size);

    LooperLayer& Cur() { return layers_[cursor_layer_]; }

    daisy::DaisyPod*               pod_     = nullptr;
    daisy::OneBitGraphicsDisplay*  disp_    = nullptr;
    TempoClock*                    tempo_   = nullptr;
    LooperLayer*                   layers_  = nullptr;
    int                            num_layers_ = 0;

    Screen     screen_      = Screen::Home;
    int        cursor_layer_ = 0;
    LayerPage  layer_page_  = LayerPage::Status;
    GlobalPage global_page_ = GlobalPage::Tempo;

    bool encoder_long_fired_ = false;
    bool button1_long_fired_ = false;
    bool button2_long_fired_ = false;

    // Knob pickup state, one slot per KnobContext (see ApplyKnobs()).
    static constexpr size_t kNumKnobContexts = (size_t)KnobContext::kCount;
    KnobContext last_knob_context_                      = KnobContext::Home;
    float       k1_pickup_raw_[kNumKnobContexts]        = {};
    float       k2_pickup_raw_[kNumKnobContexts]        = {};
    bool        k1_pickup_engaged_[kNumKnobContexts]    = {};
    bool        k2_pickup_engaged_[kNumKnobContexts]    = {};

    // Last-committed raw pot reading, deadbanded in ApplyKnobs() against
    // AnalogControl's own filtered output -- that filter is a fast 2ms
    // slew meant to declick ADC steps, not reject the pot's own resting
    // noise/drift, so without this every knob-driven parameter slowly
    // wobbles even when the knob isn't touched.
    float k1_committed_ = 0.f;
    float k2_committed_ = 0.f;

    // Starting master_volume_ is computed properly through the same
    // curve ApplyKnobs() uses (see Ui::Init()), not hardcoded here, so
    // it stays consistent if that curve ever changes.
    float master_volume_   = 1.f;
    float master_volume01_ = 0.5f; // raw 0..1 last set by the knob, for save/restore
    // On by default -- overridden by a saved startup default if one
    // exists (see ApplyStartupDefaults()/PerformanceStore::LoadPrefs()),
    // same as every other field here.
    bool  bypass_          = true;

    FilterMode master_filter_mode_      = FilterMode::Off;
    float      master_filter_cutoff01_  = 0.5f;
    float      master_filter_res01_     = 0.f;

    float reverb_size01_ = 0.6f; // shared reverb bus's Size/decay, see GetReverbSize01()
    float bypass_reverb_send01_ = 0.f; // see GetBypassReverbSend01()
    float bypass_mix_volume01_  = 0.8f; // see GetBypassMixVolume01()
    float bypass_mix_volume_    = 1.f;  // curved, set properly in the ctor below
    float bypass_pan01_         = 0.5f; // see GetBypassPan01()

    // Global:Speed -- live-performance controls, deliberately NEVER
    // persisted (Save/Load/PREFS.DAT) -- same treatment as master volume.
    // 0.5f defaults to exactly 1.0x via SpeedCurve01()'s dead zone, so no
    // extra Init()-time recompute is needed the way master_volume_ needs.
    float project_speed01_   = 0.5f; // raw 0..1
    float project_speed_     = 1.f;  // actual multiplier, read every audio block
    bool  scrub_mode_active_ = false; // Global:Speed only -- see HandleEncoder()
    uint32_t last_scrub_tick_ms_ = 0; // for ScrubBy()'s turn-speed acceleration

    bool loop_paused_ = false; // see TogglePauseAll()

    // --- Per-engine on/off (see IsGranularEnabled()).
    bool granular_enabled_ = false;
    bool looper_enabled_   = true; // see IsLooperEnabled()'s own comment for why true
    // Same false default as Granular's own now that Global:Dexed's real
    // on/off toggle exists (was temporarily true during Phase 4-6, back
    // when there was no UI toggle yet and defaulting off would have made
    // Dexed's own real MIDI dispatch unreachable/unheard).
    bool dexed_enabled_    = false;

    uint32_t draw_counter_ = 0; // throttles the (slow, blocking-I2C) OLED redraw

    // See SaveLoadMode's own comment -- shared by Global:File and Grains
    // Preset. Reset to Idle whenever the page/screen
    // changes (see HandleEncoder()'s rotate handling), so leaving one of
    // these three pages never leaves it stuck mid-flow for the next visit.
    SaveLoadMode save_load_mode_ = SaveLoadMode::Idle;
    bool         save_as_new_    = false; // ChoosingSave's own Knob1 pick

    // BrowsingLoad's own two-level structure, mirroring ChoosingSave's
    // Overwrite/Save New chooser: entering Load first shows a top-level
    // "Files" vs "New" pick (Knob1), same as ChoosingSave's own pick --
    // load_new_selected_ is that pick, forced true when there's nothing
    // to browse (same "can't select what doesn't exist" reasoning as
    // Overwrite's own force-Save-New-when-nothing-loaded). Button1
    // tapped while "Files" is picked drills in (load_browsing_files_ =
    // true), handing Knob1 to the actual numbered list instead; Button1
    // tapped anywhere else in Load backs out one level (to the chooser,
    // or to Idle from the chooser itself) -- see OnButton1Short(). Reset
    // alongside save_load_mode_ (both wherever it's set to Idle and
    // wherever it's set to BrowsingLoad fresh).
    bool load_new_selected_   = false;
    bool load_browsing_files_ = false;

    // --- SD save/load (Global:File page) -----------------------------
    static constexpr int kMaxFileSlots = 99; // must match PerformanceStore::kMaxSlots
    int  file_slots_[kMaxFileSlots] = {};    // existing slot numbers, ascending
    int  file_slot_count_           = 0;
    int  file_cursor_               = 0;     // index into file_slots_, browsed by knob1
    // Which slot the in-memory performance currently corresponds to, or
    // -1 if it's a fresh/never-saved (or just-"New"'d) performance. Save
    // overwrites this slot when set; when -1, Save adopts the next free
    // slot and this becomes that slot number. See TriggerSave()/
    // TriggerNew()'s comments.
    int  loaded_slot_               = -1;
    bool file_slots_dirty_          = true; // forces one RefreshFileSlots() on first Draw()
    bool file_op_in_progress_       = false; // true only while inside Save()/Load()
    char file_status_[24]           = {};    // last result, shown briefly on the page

    // --- SD MGMT (Global:SdMgmt page) ------------------------------------
    // Reuses file_slots_/granular_preset_user_slots_ (and their own dirty
    // flags/Refresh*() calls) as the file list for whichever folder is
    // selected -- no separate copy of the same scan.
    // Delete gets a longer hold than every other confirm gesture in this
    // project (800ms elsewhere) -- unlike an overwrite or a wipe, there's
    // no "just re-save/re-load" undo for it.
    static constexpr float kSdMgmtDeleteHoldMs = 1500.f;
    SdMgmtFolder sd_mgmt_folder_    = SdMgmtFolder::Performances;
    bool         sd_mgmt_in_folder_ = false; // false = picking a folder, true = browsing its files
    int          sd_mgmt_cursor_    = 0;     // index into the current folder's own slot list
    char         sd_mgmt_status_[24] = {};   // last duplicate/delete result

    // --- WAV export (Global:Export page) ------------------------------
    bool export_op_in_progress_     = false; // true only while inside ExportWav()
    char export_status_[24]         = {};    // last result, shown briefly on the page

    // --- Startup defaults (Global:Tempo page) -------------------------
    char tempo_status_[24]          = {};    // last "save as default" result, shown briefly here

    // --- Granular engine (Screen::Granular) -----------------------------
    GranularEngine*    granular_             = nullptr;
    GranularParamPage  granular_param_page_  = GranularParamPage::Grain;
    // false = knobs control Size+Fill, true = Gap+Scan -- see
    // GranularParamPage::Grain's Button1/Button2 handling.
    bool granular_grain_target_gap_scan_ = false;
    // false = knobs control Attack+Decay, true = Sustain+Release -- see
    // GranularParamPage::ADSR's Button1/Button2 handling.
    bool granular_adsr_target_sr_ = false;
    // false = knobs control Grain+Scan volume, true = Reverb Send -- see
    // GranularParamPage::Mix's Button1/Button2 handling.
    bool granular_mix_target_reverb_ = false;
    // Live output ring buffer for GranularParamPage::Filter's
    // oscilloscope -- same "engine writes samples, main.cpp owns the
    // buffer" convention as pad_scope_buf_ above.
    const float* granular_scope_buf_      = nullptr;
    size_t       granular_scope_capacity_ = 0;

    // --- Mixer (Screen::Mixer) ------------------------------------------
    // Live post-fader master mix ring buffer (captured in main.cpp right
    // after the master filter/click/master-volume stage -- the actual
    // final signal, not any one instrument's own output) for the
    // Mixer's own oscilloscope page. Same ownership convention as
    // pad_scope_buf_/granular_scope_buf_ above.
    const float* master_scope_buf_      = nullptr;
    size_t       master_scope_capacity_ = 0;
    // 0..3 = Layer 1..4, 4 = Plaits, 5 = Grains, 6 = Bypass, 7 = Master,
    // 8 = Overview, 9 = Scope -- see kNumMixerChannels/kNumMixerPositions/
    // MixerChannelName(). Changed by encoder rotate while on
    // Screen::Mixer (see HandleEncoder()'s own comment on why that needs
    // an explicit pickup reseed, unlike every other rotate-driven index
    // in this project).
    int  mixer_position_       = 0;
    // false = knobs control Volume+Pan, true = Reverb Send -- ignored on
    // Master and on the Overview/Scope stops (see MixerVolPan/
    // MixerReverb/MixerMaster/MixerNoKnobs's own KnobContext comment).
    bool mixer_target_reverb_  = false;

    // --- Grains capture (GranularParamPage::Capture) --------------------
    // Mutable (not const, unlike granular_scope_buf_ above) -- Ui writes
    // into these directly for the From-Layer copy, and main.cpp's
    // AudioCallback() writes into them sample-by-sample for Direct
    // Record. See main.cpp's g_granular_capture_l/r and
    // g_granular_capturing's own comments for the full split.
    float*           granular_capture_buf_l_      = nullptr;
    float*           granular_capture_buf_r_      = nullptr;
    size_t           granular_capture_capacity_   = 0;
    volatile bool*   granular_capturing_          = nullptr;
    volatile size_t* granular_capture_write_pos_  = nullptr;
    // -1 = Direct Record, -2 = Import (a WAV file from IMPORT/ on the SD
    // card), 0..num_layers_-1 = pull from that loop layer (independent of
    // cursor_layer_/Home's own selection) -- Button1 cycles through all
    // of these on the Capture page: Direct, Layer 1, Layer 2, Layer 3,
    // Layer 4, Import, back to Direct.
    int granular_capture_source_ = -1;
    // Edge-detection for Direct Record's press-to-start/release-to-stop
    // gesture (see HandleButton2()) -- also catches the ISR's own
    // auto-stop when the 5s buffer fills while Button2 is still held.
    bool granular_was_capturing_ = false;
    char granular_capture_status_[24] = {}; // last From-Layer/Import result

    // --- Grains WAV import (granular_capture_source_ == -2) -------------
    static constexpr int kMaxImportFiles = 16;
    // 61, matching PerformanceStore::kMaxImportWavNameLen+1 -- can't
    // reference that constant directly here since this header doesn't
    // include performance_store.h (only ui.cpp does, same reasoning as
    // GranularEngine being forward-declared instead of fully included).
    char granular_import_names_[kMaxImportFiles][61] = {};
    int  granular_import_file_count_ = 0;
    // Same "no factory range, browses a list directly" idiom as
    // granular_preset_cursor_ -- no continuous knob, just an index.
    int  granular_import_cursor_       = 0;
    bool granular_import_files_dirty_  = true; // forces one RefreshGranularImportFiles() on entry

    // --- Grains trim (GranularParamPage::Trim) --------------------------
    // The true, untrimmed length of whatever was most recently captured/
    // loaded (Direct Record, From Layer, or a Preset) -- GranularEngine's
    // own GetSourceLen() instead reflects whatever sub-range is CURRENTLY
    // active, so this is tracked separately as the reference the trim
    // points are computed against. Reset to 0..1 (no trim) every time a
    // new capture/load completes.
    size_t granular_capture_full_len_ = 0;
    float  granular_trim_start01_     = 0.f;
    float  granular_trim_end01_       = 1.f;
    // Peaks over the FULL untrimmed capture (see GranularParamPage::Trim's
    // own comment) -- separate from GranularEngine::GetWaveformPeaks(),
    // which only ever reflects whatever sub-range is currently active.
    // 63, matching GranularEngine::kWaveformCols -- can't reference that
    // constant directly here since GranularEngine is only forward-
    // declared in this header (only ui.cpp includes the real definition).
    float granular_trim_full_peaks_[63] = {};

    // --- Grains presets (GranularParamPage::Preset) ---------------------
    static constexpr int kMaxGranularPresetSlots = 99; // must match PerformanceStore::kMaxGranularPresets
    int  granular_preset_user_slots_[kMaxGranularPresetSlots] = {}; // SD user slots, ascending
    int  granular_preset_user_slot_count_                      = 0;
    // No factory range -- every listed slot is a real user save, so this
    // browses granular_preset_user_slots_ directly.
    int  granular_preset_cursor_       = 0;
    bool granular_preset_slots_dirty_  = true; // forces one RefreshGranularPresetSlots() on entry
    // -1 = nothing loaded yet / current sound doesn't match a saved slot.
    int  granular_loaded_preset_slot_  = -1;
    char granular_preset_status_[24]   = {}; // last save/load result

    // --- Dexed engine (Screen::Dexed) ------------------------------------
    DexedSynth*    dexed_            = nullptr;
    DexedParamPage dexed_param_page_ = DexedParamPage::Algo;

    // --- Dexed presets (DexedParamPage::Preset) --------------------------
    // Two-level folder browsing, same shape as the removed FmSynth's own
    // preset browser (fm_preset_folder_cursor_/fm_preset_folder_open_) --
    // dexed_preset_folder_cursor_ is 0..DexedSynth::kNumFactoryCategories-1
    // for a real sound-type category, or ==kNumFactoryCategories itself
    // for the trailing synthetic "User" folder holding every SD-saved
    // slot. dexed_preset_folder_open_: false = K1 scrolls the folder
    // list, true = K1 scrolls presets/slots inside the open folder.
    int  dexed_preset_folder_cursor_ = 0;
    bool dexed_preset_folder_open_   = false;
    // Index WITHIN the open folder (not a flat index) -- resolved to a
    // real 1-based PerformanceStore slot by ResolveDexedPresetSlot().
    int  dexed_preset_cursor_ = 0;
    // PerformanceStore::kMaxDexedPresets (4200) minus the current 3834
    // factory presets (28 folders total) leaves up to 366 possible user
    // slots -- 200 gives real headroom without needing to keep this in
    // exact lockstep with the factory bank's own size.
    static constexpr int kMaxDexedPresetSlots = 200;
    int  dexed_preset_user_slots_[kMaxDexedPresetSlots] = {}; // SD user slots, ascending
    int  dexed_preset_user_slot_count_                   = 0;
    bool dexed_preset_slots_dirty_ = true; // forces one RefreshDexedPresetSlots() on entry
    // Starts at slot 1 (factory preset 0, "ARP 2600"), matching
    // DexedSynth::Init()'s own boot default.
    int  dexed_loaded_preset_slot_ = 1;
    char dexed_preset_status_[24]  = {}; // last save/load result, shown briefly
};
