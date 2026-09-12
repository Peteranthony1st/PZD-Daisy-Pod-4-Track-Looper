#pragma once
#include "daisy_pod.h"
#include "dev/oled_ssd130x.h"
#include "tempo_clock.h"
#include "looper_layer.h"
#include "font_tomthumb.h"

class PadSynth;
class GranularEngine;

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
              PadSynth*                     pad_synth,
              const float*                  pad_scope_buf,
              size_t                        pad_scope_capacity,
              GranularEngine*               granular,
              const float*                  granular_scope_buf,
              size_t                        granular_scope_capacity,
              float*                        granular_capture_buf_l,
              float*                        granular_capture_buf_r,
              size_t                        granular_capture_capacity,
              volatile bool*                granular_capturing,
              volatile size_t*              granular_capture_write_pos);

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
    // Block-rate: tape-style multiplier applied on top of every layer's
    // own Speed and TempoClock's own tick rate -- see main.cpp's
    // AudioCallback(), TempoClock::Process(), LooperLayer::Process().
    // 1.0 (default) = normal.
    float GetProjectSpeed() const { return project_speed_; }

    // Per-engine on/off (Global:Pad / Global:Granular, Button1 tap) --
    // pulled forward from the granular-rewrite plan's Stage 4 once real
    // hardware measurement (4 layers + full 8-voice Pad + Granular, all
    // active) showed CPU headroom was tight enough (~89% worst case) that
    // a real way to free up budget was needed now, not just once
    // Granular's own UI/persistence stages were done. main.cpp's
    // AudioCallback() skips a disabled engine's Process() call entirely
    // (writing silence instead) -- a real CPU saving, not just a mute.
    bool IsPadEnabled() const { return pad_enabled_; }
    bool IsGranularEnabled() const { return granular_enabled_; }
    // Global:Looper, Button1 tap -- a REAL stop, not TogglePauseAll()'s
    // own phase-locked pause: main.cpp's AudioCallback() skips every
    // LooperLayer::Process() call entirely while this is false, so
    // someone using only Plaits/Grains gets that CPU back rather than 4
    // idle-but-still-processing layers. Defaults true (unlike Pad/
    // Granular's own false default) since the loop is this project's
    // original core feature, not an add-on someone opts into.
    bool IsLooperEnabled() const { return looper_enabled_; }

    // TEMPORARY -- CPU diagnostic overlay (see main.cpp's DWT cycle
    // counter). Drawn as part of Draw() itself, not a separate direct
    // display write, so it's redrawn every frame at the same throttled
    // rate as everything else instead of being wiped almost immediately
    // by the next regular redraw (the earlier version's "hard to read"
    // problem). -1 = don't draw (default). Remove once CPU is confirmed
    // safe and this is no longer needed.
    void SetDiagCpuPercent(int pct) { diag_cpu_percent_ = pct; }

  private:
    enum class Screen
    {
        Home,
        Layer,
        Global,
        // A real top-level screen, not a Global page -- entered from
        // Global's own Pad entry-point page (encoder click), exits back
        // to Home via the same long-press-from-any-non-Home-screen path
        // every other screen uses. See PadParamPage for its own pages.
        Pad,
        // Same treatment as Pad above, entered from Global:Granular. See
        // GranularParamPage for its own pages.
        Granular
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
        Export,
        // Entry point into Screen::Pad only -- no continuous knobs of its
        // own, same treatment as every other screen's Global entry page.
        Pad,
        // Entry point into Screen::Granular (not built yet -- Stage 2 of
        // the granular rewrite). For now this page only exposes the
        // on/off toggle pulled forward from Stage 4, same idiom as Pad's
        // own toggle below.
        Granular,
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

    // Screen::Pad's own pages, cycled by encoder rotate while there (same
    // convention as LayerPage/GlobalPage).
    enum class PadParamPage
    {
        Tone,      // Registration (tone morph) + Osc Gain
        // Attack/Decay/Sustain/Release graph, all four stages always
        // shown together -- Button1 maps the knobs to Attack+Decay,
        // Button2 maps them to Sustain+Release (see pad_adsr_target_sr_).
        ADSR,
        Chorus,    // Depth + Rate
        Vibrato,   // Depth + Rate -- ceiling the mod wheel scales up to (see ModDestination)
        Filter,    // Cutoff + Resonance, mode cycled by Button1
        Mix,       // Reverb Send + Output Level -- also reachable from Global:Mixer,
                   // same underlying pad_synth_ value either way, so changing it in
                   // either place updates the other with no extra sync needed.
        ModAssign, // Button1 cycles PadSynth::ModDestination; no knobs
        Preset,    // Save/Load the whole pad sound as a preset
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
        GlobalPad, // entry point only -- see Screen::Pad instead
        GlobalGranular, // entry point only -- see Screen::Granular instead
        GlobalLooper, // no continuous knobs -- Button1 toggle only
        GlobalMixer,
        GlobalMixerReverb,
        PadTone,
        PadEnvAD,
        PadEnvSR,
        PadChorus,
        PadVibrato,
        PadFilter,
        PadMix,
        PadModAssign,
        PadPreset,
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
    void DrawExportScreen();
    void DrawSpeedScreen();
    void DrawPadScreen();
    void DrawGranularScreen();
    // Attack/decay/sustain/release graph, four fixed-equal-width zones
    // (so turning one knob never visibly shifts another stage that
    // didn't change) -- shared by PadParamPage::EnvAD/EnvSR, given the
    // actual curved seconds (see PadSynth::GetAttackSeconds() etc.) and
    // sustain01 rather than reading an engine pointer directly, so this
    // stays reusable if anything else ever wants the same graph.
    void DrawAdsrShape(int   top,
                        int   bottom,
                        float attack_s,
                        float decay_s,
                        float sustain01,
                        float release_s);
    // Live auto-scaled waveform trace from a small ring buffer -- shared
    // by whichever PadParamPage wants a live timbre reference (Tone/
    // Chorus/Filter all show the pad's actual output changing as you
    // turn their knobs). Takes the buffer/capacity explicitly rather
    // than reading a fixed member, same reusability reasoning as
    // DrawAdsrShape above.
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
    // Button1 long-hold (800ms, see HandleButton1FilePage): clears every
    // layer's audio (keeping tempo/global/per-layer settings, same as
    // the per-layer Clear()) and forgets loaded_slot_, so the next Save
    // lands in a new slot rather than overwriting whatever was loaded --
    // the prior save on the card is untouched either way.
    void TriggerNew();
    // Button2 long-hold (800ms): loads file_slots_[file_cursor_],
    // replacing every layer's audio and all settings with the saved
    // performance's.
    void TriggerLoad();
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

    // --- Pad presets (Screen::Pad, PadParamPage::Preset) ----------------
    // Refreshes pad_preset_user_slots_/pad_preset_user_slot_count_ from
    // the card -- same RefreshFileSlots() convention, called on entry to
    // this page and after a save. The kNumFactoryPresets factory presets
    // are always available regardless of card state and aren't part of
    // this scan.
    void RefreshPadPresetSlots();
    // Button1 short: same "smart save" idiom as TriggerSave() -- overwrite
    // pad_loaded_preset_slot_ if it's a user slot, otherwise (nothing
    // loaded, or a read-only factory preset is current) save to a new
    // user slot instead. Button2 short (force_new=true): always a new
    // user slot, same non-destructive reasoning as TriggerSave's own
    // force_new.
    void TriggerSavePadPreset(bool force_new = false);
    // Button2 long-hold (800ms): loads whichever preset pad_preset_cursor_
    // is currently browsing (factory or user), replacing every live pad
    // setting.
    void TriggerLoadPadPreset();
    // Instant copy of whichever loop layer granular_capture_source_
    // currently names (independent of Home's own cursor_layer_) into
    // Grains' own capture buffer, up to granular_capture_capacity_, then
    // points the engine at it via SetSource(). Runs from the main loop
    // (HandleButton2()), not the audio ISR -- a few hundred microseconds
    // to low milliseconds of copying is fine there, same reasoning as
    // SetSource()'s own waveform-peak rebuild.
    void TriggerGranularCaptureFromLayer();
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
    // reasoning as TriggerSave()/TriggerSavePadPreset()'s own force_new.
    void TriggerSaveGranularPreset(bool force_new = false);
    // Button2 long-hold (800ms): loads whichever preset
    // granular_preset_cursor_ is currently browsing, replacing both the
    // live parameters AND the currently loaded capture audio.
    void TriggerLoadGranularPreset();
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
    bool  bypass_          = false;

    FilterMode master_filter_mode_      = FilterMode::Off;
    float      master_filter_cutoff01_  = 0.5f;
    float      master_filter_res01_     = 0.f;

    float reverb_size01_ = 0.6f; // shared reverb bus's Size/decay, see GetReverbSize01()
    float bypass_reverb_send01_ = 0.f; // see GetBypassReverbSend01()

    // Global:Speed -- live-performance controls, deliberately NEVER
    // persisted (Save/Load/PREFS.DAT) -- same treatment as master volume.
    // 0.5f defaults to exactly 1.0x via SpeedCurve01()'s dead zone, so no
    // extra Init()-time recompute is needed the way master_volume_ needs.
    float project_speed01_   = 0.5f; // raw 0..1
    float project_speed_     = 1.f;  // actual multiplier, read every audio block
    bool  scrub_mode_active_ = false; // Global:Speed only -- see HandleEncoder()
    uint32_t last_scrub_tick_ms_ = 0; // for ScrubBy()'s turn-speed acceleration

    bool loop_paused_ = false; // see TogglePauseAll()

    // --- Per-engine on/off (see IsPadEnabled()/IsGranularEnabled()) ---
    bool pad_enabled_      = false;
    bool granular_enabled_ = false;
    bool looper_enabled_   = true; // see IsLooperEnabled()'s own comment for why true
    // false = knobs control Output Level (Plaits/Grains), true = Reverb
    // Send -- see GlobalPage::Mixer's Button1/Button2 handling.
    bool global_mixer_target_reverb_ = false;

    uint32_t draw_counter_ = 0; // throttles the (slow, blocking-I2C) OLED redraw

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

    // --- WAV export (Global:Export page) ------------------------------
    bool export_op_in_progress_     = false; // true only while inside ExportWav()
    char export_status_[24]         = {};    // last result, shown briefly on the page

    // --- Startup defaults (Global:Tempo page) -------------------------
    char tempo_status_[24]          = {};    // last "save as default" result, shown briefly here

    // --- Pad synth (Screen::Pad) ---------------------------------------
    PadSynth*    pad_synth_          = nullptr;
    const float* pad_scope_buf_      = nullptr;
    size_t       pad_scope_capacity_ = 0;
    PadParamPage pad_param_page_     = PadParamPage::Tone;
    // false = knobs control Attack+Decay, true = Sustain+Release -- see
    // PadParamPage::ADSR's Button1/Button2 handling.
    bool pad_adsr_target_sr_ = false;

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
    // -1 = Direct Record, 0..num_layers_-1 = pull from that loop layer
    // (independent of cursor_layer_/Home's own selection) -- Button1
    // cycles through all of these on the Capture page: Direct, Layer 1,
    // Layer 2, Layer 3, Layer 4, back to Direct.
    int granular_capture_source_ = -1;
    // Edge-detection for Direct Record's press-to-start/release-to-stop
    // gesture (see HandleButton2()) -- also catches the ISR's own
    // auto-stop when the 5s buffer fills while Button2 is still held.
    bool granular_was_capturing_ = false;
    char granular_capture_status_[24] = {}; // last From-Layer copy result

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
    // No factory range (unlike pad_preset_cursor_) -- every listed slot
    // is a real user save, so this browses granular_preset_user_slots_
    // directly rather than needing PadPresetCursorToSlot()'s own
    // factory/user split.
    int  granular_preset_cursor_       = 0;
    bool granular_preset_slots_dirty_  = true; // forces one RefreshGranularPresetSlots() on entry
    // -1 = nothing loaded yet / current sound doesn't match a saved slot
    // (no factory-preset-0 equivalent here, unlike pad_loaded_preset_slot_).
    int  granular_loaded_preset_slot_  = -1;
    char granular_preset_status_[24]   = {}; // last save/load result

    // --- Pad presets (PadParamPage::Preset) -----------------------------
    static constexpr int kMaxPadPresetSlots = 99; // must match PerformanceStore::kMaxPadPresets
    int  pad_preset_user_slots_[kMaxPadPresetSlots] = {}; // SD user slots, ascending
    int  pad_preset_user_slot_count_                 = 0;
    // Browses the WHOLE list -- factory presets first (index
    // 0..kNumFactoryPresets-1), then user slots
    // (kNumFactoryPresets..kNumFactoryPresets+pad_preset_user_slot_count_-1).
    int  pad_preset_cursor_          = 0;
    bool pad_preset_slots_dirty_     = true; // forces one RefreshPadPresetSlots() on first entry
    // Which slot the live pad settings currently correspond to, or -1 if
    // they've been tweaked since the last load/save (or never loaded at
    // all this session) -- same "unsaved" semantics as loaded_slot_.
    // Starts at 1 ("New", factory preset index 0) since that's exactly
    // what PadSynth::Init() actually applies at boot.
    int  pad_loaded_preset_slot_     = 1;
    char pad_preset_status_[24]      = {}; // last save/load result, shown briefly here

    // TEMPORARY -- see SetDiagCpuPercent() above.
    int diag_cpu_percent_ = -1;
};
