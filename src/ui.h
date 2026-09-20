#pragma once
#include "daisy_pod.h"
#include "dev/oled_ssd130x.h"
#include "tempo_clock.h"
#include "looper_layer.h"
#include "performance_store.h"
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
    // Shared delay bus's Time/Feedback (see main.cpp's fx_delay_l/r) --
    // same "one shared instance, everyone sends into it" relationship to
    // Dexed's/Grains' own Delay Send as Reverb Size has to their Reverb
    // Send. Raw 0..1 -- main.cpp maps Time onto an actual ms value with
    // its own exponential curve, and scales Feedback below 1.0 to avoid
    // runaway self-oscillation in the cross-feedback pair.
    float GetDelayTime01() const { return delay_time01_; }
    float GetDelayFeedback01() const { return delay_feedback01_; }
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
    // someone using only Dexed/Grains gets that CPU back rather than 4
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
        Dexed,
        // The advanced per-operator FM editor -- entered from
        // Screen::Dexed's own Advanced page (encoder click), NOT a
        // Global page of its own. Encoder click here returns to
        // Screen::Dexed (landing back on the Advanced page); long-press
        // still goes all the way back to Home like every other screen.
        // See DexedOpParamPage for its own pages.
        DexedOperator
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
    // Global:Speed's own transport sub-mode, cycled by a Button1 short
    // tap. Normal: encoder rotation does nothing here. Scrub: encoder
    // rotation nudges every layer's play_pos_ together (see ScrubBy()),
    // normal tempo-driven playback keeps advancing underneath. Freeze:
    // encoder rotation moves a small frozen "drone" window instead (see
    // NudgeFreeze()) -- normal playback genuinely stops advancing while
    // active, see LooperLayer::SetFreezeActive()'s own doc comment.
    enum class SpeedTransportMode
    {
        Normal,
        Scrub,
        Freeze
    };
    enum class LayerPage
    {
        Status,
        // Recording input gain (SetInputGain01() -- boosts a quiet mic/
        // line source before capture), not a playback volume control.
        // Sits right after Status, before the playback-shaping pages
        // below (Speed/Filter/Effect/FX), since it only matters before
        // or during recording, not to what's already been captured.
        Gain,
        Speed,
        Filter,
        Effect,
        Reverb,
        kCount
    };
    enum class GlobalPage
    {
        Tempo,
        Speed, // Sits right after Tempo -- both are project-wide timing/transport controls
        Filter,
        Reverb,
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
        // On/off toggle only, same treatment as Granular/Dexed above --
        // but for the whole 4-layer loop system, and a REAL stop (see
        // IsLooperEnabled()'s doc comment), not TogglePauseAll()'s own
        // phase-locked pause. Lets someone using only Dexed/Grains skip
        // all 4 LooperLayer::Process() calls entirely for the CPU back,
        // not just silence.
        Looper,
        // Entry point into Screen::Mixer -- no knobs of its own any more
        // (the real per-engine level controls live there), same "entry
        // point only" idiom as Granular/Dexed above.
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
        // markers, Size/Fill/Gap/Jitter values shown live -- Button1 maps
        // the knobs to Size+Fill, Button2 maps them to Gap+Jitter (see
        // granular_grain_target_gap_jitter_), same toggle idiom as Pad's
        // own merged ADSR page. Jitter lives here (not its own page) --
        // it's a Grain-layer-only refinement, same family as Size/Fill/
        // Gap, not a separate feature; see GranularEngine::SetJitter01()
        // for what it actually does (a fixed Position anchor replays the
        // exact same buffer offset every retrigger, so a source with a
        // strong feature sitting inside the grain's window -- not at its
        // very edge, where the Hann window would silence it -- repeats
        // it identically forever, audible as a perfectly periodic click;
        // Jitter smears consecutive grains across nearby offsets
        // instead -- confirmed via a real exported capture on a
        // sustained pad sample).
        Grain,
        // Knob1 = Position (Grain layer's fixed anchor), Knob2 =
        // Direction (moved here from TuneDirection, below -- that page's
        // K2 slot is now free since this one was, per a real user
        // request). Button1/2 cycle Rhythm/Speed. Sits right after Grain
        // so the Grain layer's own two pages (what plays, then where it
        // reads from) are adjacent.
        Position,
        // Knob1 = Scan (speed/direction, dead-zone-centered mute -- moved
        // here from the Grain page), Knob2 = Scan's own Fill (how many
        // concurrent grains the Scan layer uses, independent of the
        // Grain layer's own Fill -- see GranularEngine::SetScanFill01()).
        // Split into its own page specifically so these two live
        // together: Scan is a genuinely separate layer from Grain (its
        // own anchor, its own density), so it gets its own independent
        // density control instead of being forced to always match
        // whatever the Grain layer's Fill happens to be. Sits right
        // after Position and before ScanRange, below, so the Scan
        // layer's own two pages are adjacent to each other, mirroring
        // Grain/Position's own pairing.
        Scan,
        // Knob1 = Scan Start, Knob2 = Scan End -- the Scan layer's own
        // sweep range, fully adjustable at both ends (see
        // GranularEngine::SetScanEnd01()'s own doc comment). Sits right
        // after Scan specifically since it's that page's own direct
        // sibling -- Scan is "what plays" for that layer, this is "where
        // it reads from," same relationship Position has to Grain.
        // Previously Scan's end was fixed to the buffer's own end and
        // only the start (then called "Scan Position") lived on the
        // Position page's K2; moved out to its own page once End became
        // independently adjustable too, per a real user request.
        ScanRange,
        // Knob1 only = Tune (semitones); Button1 cycles Map-to-Note
        // on/off. Direction (previously this page's own K2) moved to
        // Position's own K2 -- see its own doc comment above.
        Tune,
        // Attack/Decay/Sustain/Release graph, same merged-page idiom as
        // Grain's Size/Fill/Gap/Scan and Pad's own ADSR page.
        ADSR,
        Filter, // Cutoff + Resonance, mode cycled by Button1; live oscilloscope
        Mix,    // Grain layer volume + Scan layer volume
        // Full reverb/delay control from within Grains itself -- same
        // shape/reasoning as DexedParamPage::FX.
        FX,
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
        // Full reverb/delay control from within Dexed itself: Button1/
        // Button2 toggle between Reverb (own Send + shared Size) and
        // Delay (own Send + shared Time) -- Mix's own Reverb Send/Output
        // Level are untouched (this is a separate, additional page, not
        // a replacement -- Reverb Send is genuinely reachable from both
        // places, same "same value, multiple entry points" idiom already
        // established for Reverb Send across Mix/Global Mixer).
        FX,
        // Entry point into Screen::DexedOperator (encoder click) -- no
        // continuous knobs of its own, same "entry point only" idiom as
        // GlobalPage::Granular/Dexed. Positioned right before Preset so
        // the deep-editing detour sits next to the save/load workflow
        // it's meant to feed into, without being the very last page.
        Advanced,
        // Save/Load the whole Dexed sound as a preset, folder-browsing
        // 150+ factory presets by real sound-type category plus a
        // "User" folder of SD saves -- same two-level browsing shape
        // the removed FmSynth's own Preset page used, generalized to
        // more/larger categories.
        Preset,
        kCount
    };

    // Preset page's "Files" branch own group chooser -- one level above
    // the factory-category/User folder list, entered right after
    // picking "Files" from the top-level Save/Load chooser. Added
    // because that flat folder list was already 54 factory categories +
    // User before any SysEx import existed (see
    // DexedSynth::kNumFactoryCategories's own doc comment on why so
    // many small categories exist in the first place -- one knob sweep
    // across a few hundred presets in ONE folder was already
    // unreliable); every completed import now adds one MORE folder
    // (PerformanceStore::DexedImportFolder), so a single flat sweep
    // across ever-growing dozens of folders was never going to scale.
    // Roms: the 4 real "Rom 1".."Rom 4" categories (real factory index
    //   0-3 -- literal unmodified Yamaha ROM cartridge contents).
    // Dexed: the other ~50 curated-by-sound-type categories (real
    //   factory index 4..kNumFactoryCategories-1).
    // Imports: one folder per completed SysEx import (see
    //   PerformanceStore::ListDexedImportFolders()), each holding just
    //   that one import's own contiguous slot range.
    // User: every USER slot that ISN'T part of a recorded import folder
    //   -- hand-saved presets only, filtered in RefreshDexedPresetSlots().
    enum class DexedFilesGroup
    {
        Roms,
        Dexed,
        Imports,
        User,
        kCount
    };

    // Screen::DexedOperator's own pages -- the v1 "core set" advanced
    // per-operator editor scope (Ratio/Level, Detune, and the 4 EG
    // Rates + 4 EG Levels via the existing AD/SR Button2-toggle idiom
    // already used for every ADSR-shaped page in this codebase -- see
    // dexed_op_egrate_target_sr_/dexed_op_eglevel_target_sr_). Keyboard
    // scaling, velocity sensitivity, and amp-mod sensitivity are
    // deliberately deferred to a later increment. Operator select
    // (1-6, shown as the real HW op number, i.e. 6-dexed_op_index_) is
    // Button1-cycled, not a page of its own -- rotate stays reserved
    // for page navigation, matching every other screen's own
    // rotate-cycles-pages convention.
    enum class DexedOpParamPage
    {
        RatioLevel, // K1 = Coarse ratio (quantized 0-31), K2 = Output Level
        Detune,     // K1 only, 0-14 centered on 7
        EgRate,     // K1/K2 = Rate1/Rate2 (AD) or Rate3/Rate4 (SR), Button2 toggles
        EgLevel,    // K1/K2 = Level1/Level2 (AD) or Level3/Level4 (SR), Button2 toggles
        // K1 only, 0-3 (patch[off+14] & 3, ampmodsenstab in msfa) -- gates
        // whether this operator responds to the mod wheel's Amp/EG Bias
        // targets at all (0 = never, regardless of wheel position); see
        // DexedSynth::ModWheelTarget's own comment. Explicitly deferred in
        // the original advanced-editor plan, added once a real user
        // report ("I can hear no effect... on all of the cycles") traced
        // Amp/EG's silence to this byte never being exposed anywhere.
        AmpModSens,
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
        GlobalDelay, // Global:FX's Delay pair (Time/Feedback) -- see global_fx_target_delay_
        GlobalSpeed,
        GlobalFile,
        GlobalExport,
        GlobalGranular, // entry point only -- see Screen::Granular instead
        GlobalLooper, // no continuous knobs -- Button1 toggle only
        GlobalMixer, // entry point only -- see Screen::Mixer instead
        GlobalSdMgmt, // browses a list directly, no pickup used -- see GlobalPage::SdMgmt
        GlobalDexed, // entry point only -- see Screen::Dexed instead
        // Screen::Granular's Grain page -- Button1/Button2 toggle which
        // pair the knobs reach (see granular_grain_target_gap_jitter_).
        GranularGrainSizeFill,
        GranularGrainGapJitter,
        GranularPosition,
        GranularScanRange,
        GranularScanGap,
        GranularScan,
        GranularTune,
        GranularEnvAD,
        GranularEnvSR,
        GranularFilter,
        GranularMix,
        GranularMixReverb,
        // Screen::Granular's own new FX page -- Button1/Button2 toggle
        // between Reverb (own Send + shared Size) and Delay (own Send +
        // shared Time), same shape as GranularMix/GranularMixReverb above
        // but a separate page so Mix's own Grain/Scan volumes don't need
        // to share room with 4 more FX parameters.
        GranularFxReverb,
        GranularFxDelay,
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
        // MOD page's own Button2 toggle -- K1 = patch[143]&7, the global
        // Pitch Mod Sensitivity byte that gates BOTH the automatic
        // Vibrato Depth knob above AND the wheel's Pitch target (see
        // DexedSynth::ModWheelTarget's own comment).
        DexedVibratoSens,
        DexedBrightness,
        DexedEnvSpeed,
        DexedFilter,
        DexedMix,
        // Screen::Dexed's own new FX page -- same shape/reasoning as
        // GranularFxReverb/GranularFxDelay above.
        DexedFxReverb,
        DexedFxDelay,
        DexedAdvanced, // entry point only -- see Screen::DexedOperator instead
        DexedPreset, // browses a list directly, no pickup used
        // Screen::DexedOperator -- one context per sub-page, shared
        // across all 6 operators (branch internally on dexed_op_index_),
        // same "one context, branch on which target" idiom LayerStatus
        // already uses for cursor_layer_/Cur(). EgRate/EgLevel each
        // split into two contexts for the AD/SR toggle, same shape as
        // GranularEnvAD/GranularEnvSR above.
        DexedOpRatioLevel,
        DexedOpDetune,
        DexedOpEgRateAD,
        DexedOpEgRateSR,
        DexedOpEgLevelAD,
        DexedOpEgLevelSR,
        DexedOpAmpModSens,
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
    // True for every channel except Master -- Dexed's own channel (ch 5)
    // used to be excluded here too, back when Dexed had no real Pan
    // control of its own; it now does (see DexedSynth::SetPan01()), so
    // this is just Master's own exception now.
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
    // Real DX7 4-segment operator envelope (R1/L1..R4/L4) -- genuinely
    // different shape from DrawAdsrShape() above, not a reuse of it:
    // each of the 4 equal-width zones ramps from wherever the previous
    // segment left off to its OWN target level (rates/levels[0..3], raw
    // 0-99 patch bytes), holding flat for whatever's left of that zone
    // once it gets there -- unlike a classic ADSR, a real DX7 envelope
    // can rise and fall repeatedly across its 4 stages, so the fixed
    // up/down/flat/down topology DrawAdsrShape() assumes doesn't apply.
    // Higher rate = faster = reaches its target sooner within the zone
    // (the opposite direction from DrawAdsrShape()'s own seconds
    // parameters, where smaller = faster). Shared by
    // DexedOpParamPage::EgRate/EgLevel -- both show the same real shape
    // regardless of which pair is currently the editable one.
    void DrawDx7EnvelopeShape(int top, int bottom, const uint8_t rates[4], const uint8_t levels[4]);
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
    // grain_size01: fraction of the buffer one grain spans (see
    // GranularEngine::GetGrainSizeFraction01()) -- draws the Grain
    // marker as a span this wide, centered on grain_anchor01, instead of
    // a single-pixel tick, so Size is actually visible on the waveform.
    // 0 collapses back to a plain point marker (used by Trim's own reuse
    // of this same function for start/end points, where a size span
    // makes no sense).
    // range_start01/range_end01: when both are >= 0, draws a full-height
    // vertical line at each across the whole waveform band -- used by
    // the Scan Range page to show its own Start/End boundaries directly
    // on the display, separate from (and in addition to) the Grain/Scan
    // anchor ticks above. Negative (either one) disables this entirely,
    // the default for every other caller.
    void DrawGranularWaveform(const float* peaks,
                                float        grain_anchor01,
                                float        scan_anchor01,
                                bool         has_source,
                                float        grain_size01 = 0.f,
                                float        range_start01 = -1.f,
                                float        range_end01   = -1.f);
    // Encoder rotation while Global:Speed's transport mode is Scrub (see
    // speed_transport_mode_) -- nudges every non-empty layer's play_pos_
    // by the same raw-sample amount, keeping them all pointing at the
    // same shared timeline position, like scratching a physical tape loop.
    void ScrubBy(int32_t inc);
    // Encoder rotation while Global:Speed's transport mode is Freeze --
    // same per-layer raw-sample nudge as ScrubBy(), but moves each
    // layer's frozen drone window instead (LooperLayer::NudgeFreeze()),
    // so scrubbing selects which part of the loop the drone plays.
    void NudgeFreezeBy(int32_t inc);
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
    // See DexedFilesGroup's own doc comment for the current 3-level
    // (group -> folder -> preset) shape -- was originally a flat 2-level
    // folder browser like the removed FmSynth's own, generalized further
    // once SysEx import made the flat folder list grow unbounded.
    // Re-scans BOTH the User slot list (filtered, see
    // dexed_preset_user_slots_'s own comment) and the import-folder
    // manifest (dexed_import_folders_) together -- the former's
    // filtering depends on the latter being current first.
    void RefreshDexedPresetSlots();
    int  ResolveDexedPresetSlot() const;
    // Real flat 0..(kNumFactoryCategories-1) factory category index for
    // the current dexed_files_group_ + dexed_preset_folder_cursor_
    // selection -- valid only when the group is Roms or Dexed (Imports/
    // User have no factory-category mapping at all, see
    // ResolveDexedPresetSlot()'s own per-group branches). Roms group
    // cursor 0-3 maps directly (already real); Dexed group cursor
    // 0..(kNumFactoryCategories-5) is offset by the 4 Roms categories
    // that come first in DexedSynth::kDexedFactoryCategories's own table.
    int ResolveDexedRealCategory() const;
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
    // Re-scans DXIMPORT/ on the SD card for .syx files -- called once on
    // entry to the Preset page's own Import file list, same pattern as
    // RefreshGranularImportFiles().
    void RefreshDexedImportFiles();
    // Hold-to-confirm-then-fire-once (see HandleButton2()'s own Dexed
    // Preset handling, same weight as TriggerGranularImport()'s own real
    // SD read). Reads/decodes whichever file dexed_import_cursor_ points
    // at (PerformanceStore::ImportDexedSyx()) and writes every voice it
    // contains into new, consecutively-numbered Dexed USER slots -- a
    // Single Voice Dump becomes one new slot, a 32-Voice Bulk Dump
    // becomes 32. The LAST voice imported is also applied live
    // (DexedSynth::ApplyPreset()) and becomes dexed_loaded_preset_slot_,
    // the same "land on something real" feel as TriggerLoadDexedPreset()'s
    // own preview -- called from there when dexed_import_selected_ is
    // set, so every existing Button1/Button2 call site reaches this
    // automatically with no per-call-site branching needed.
    void TriggerDexedSyxImport();
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
    float delay_time01_     = 0.3f; // shared delay bus's Time, see GetDelayTime01()
    float delay_feedback01_ = 0.35f; // shared delay bus's Feedback, see GetDelayFeedback01()
    // Global:FX's own toggle between the Reverb pair (Size/Bypass Send,
    // existing) and the Delay pair (Time/Feedback, new) -- same "Button1
    // = pair A, Button2 = pair B" idiom as Grain's Gap+Scan toggle.
    bool global_fx_target_delay_ = false;
    // Same toggle, one per engine's own new FX page (Reverb Send+Size vs
    // Delay Send+Time) -- independent of Global:FX's own toggle above
    // and of each other.
    bool dexed_fx_target_delay_    = false;
    // MOD page's own Button2 toggle -- see KnobContext::DexedVibratoSens.
    bool dexed_vibrato_target_sens_ = false;
    bool granular_fx_target_delay_ = false;
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
    SpeedTransportMode speed_transport_mode_ = SpeedTransportMode::Normal; // Global:Speed only -- see HandleEncoder()
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
    // false = knobs control Size+Fill, true = Gap+Jitter -- see
    // GranularParamPage::Grain's Button1/Button2 handling.
    bool granular_grain_target_gap_jitter_ = false;
    // false = knobs control Start+End, true = Gap (Scan's own, K1 only)
    // -- see GranularParamPage::ScanRange's Button1/Button2 handling,
    // same two-button pattern as the Grain page's own toggle.
    bool granular_scanrange_target_gap_ = false;
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
    // 0..3 = Layer 1..4, 4 = Grains, 5 = Dexed, 6 = Bypass, 7 = Master,
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
    // Throttles ApplyGranularTrim()'s own waveform-peaks recompute (an
    // O(full capture length) scan) to well below the ~1kHz main-loop/
    // knob-polling rate -- see GranularEngine::SetTrimRange()'s own doc
    // comment for why redoing that scan on every single tick was a real,
    // user-reported cause of Trim's knobs feeling laggy/slow to update.
    uint32_t granular_trim_peaks_throttle_ = 0;
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
    // Three levels now (see DexedFilesGroup's own doc comment for why):
    // group (Roms/Dexed/Imports/User) -> folder (skipped entirely for
    // User, which has no further sub-grouping) -> preset. dexed_files_group_
    // is which group; dexed_files_group_open_: false = K1 picks the
    // group, true = past it. Within a group, dexed_preset_folder_cursor_/
    // dexed_preset_folder_open_ mean "index of the highlighted folder
    // WITHIN this group" / "folder open, K1 now scrolls presets inside
    // it" for Roms/Dexed/Imports -- resolved to a real factory category
    // (see ResolveDexedRealCategory()) or a real PerformanceStore::
    // DexedImportFolder (see dexed_import_folders_) depending on the
    // group. User has no folder level at all -- dexed_files_group_open_
    // true for User means "browsing the flat, import-filtered slot list"
    // directly, same shape Global:File's own single-level list uses.
    DexedFilesGroup dexed_files_group_      = DexedFilesGroup::Roms;
    bool            dexed_files_group_open_ = false;
    int  dexed_preset_folder_cursor_ = 0;
    bool dexed_preset_folder_open_   = false;
    // Index WITHIN the open folder (not a flat index) -- resolved to a
    // real 1-based PerformanceStore slot by ResolveDexedPresetSlot().
    int  dexed_preset_cursor_ = 0;
    // PerformanceStore::kMaxDexedPresets (3500) minus the current 3129
    // factory presets (54 folders total) leaves up to 371 possible user
    // slots -- 200 gives real headroom without needing to keep this in
    // exact lockstep with the factory bank's own size.
    static constexpr int kMaxDexedPresetSlots = 200;
    // Filtered to EXCLUDE any slot that belongs to a recorded import
    // folder (see RefreshDexedPresetSlots()) -- this is specifically the
    // User group's own slot list, not every user slot on the card.
    int  dexed_preset_user_slots_[kMaxDexedPresetSlots] = {}; // ascending
    int  dexed_preset_user_slot_count_                   = 0;
    bool dexed_preset_slots_dirty_ = true; // forces one RefreshDexedPresetSlots() on entry
    // One entry per completed SysEx import (see
    // PerformanceStore::SaveDexedImportFolder()), refreshed by the same
    // RefreshDexedPresetSlots() call/dirty flag above -- kept alongside
    // it (not a separate dirty flag) since the User list's own filtering
    // depends on this being current first.
    PerformanceStore::DexedImportFolder
         dexed_import_folders_[PerformanceStore::kMaxDexedImportFolders] = {};
    int  dexed_import_folder_count_ = 0;
    // Starts at slot 1 (factory preset 0, "ARP 2600"), matching
    // DexedSynth::Init()'s own boot default.
    int  dexed_loaded_preset_slot_ = 1;
    char dexed_preset_status_[24]  = {}; // last save/load result, shown briefly

    // Preset page's top-level chooser own third option -- Import (SD
    // .syx files, DXIMPORT/, separate from Grains' own WAV IMPORT/, see
    // PerformanceStore::ImportDexedSyx()). Mutually exclusive with
    // load_new_selected_ (Files is neither of these two) -- see the K1
    // 3-way quantization in ApplyKnobs()'s own DexedParamPage::Preset
    // case. dexed_import_browsing_: false = still at the top-level
    // chooser, true = drilled into the flat file list (one level, unlike
    // Files' own two -- there's no factory-category structure for an
    // arbitrary SD folder).
    bool dexed_import_selected_ = false;
    bool dexed_import_browsing_ = false;
    int  dexed_import_cursor_   = 0; // which .syx file is highlighted
    char dexed_import_names_[kMaxImportFiles][PerformanceStore::kMaxImportSyxNameLen + 1] = {};
    int  dexed_import_file_count_  = 0;
    bool dexed_import_files_dirty_ = true; // forces one RefreshDexedImportFiles() on entry

    // Caches the real name (DexedSynth::GetPresetDataName()) of whichever
    // USER slot is currently highlighted while browsing "Load:" -- unlike
    // a factory slot's name (read straight out of a const embedded array,
    // free), a user slot's name lives inside its own saved file, so
    // showing it means an SD read; caching by slot number means that
    // only happens once per distinct slot the cursor lands on, not once
    // per Draw() tick while sitting still on one. -2 (never a real slot
    // number -- see ResolveDexedPresetSlot()'s own -1 "not found") means
    // nothing is cached yet.
    int  dexed_browse_name_slot_   = -2;
    char dexed_browse_name_buf_[11] = {};

    // --- Dexed advanced editor (Screen::DexedOperator) -------------------
    // 0-5, array index into patch_[] (op*21) -- 0 = HW OP6, 5 = OP1, same
    // convention confirmed from the real DX7 SysEx spec and already used
    // throughout the Algo diagram. Button1-cycled, not a page of its own.
    int                dexed_op_index_ = 0;
    DexedOpParamPage   dexed_op_page_  = DexedOpParamPage::RatioLevel;
    // AD/SR toggles for the EgRate/EgLevel pages, same idiom as
    // granular_adsr_target_sr_ -- two independent bools since the two
    // pages' own toggle state shouldn't reset just from navigating
    // between them.
    bool dexed_op_egrate_target_sr_  = false;
    bool dexed_op_eglevel_target_sr_ = false;
    void DrawDexedOperatorScreen();
};
