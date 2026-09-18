#include "ui.h"
#include "performance_store.h"
#include "audio_engine.h"
#include "granular_engine.h"
#include "dexed_synth.h"
#include "msfa/fm_core.h"
#include <cstdio>
#include <cstring>
#include <cctype>
#include <math.h>

using namespace daisy;

namespace
{
inline float Clampf(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

// Set just before a blocking PerformanceStore::Save()/Load() call so the
// static Ui::OnSaveLoadProgress() callback (a plain C function pointer,
// so it can't carry a `this`) has something to draw the progress bar to.
// Safe as a single global: there is exactly one Ui instance and Save()/
// Load() are only ever called synchronously from the main loop.
OneBitGraphicsDisplay* g_progress_disp = nullptr;

const char* StateGlyph(LayerState s)
{
    switch(s)
    {
        case LayerState::Empty: return "-";
        case LayerState::ArmedCountIn: return "C";
        case LayerState::Recording: return "R";
        case LayerState::Playing: return "P";
        case LayerState::Paused: return "||";
        case LayerState::Overdubbing: return "O";
    }
    return "?";
}

const char* FilterModeName(FilterMode m)
{
    switch(m)
    {
        case FilterMode::Off: return "Off";
        case FilterMode::LowPass: return "LowPass";
        case FilterMode::HighPass: return "HighPass";
        case FilterMode::BandPass: return "BandPass";
        default: return "?";
    }
}

const char* EffectName(LayerEffect e)
{
    switch(e)
    {
        case LayerEffect::Off: return "Off";
        case LayerEffect::Drive: return "Drive";
        case LayerEffect::Bitcrush: return "Bitcrush";
        case LayerEffect::Chorus: return "Chorus";
        case LayerEffect::Tremolo: return "Tremolo";
        case LayerEffect::Phaser: return "Phaser";
        case LayerEffect::AutoWah: return "AutoWah";
        case LayerEffect::Flanger: return "Flanger";
        default: return "?";
    }
}

// Knob 1 / Knob 2 labels for the Effect page -- matches the param
// mapping in LooperLayer::ProcessEffectsChain(). Drive only reads
// ParamA (ParamB does nothing for it), everything else uses both.
const char* EffectParamALabel(LayerEffect e)
{
    switch(e)
    {
        case LayerEffect::Drive: return "Drive";
        case LayerEffect::Bitcrush: return "Crush";
        case LayerEffect::Chorus: return "Depth";
        case LayerEffect::Tremolo: return "Depth";
        case LayerEffect::Phaser: return "Depth";
        case LayerEffect::AutoWah: return "Wah";
        case LayerEffect::Flanger: return "Depth";
        default: return "-";
    }
}
const char* GranularDirectionName(GranularEngine::Direction d)
{
    switch(d)
    {
        case GranularEngine::Direction::Forward: return "Forward";
        case GranularEngine::Direction::Reverse: return "Reverse";
        case GranularEngine::Direction::Random: return "Random";
        default: return "?";
    }
}

const char* EffectParamBLabel(LayerEffect e)
{
    switch(e)
    {
        case LayerEffect::Drive: return "-";
        case LayerEffect::Bitcrush: return "Rate";
        case LayerEffect::Chorus: return "Rate";
        case LayerEffect::Tremolo: return "Rate";
        case LayerEffect::Phaser: return "Rate";
        case LayerEffect::AutoWah: return "Level";
        case LayerEffect::Flanger: return "Rate";
        default: return "-";
    }
}
} // namespace

void Ui::Init(daisy::DaisyPod*              pod,
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
              DexedSynth*                   dexed)
{
    pod_        = pod;
    disp_       = display;
    tempo_      = tempo;
    layers_     = layers;
    num_layers_ = num_layers;
    granular_           = granular;
    granular_scope_buf_      = granular_scope_buf;
    granular_scope_capacity_ = granular_scope_capacity;
    granular_capture_buf_l_     = granular_capture_buf_l;
    granular_capture_buf_r_     = granular_capture_buf_r;
    granular_capture_capacity_  = granular_capture_capacity;
    granular_capturing_         = granular_capturing;
    granular_capture_write_pos_ = granular_capture_write_pos;
    master_scope_buf_      = master_scope_buf;
    master_scope_capacity_ = master_scope_capacity;
    dexed_                 = dexed;

    // Same curve ApplyKnobs() uses for knob1 on Home, applied once here
    // so master_volume_ actually matches master_volume01_'s starting
    // value instead of carrying its own separate hardcoded default.
    master_volume_ = powf(master_volume01_, 2.5f) * 1.43f;
    if(master_volume_ < 0.f)
        master_volume_ = 0.f;
    // Same reasoning as master_volume_ just above, for Screen::Mixer's
    // own Bypass channel volume.
    SetBypassMixVolume01(bypass_mix_volume01_);

    // screen_ and last_knob_context_ both default to Home, so the
    // context-change check in ApplyKnobs() that normally calls this
    // never fires for the very first tick after power-on -- without
    // this, Home's pickup targets would sit at their raw 0-default
    // instead of the real starting values above, meaning the knobs
    // would need sweeping down to 0% to regain control rather than to
    // wherever Volume/Metro actually start.
    SyncPickupTargets(KnobContext::Home);
}

void Ui::ApplyStartupDefaults()
{
    float      bpm, master_vol01, cutoff01, res01, reverb_sz01, byp_send01, metro_vol01;
    int        bars;
    bool       byp, metro_on;
    FilterMode mode;
    if(!PerformanceStore::LoadPrefs(&bpm, &bars, &master_vol01, &byp, &mode, &cutoff01,
                                      &res01, &reverb_sz01, &byp_send01, &metro_on,
                                      &metro_vol01))
        return; // nothing saved yet -- Init()'s hardcoded defaults already stand

    // SetBpm()/SetBars() are no-ops while locked, but nothing can be
    // locked this early (no layer has recorded anything yet).
    tempo_->SetBpm(bpm);
    tempo_->SetBars(bars);
    tempo_->SetMetronomeEnabled(metro_on);
    tempo_->SetMetronomeVolume01(metro_vol01);

    master_volume01_ = master_vol01;
    master_volume_   = powf(Clampf(master_volume01_, 0.f, 1.f), 2.5f) * 1.43f;
    if(master_volume_ < 0.f)
        master_volume_ = 0.f;

    bypass_                  = byp;
    master_filter_mode_      = mode;
    master_filter_cutoff01_  = cutoff01;
    master_filter_res01_     = res01;
    reverb_size01_           = reverb_sz01;
    bypass_reverb_send01_    = byp_send01;

    // Init() already called this once with the pre-defaults values (see
    // its own comment above) -- re-seed now that the real starting
    // values are in place, same reasoning, same fix.
    SyncPickupTargets(KnobContext::Home);
}

void Ui::SetBypassMixVolume01(float v01)
{
    bypass_mix_volume01_ = Clampf(v01, 0.f, 1.f);
    bypass_mix_volume_   = powf(bypass_mix_volume01_, 2.5f) * 1.4f;
    if(bypass_mix_volume_ < 0.f)
        bypass_mix_volume_ = 0.f;
}

void Ui::Update(const UiControlEvents& events)
{
    HandleEncoder(events);
    HandleButton1(events);
    HandleButton2(events);
    ApplyKnobs();

    // Grains' Rhythm Speed has a "Sync" option that locks to the
    // project's live tempo -- the engine has no tempo concept of its
    // own otherwise, so Ui (which already owns the TempoClock) pushes
    // the current BPM in once per tick, cheap enough to do unconditionally
    // rather than only when Speed is actually set to Sync.
    if(granular_ && tempo_)
        granular_->SetExternalBpm(tempo_->GetBpm());

    // Once any layer holds a recording, lock the tempo so BPM/Bars can't
    // be changed out from under it; unlock once every layer is empty
    // again so a fresh song can pick a new tempo.
    bool any_content = false;
    for(int i = 0; i < num_layers_; i++)
        if(layers_[i].HasContent())
            any_content = true;
    if(any_content)
        tempo_->Lock();
    else
        tempo_->Unlock();

    UpdateLeds();

    // The OLED redraw is a blocking I2C transfer of ~1KB -- a few
    // milliseconds. Throttled to ~30Hz here; Update() is expected to be
    // called at roughly 500Hz-1kHz from main()'s loop (see main.cpp), so
    // redrawing every 20th call keeps the screen responsive without
    // hogging the I2C bus or the main loop.
    draw_counter_++;
    if(draw_counter_ >= 20)
    {
        draw_counter_ = 0;
        Draw();
    }
}

// --- Input handling ------------------------------------------------------

void Ui::HandleEncoder(const UiControlEvents& events)
{
    int32_t inc = events.encoder_delta;
    if(inc != 0)
    {
        if(scrub_mode_active_ && screen_ == Screen::Global && global_page_ == GlobalPage::Speed)
        {
            ScrubBy(inc);
        }
        else if(screen_ == Screen::Home)
        {
            cursor_layer_ = ((cursor_layer_ + inc) % num_layers_ + num_layers_) % num_layers_;
        }
        else if(screen_ == Screen::Layer)
        {
            int n = (int)LayerPage::kCount;
            int p = (((int)layer_page_ + inc) % n + n) % n;
            layer_page_ = (LayerPage)p;
        }
        else if(screen_ == Screen::Global)
        {
            int n = (int)GlobalPage::kCount;
            int p = (((int)global_page_ + inc) % n + n) % n;
            GlobalPage new_page = (GlobalPage)p;
            if(new_page == GlobalPage::File && global_page_ != GlobalPage::File)
                file_slots_dirty_ = true; // re-scan the card on entry
            if(new_page == GlobalPage::Export && global_page_ != GlobalPage::Export)
                export_status_[0] = '\0'; // clear any stale result on entry
            if(new_page == GlobalPage::SdMgmt && global_page_ != GlobalPage::SdMgmt)
                sd_mgmt_in_folder_ = false; // always start back at folder-select on entry
            global_page_    = new_page;
            save_load_mode_ = SaveLoadMode::Idle; // leaving/entering any page resets this
        }
        else if(screen_ == Screen::Granular)
        {
            int n = (int)GranularParamPage::kCount;
            int p = (((int)granular_param_page_ + inc) % n + n) % n;
            GranularParamPage new_granular_page = (GranularParamPage)p;
            if(new_granular_page == GranularParamPage::Preset
               && granular_param_page_ != GranularParamPage::Preset)
                granular_preset_slots_dirty_ = true; // re-scan the card on entry
            granular_param_page_ = new_granular_page;
            save_load_mode_      = SaveLoadMode::Idle; // same reset as Global:File above
        }
        else if(screen_ == Screen::Dexed)
        {
            int n = (int)DexedParamPage::kCount;
            int p = (((int)dexed_param_page_ + inc) % n + n) % n;
            DexedParamPage new_dexed_page = (DexedParamPage)p;
            if(new_dexed_page == DexedParamPage::Preset
               && dexed_param_page_ != DexedParamPage::Preset)
            {
                dexed_preset_slots_dirty_ = true; // re-scan the card on entry
                dexed_preset_folder_open_ = false; // always start at the folder list
            }
            dexed_param_page_ = new_dexed_page;
            save_load_mode_   = SaveLoadMode::Idle; // same reset as Global:File above
        }
        else if(screen_ == Screen::DexedOperator)
        {
            int n = (int)DexedOpParamPage::kCount;
            int p = (((int)dexed_op_page_ + inc) % n + n) % n;
            dexed_op_page_ = (DexedOpParamPage)p;
        }
        else if(screen_ == Screen::Mixer)
        {
            int n            = kNumMixerPositions;
            int new_position = ((mixer_position_ + inc) % n + n) % n;
            if(new_position != mixer_position_)
            {
                mixer_position_ = new_position;
                // Same reset/reseed CurrentKnobContext()'s own change
                // would normally trigger automatically in ApplyKnobs()
                // (see MixerVolPan/MixerReverb/MixerMaster's own comment
                // for why this needs doing explicitly here instead) --
                // without this, turning past a channel whose knob context
                // enum doesn't itself change (any of the 7 non-Master
                // channels) would leave the knobs pointing at the OLD
                // channel's cached pickup values instead of the new one's.
                KnobContext ctx        = CurrentKnobContext();
                size_t      ctx_index  = (size_t)ctx;
                k1_pickup_engaged_[ctx_index] = false;
                k2_pickup_engaged_[ctx_index] = false;
                SyncPickupTargets(ctx);
                last_knob_context_ = ctx;
            }
        }
    }

    if(pod_->encoder.Pressed() && pod_->encoder.TimeHeldMs() > 600.f && !encoder_long_fired_)
    {
        encoder_long_fired_ = true;
        if(screen_ == Screen::Home)
        {
            screen_      = Screen::Global;
            global_page_ = GlobalPage::Tempo;
            // This hand-wired SD socket has no card-detect pin, so the
            // firmware otherwise never notices a card was swapped out
            // (e.g. pulled to move a WAV export to another device) and
            // put back -- re-mount here so Global:File/Export reflect
            // whatever card is actually in the slot right now, rather
            // than a stale mount from boot.
            PerformanceStore::Remount();
        }
        else
        {
            screen_ = Screen::Home;
        }
    }

    // "Click" (drill into the cursor layer) only fires on release, and
    // only if this press didn't already turn out to be a long-press --
    // otherwise a long-press from Home would drill into Layer on the
    // initial press-down before the 600ms check ever saw screen_ as
    // Home, turning every long-press into an immediate Layer->Home
    // bounce instead of reaching Global.
    if(events.encoder_click_fell)
    {
        if(!encoder_long_fired_ && screen_ == Screen::Home)
        {
            screen_     = Screen::Layer;
            layer_page_ = LayerPage::Status;
        }
        else if(!encoder_long_fired_ && screen_ == Screen::Global
                && global_page_ == GlobalPage::Granular)
        {
            // Global:Granular is an entry point into Screen::Granular,
            // same convention Screen::Mixer's own Global page uses.
            screen_ = Screen::Granular;
        }
        else if(!encoder_long_fired_ && screen_ == Screen::Global
                && global_page_ == GlobalPage::Looper)
        {
            // No screen to drill into (Button1 tap is this page's own
            // toggle) -- explicitly excluded from TogglePauseAll() below
            // so this page's click can't accidentally trigger a second,
            // different "stop the loop" mechanism.
        }
        else if(!encoder_long_fired_ && screen_ == Screen::Global
                && global_page_ == GlobalPage::Mixer)
        {
            // Global:Mixer is an entry point into Screen::Mixer, same
            // convention Global:Pad/Global:Granular's own click uses.
            screen_ = Screen::Mixer;
        }
        else if(!encoder_long_fired_ && screen_ == Screen::Global
                && global_page_ == GlobalPage::Dexed)
        {
            // Global:Dexed is an entry point into Screen::Dexed, same
            // convention Global:Granular's own click uses.
            screen_ = Screen::Dexed;
        }
        else if(!encoder_long_fired_ && screen_ == Screen::Global)
        {
            TogglePauseAll();
        }
        else if(!encoder_long_fired_ && screen_ == Screen::Dexed
                && dexed_param_page_ == DexedParamPage::Advanced)
        {
            // Dexed's own Advanced page is an entry point into
            // Screen::DexedOperator, same convention Global:Granular/
            // Global:Dexed's own click uses to enter their screens.
            screen_ = Screen::DexedOperator;
        }
        else if(!encoder_long_fired_ && screen_ == Screen::DexedOperator)
        {
            // Short click backs out to Screen::Dexed (landing back on
            // the Advanced page); long-press above still goes all the
            // way to Home, same as every other screen.
            screen_ = Screen::Dexed;
        }
        else if(!encoder_long_fired_
                && (screen_ == Screen::Granular || screen_ == Screen::Dexed))
        {
            // Same mute-all-loop-layers click as every Global page's own
            // (TogglePauseAll()) -- every other Granular/Dexed page has
            // no click-to-drill-in of their own (that's the encoder's
            // job from Global instead, or Dexed's own Advanced page
            // above), so there's nothing else useful for their click to
            // do either.
            TogglePauseAll();
        }
        // Screen::Mixer: rotate already picks the stop (see
        // HandleEncoder()'s own rotate handling above, including the
        // Scope stop) -- click is unused here, same as every other
        // Granular-style screen.
        encoder_long_fired_ = false;
    }

    // Scrub mode is a Global:Speed-only, always-transient toggle -- clear
    // it unconditionally the instant we're not there any more (long-press
    // to Home above is the only current exit path, but this isn't tied to
    // that one call site specifically so it can't be left silently stuck
    // on by any future navigation change).
    if(!(screen_ == Screen::Global && global_page_ == GlobalPage::Speed))
        scrub_mode_active_ = false;
}

void Ui::HandleButton1(const UiControlEvents& events)
{
    Switch& b = pod_->button1;
    if(b.Pressed() && b.TimeHeldMs() > 400.f && !button1_long_fired_)
    {
        button1_long_fired_ = true;
        OnButton1Long();
    }
    if(events.btn1_released)
    {
        // Fallback for when the main loop never got a chance to observe
        // "still held past threshold" before the release happened -- see
        // UiControlEvents::btn1_held_ms/btn2_held_ms's own comment. Every
        // long-press site in this function and HandleButton2() below
        // needs this same fallback, not just this one.
        if(!button1_long_fired_ && events.btn1_held_ms > 400.f)
        {
            OnButton1Long();
            button1_long_fired_ = true;
        }
        if(!button1_long_fired_)
            OnButton1Short();
        else
            OnButton1Release();
        button1_long_fired_ = false;
    }
}

void Ui::HandleButton2(const UiControlEvents& events)
{
    Switch& b = pod_->button2;
    // Button2's only "long" behaviours are the hold-to-clear confirm on
    // the layer Status page and the hold-to-load confirm on Global:File;
    // everywhere else it's a plain short-press action.
    if(screen_ == Screen::Layer && layer_page_ == LayerPage::Status)
    {
        if(b.Pressed() && b.TimeHeldMs() > 800.f && !button2_long_fired_)
        {
            button2_long_fired_ = true;
            Cur().Clear();
        }
        if(events.btn2_released)
        {
            // Fallback for a press/hold/release cycle that completed
            // between main-loop iterations -- see HandleButton1()'s
            // matching comment and main.cpp's g_btn2_held_ms.
            if(!button2_long_fired_ && events.btn2_held_ms > 800.f)
                Cur().Clear();
            button2_long_fired_ = false;
        }
    }
    else if(screen_ == Screen::Global && global_page_ == GlobalPage::SdMgmt
            && sd_mgmt_in_folder_)
    {
        // Longer hold than every other confirm gesture here (see
        // kSdMgmtDeleteHoldMs's own comment) -- delete has no undo.
        if(b.Pressed() && b.TimeHeldMs() > kSdMgmtDeleteHoldMs && !button2_long_fired_)
        {
            button2_long_fired_ = true;
            TriggerSdMgmtDelete();
        }
        if(events.btn2_released)
        {
            if(!button2_long_fired_ && events.btn2_held_ms > kSdMgmtDeleteHoldMs)
                TriggerSdMgmtDelete();
            button2_long_fired_ = false;
        }
    }
    else if(screen_ == Screen::Global && global_page_ == GlobalPage::File)
    {
        // Button2 confirms whichever of the two revealed states is
        // active -- Save (ChoosingSave) or Load (BrowsingLoad) -- so
        // Button1's own tap is free to always mean "Back" once inside
        // either one (see OnButton1Short()), same as SD MGMT's own
        // Back/Hold=Duplicate split.
        bool can_confirm_save = save_load_mode_ == SaveLoadMode::ChoosingSave;
        // BrowsingLoad only has something to confirm once either "New" is
        // picked at the top-level chooser or the numbered list has been
        // drilled into (see ApplyKnobs()'s own BrowsingLoad handling and
        // load_new_selected_/load_browsing_files_'s own comment) -- at
        // the chooser with "Files" picked there's no specific target yet,
        // so a hold there does nothing.
        bool can_confirm_load = save_load_mode_ == SaveLoadMode::BrowsingLoad
                                 && (load_browsing_files_ || load_new_selected_);
        bool can_confirm      = can_confirm_save || can_confirm_load;
        if(b.Pressed() && b.TimeHeldMs() > 800.f && !button2_long_fired_ && can_confirm)
        {
            button2_long_fired_ = true;
            if(can_confirm_save)
                TriggerSave(save_as_new_);
            else
                TriggerLoad();
            save_load_mode_ = SaveLoadMode::Idle;
        }
        if(events.btn2_released)
        {
            if(!button2_long_fired_ && events.btn2_held_ms > 800.f && can_confirm)
            {
                if(can_confirm_save)
                    TriggerSave(save_as_new_);
                else
                    TriggerLoad();
                save_load_mode_ = SaveLoadMode::Idle;
            }
            else if(!button2_long_fired_ && save_load_mode_ == SaveLoadMode::Idle)
            {
                // Short tap from Idle only: reveal the file list to
                // browse (Button2's own label becomes "Hold=Load" to
                // commit). While already in ChoosingSave/BrowsingLoad, a
                // short tap here does nothing new -- only the hold above
                // commits, and Button1's tap is "Back".
                if(!PerformanceStore::IsCardPresent())
                    PerformanceStore::Remount();
                else
                {
                    save_load_mode_     = SaveLoadMode::BrowsingLoad;
                    load_new_selected_  = false;
                    load_browsing_files_ = false;
                }
            }
            button2_long_fired_ = false;
        }
    }
    else if(screen_ == Screen::Granular && granular_param_page_ == GranularParamPage::Capture)
    {
        if(granular_capture_source_ >= 0)
        {
            // Instant copy -- same hold-to-confirm-then-fire-once
            // gesture as every other destructive/overwriting action in
            // this project (Load/New/Clear).
            if(b.Pressed() && b.TimeHeldMs() > 800.f && !button2_long_fired_)
            {
                button2_long_fired_ = true;
                TriggerGranularCaptureFromLayer();
            }
            if(events.btn2_released)
            {
                if(!button2_long_fired_ && events.btn2_held_ms > 800.f)
                    TriggerGranularCaptureFromLayer();
                button2_long_fired_ = false;
            }
        }
        else if(granular_capture_source_ == -2)
        {
            // Import -- same hold-to-confirm-then-fire-once gesture as
            // From Layer above (a real SD read + decode, not instant, but
            // still a single fire-once action, not a live-recording
            // gesture like Direct Record below).
            if(b.Pressed() && b.TimeHeldMs() > 800.f && !button2_long_fired_)
            {
                button2_long_fired_ = true;
                TriggerGranularImport();
            }
            if(events.btn2_released)
            {
                if(!button2_long_fired_ && events.btn2_held_ms > 800.f)
                    TriggerGranularImport();
                button2_long_fired_ = false;
            }
        }
        else if(granular_capturing_ && granular_capture_write_pos_)
        {
            // Direct Record: holding Button2 IS recording (live input
            // written in from AudioCallback() while *granular_capturing_
            // is true) -- release stops it, and so does the ISR itself
            // once the 5s buffer fills while still held (granular_was_
            // capturing_ catches both: it only clears once *granular_
            // capturing_ has actually gone false).
            bool pressed_now = b.Pressed();
            if(pressed_now && !granular_was_capturing_)
            {
                *granular_capture_write_pos_ = 0;
                *granular_capturing_         = true;
                granular_was_capturing_      = true;
            }
            else if(granular_was_capturing_ && (!pressed_now || !*granular_capturing_))
            {
                *granular_capturing_ = false;
                if(granular_)
                    granular_->SetSource(granular_capture_buf_l_, granular_capture_buf_r_,
                                          *granular_capture_write_pos_);
                OnNewGranularCapture(*granular_capture_write_pos_);
                granular_capture_status_[0] = '\0'; // no From-Layer status to show any more
                granular_was_capturing_     = false;
            }
        }
    }
    else if(screen_ == Screen::Granular && granular_param_page_ == GranularParamPage::Preset)
    {
        // Button2 confirms whichever of Save/Load is revealed -- see
        // Global:File's own Button2 handling for the shared SaveLoadMode
        // this mirrors.
        bool can_confirm_save = save_load_mode_ == SaveLoadMode::ChoosingSave;
        // BrowsingLoad only has something to confirm once either "New" is
        // picked at the top-level chooser or the numbered list has been
        // drilled into (see Global:File's own can_confirm_load comment).
        bool can_confirm_load = save_load_mode_ == SaveLoadMode::BrowsingLoad
                                 && (load_browsing_files_ || load_new_selected_);
        bool can_confirm      = can_confirm_save || can_confirm_load;
        if(b.Pressed() && b.TimeHeldMs() > 800.f && !button2_long_fired_ && can_confirm)
        {
            button2_long_fired_ = true;
            if(can_confirm_save)
                TriggerSaveGranularPreset(save_as_new_);
            else
                TriggerLoadGranularPreset();
            save_load_mode_ = SaveLoadMode::Idle;
        }
        if(events.btn2_released)
        {
            if(!button2_long_fired_ && events.btn2_held_ms > 800.f && can_confirm)
            {
                if(can_confirm_save)
                    TriggerSaveGranularPreset(save_as_new_);
                else
                    TriggerLoadGranularPreset();
                save_load_mode_ = SaveLoadMode::Idle;
            }
            else if(!button2_long_fired_ && save_load_mode_ == SaveLoadMode::Idle)
            {
                // Short tap from Idle only: reveal the preset list to
                // browse.
                if(!PerformanceStore::IsCardPresent())
                    PerformanceStore::Remount();
                else
                {
                    save_load_mode_      = SaveLoadMode::BrowsingLoad;
                    load_new_selected_   = false;
                    load_browsing_files_ = false;
                }
            }
            button2_long_fired_ = false;
        }
    }
    else if(screen_ == Screen::Dexed && dexed_param_page_ == DexedParamPage::Preset)
    {
        // Button2 confirms whichever of Save/Load is revealed -- see
        // Global:File's own Button2 handling for the shared SaveLoadMode
        // this mirrors.
        bool can_confirm_save = save_load_mode_ == SaveLoadMode::ChoosingSave;
        // Nothing concrete is highlighted while just browsing the folder
        // list (folder names aren't presets) -- confirm only once "New"
        // is picked at the top-level chooser, or a folder has actually
        // been opened onto a real preset (see dexed_preset_folder_open_'s
        // own comment).
        bool can_confirm_load = save_load_mode_ == SaveLoadMode::BrowsingLoad
                                 && (load_new_selected_
                                     || (load_browsing_files_ && dexed_preset_folder_open_));
        bool can_confirm      = can_confirm_save || can_confirm_load;
        if(b.Pressed() && b.TimeHeldMs() > 800.f && !button2_long_fired_ && can_confirm)
        {
            button2_long_fired_ = true;
            if(can_confirm_save)
                TriggerSaveDexedPreset(save_as_new_);
            else
                TriggerLoadDexedPreset();
            save_load_mode_ = SaveLoadMode::Idle;
        }
        if(events.btn2_released)
        {
            if(!button2_long_fired_ && events.btn2_held_ms > 800.f && can_confirm)
            {
                if(can_confirm_save)
                    TriggerSaveDexedPreset(save_as_new_);
                else
                    TriggerLoadDexedPreset();
                save_load_mode_ = SaveLoadMode::Idle;
            }
            else if(!button2_long_fired_ && save_load_mode_ == SaveLoadMode::BrowsingLoad
                     && load_browsing_files_ && dexed_preset_folder_open_)
            {
                // Short tap while a preset is highlighted inside an open
                // folder -- preview it immediately (apply to the live
                // engine) WITHOUT leaving the browser, so scrolling K1
                // and tapping B2 auditions one preset after another
                // without re-entering the whole Save/Load flow each
                // time. Button2's hold gesture above still does the same
                // load AND exits back to Idle, for once you've settled
                // on one.
                TriggerLoadDexedPreset();
            }
            else if(!button2_long_fired_ && save_load_mode_ == SaveLoadMode::Idle)
            {
                // Short tap from Idle only: reveal the preset list to
                // browse (Button2's own label becomes "Hold=Load" to
                // commit).
                if(!PerformanceStore::IsCardPresent())
                    PerformanceStore::Remount();
                else
                {
                    save_load_mode_           = SaveLoadMode::BrowsingLoad;
                    load_new_selected_        = false;
                    load_browsing_files_      = false;
                    dexed_preset_folder_open_ = false; // always start at the folder list
                }
            }
            button2_long_fired_ = false;
        }
    }
    else if(screen_ == Screen::Global && global_page_ == GlobalPage::Tempo)
    {
        // Button2 does nothing else on this page -- save the current
        // global settings as the startup default (see
        // TriggerSaveDefaults()/PerformanceStore::SavePrefs()). A hold,
        // not a tap, since this is a deliberate write, same weight as
        // File's hold-to-Load/Status's hold-to-Clear.
        if(b.Pressed() && b.TimeHeldMs() > 800.f && !button2_long_fired_)
        {
            button2_long_fired_ = true;
            TriggerSaveDefaults();
        }
        if(events.btn2_released)
        {
            if(!button2_long_fired_ && events.btn2_held_ms > 800.f)
                TriggerSaveDefaults();
            button2_long_fired_ = false;
        }
    }
    else if(events.btn2_released)
    {
        OnButton2Short();
    }
}

void Ui::OnButton1Short()
{
    switch(screen_)
    {
        case Screen::Home: layers_[cursor_layer_].OnRecordButtonPressed(*tempo_); break;
        case Screen::Layer:
            switch(layer_page_)
            {
                case LayerPage::Status: Cur().OnRecordButtonPressed(*tempo_); break;
                case LayerPage::Speed:
                    Cur().SetSpeed01(0.5f); // quick reset to 1.0x
                    // Without this, the knob (if already "picked up")
                    // would overwrite the reset on the very next
                    // ApplyKnobs() tick with wherever it physically
                    // sits -- re-arm pickup so the reset actually
                    // sticks until the knob is swept back to center.
                    k1_pickup_engaged_[(size_t)KnobContext::LayerSpeed] = false;
                    k1_pickup_raw_[(size_t)KnobContext::LayerSpeed]     = 0.5f;
                    break;
                case LayerPage::Filter:
                {
                    int n = (int)FilterMode::kNumModes;
                    int m = ((int)Cur().GetFilterMode() + 1) % n;
                    Cur().SetFilterMode((FilterMode)m);
                    break;
                }
                case LayerPage::Effect:
                {
                    int n = (int)LayerEffect::kNumEffects;
                    int e = ((int)Cur().GetEffect() + 1) % n;
                    Cur().SetEffect((LayerEffect)e);
                    // SetEffect() already reset the params themselves to
                    // 0 (see its comment) -- but cycling through effects
                    // stays within the same knob context the whole time,
                    // so the generic context-change pickup sync never
                    // fires here on its own. Re-arm both knobs to that
                    // fresh 0, same reasoning as the Speed page's reset
                    // above: without this, an already-picked-up knob
                    // would immediately overwrite the reset with wherever
                    // it physically sits.
                    k1_pickup_engaged_[(size_t)KnobContext::LayerEffect] = false;
                    k2_pickup_engaged_[(size_t)KnobContext::LayerEffect] = false;
                    k1_pickup_raw_[(size_t)KnobContext::LayerEffect]     = 0.f;
                    k2_pickup_raw_[(size_t)KnobContext::LayerEffect]     = 0.f;
                    break;
                }
                default: break;
            }
            break;
        case Screen::Global:
            if(global_page_ == GlobalPage::Tempo)
                tempo_->ToggleMetronome();
            else if(global_page_ == GlobalPage::Filter)
            {
                int n = (int)FilterMode::kNumModes;
                int m = ((int)master_filter_mode_ + 1) % n;
                master_filter_mode_ = (FilterMode)m;
            }
            else if(global_page_ == GlobalPage::Speed)
                scrub_mode_active_ = !scrub_mode_active_;
            else if(global_page_ == GlobalPage::File)
            {
                if(save_load_mode_ == SaveLoadMode::BrowsingLoad && load_browsing_files_)
                {
                    // Back out of the numbered file list to the Files/New
                    // chooser (see load_browsing_files_'s own comment).
                    load_browsing_files_ = false;
                }
                else if(save_load_mode_ == SaveLoadMode::BrowsingLoad && !load_new_selected_)
                {
                    // "Files" is highlighted at the chooser -- drill into
                    // the numbered list (Knob1 now scrolls it directly,
                    // see ApplyKnobs()).
                    load_browsing_files_ = true;
                }
                else if(save_load_mode_ != SaveLoadMode::Idle)
                {
                    // Back -- covers ChoosingSave, and the Load chooser
                    // with "New" highlighted (nothing further to drill
                    // into there). Button2 owns confirming whichever of
                    // Save/Load is currently revealed (see
                    // HandleButton2()), so Button1's tap here is
                    // otherwise always just a way out, same as SD MGMT's
                    // own Back.
                    save_load_mode_ = SaveLoadMode::Idle;
                }
                // No card: this hand-wired socket has no card-detect pin
                // (see PerformanceStore::Remount()'s doc comment), so a
                // card swapped out and back in needs an explicit re-mount
                // attempt -- repurpose Button1 for that instead of Save
                // while there's nothing to save to anyway.
                else if(!PerformanceStore::IsCardPresent())
                {
                    PerformanceStore::Remount();
                    file_slots_dirty_ = true; // re-scan once actually mounted
                }
                else
                {
                    // Reveal the Overwrite/Save New choice -- Button2's
                    // hold now confirms it (see HandleButton2()).
                    // Default to whichever's actually available:
                    // Overwrite only makes sense if something's loaded.
                    save_load_mode_ = SaveLoadMode::ChoosingSave;
                    save_as_new_    = loaded_slot_ < 0;
                }
            }
            else if(global_page_ == GlobalPage::Export)
            {
                if(!PerformanceStore::IsCardPresent())
                    PerformanceStore::Remount();
                else
                    TriggerExport();
            }
            else if(global_page_ == GlobalPage::Granular)
                granular_enabled_ = !granular_enabled_;
            else if(global_page_ == GlobalPage::Dexed)
                dexed_enabled_ = !dexed_enabled_;
            else if(global_page_ == GlobalPage::Looper)
            {
                looper_enabled_ = !looper_enabled_;
                // Switching off resets the shared tempo clock's own
                // phase (bar/beat position, count-in state, click
                // envelope) back to bar 1 beat 1 -- see ResetPhase()'s
                // own doc comment. AudioCallback() also stops advancing
                // it at all while disabled (see IsLooperEnabled() there),
                // so this is what makes "off" actually mean "reset and
                // ready to resume cleanly", not just frozen wherever it
                // happened to be.
                if(!looper_enabled_ && tempo_)
                    tempo_->ResetPhase();
            }
            else if(global_page_ == GlobalPage::SdMgmt)
            {
                if(!sd_mgmt_in_folder_)
                {
                    // Drill into the highlighted folder -- force a fresh
                    // scan (same "re-scan on entry" idiom as Global:File)
                    // since files may have changed since the last visit.
                    sd_mgmt_in_folder_ = true;
                    sd_mgmt_cursor_    = 0;
                    sd_mgmt_status_[0] = '\0';
                    switch(sd_mgmt_folder_)
                    {
                        case SdMgmtFolder::Performances: file_slots_dirty_ = true; break;
                        case SdMgmtFolder::GranularPresets:
                            granular_preset_slots_dirty_ = true;
                            break;
                        default: break;
                    }
                }
                else
                {
                    // Back up to folder-select -- Button1's hold means
                    // Duplicate once already browsing files (see
                    // OnButton1Long()), so its tap is free to mean this.
                    sd_mgmt_in_folder_ = false;
                }
            }
            break;
        case Screen::Granular:
            if(granular_param_page_ == GranularParamPage::Grain)
                granular_grain_target_gap_scan_ = false; // knobs -> Size+Fill
            else if(granular_param_page_ == GranularParamPage::Mix)
                granular_mix_target_reverb_ = false; // knobs -> Grain+Scan
            else if(granular_param_page_ == GranularParamPage::ADSR)
                granular_adsr_target_sr_ = false; // knobs -> Attack+Decay
            else if(granular_param_page_ == GranularParamPage::TuneDirection && granular_)
                granular_->SetGrainFollowsNote(!granular_->GetGrainFollowsNote());
            else if(granular_param_page_ == GranularParamPage::Position && granular_)
                granular_->CycleRhythm();
            else if(granular_param_page_ == GranularParamPage::Filter && granular_)
            {
                // Same "Button1 cycles" idiom as Layer:Filter/Pad:Filter.
                int n = (int)FilterMode::kNumModes;
                int m = ((int)granular_->GetFilterMode() + 1) % n;
                granular_->SetFilterMode((FilterMode)m);
            }
            else if(granular_param_page_ == GranularParamPage::Capture)
            {
                // Cycles Direct Record -> Layer 1 -> ... -> Layer
                // num_layers_ -> Import -> Direct Record. The final
                // Import(-2) -> Direct(-1) step happens for free on the
                // NEXT tap via plain integer ++ -- only the "just past
                // the last layer" transition needs an explicit redirect.
                granular_capture_source_++;
                if(granular_capture_source_ >= num_layers_)
                    granular_capture_source_ = -2;
                if(granular_capture_source_ == -2)
                    granular_import_files_dirty_ = true; // re-scan IMPORT/ on entry
                granular_capture_status_[0] = '\0'; // stale result from the other source
            }
            else if(granular_param_page_ == GranularParamPage::Preset)
            {
                if(save_load_mode_ == SaveLoadMode::BrowsingLoad && load_browsing_files_)
                {
                    // Back out of the numbered list to the Files/New
                    // chooser -- see Global:File's own comment.
                    load_browsing_files_ = false;
                }
                else if(save_load_mode_ == SaveLoadMode::BrowsingLoad && !load_new_selected_)
                {
                    // "Files" is highlighted -- drill into the numbered
                    // list (Knob1 now scrolls it directly).
                    load_browsing_files_ = true;
                }
                else if(save_load_mode_ != SaveLoadMode::Idle)
                {
                    // Back -- Button2 confirms Save/Load (see
                    // HandleButton2()), so this tap always just returns
                    // to the page's own idle state, same as Global:File.
                    save_load_mode_ = SaveLoadMode::Idle;
                }
                else if(!PerformanceStore::IsCardPresent())
                    PerformanceStore::Remount();
                else
                {
                    // Reveal the Overwrite/Save New choice -- Button2's
                    // hold now confirms it (see HandleButton2()).
                    save_load_mode_ = SaveLoadMode::ChoosingSave;
                    save_as_new_    = granular_loaded_preset_slot_ <= 0;
                }
            }
            break;
        case Screen::Dexed:
            if(dexed_param_page_ == DexedParamPage::Algo && dexed_)
            {
                // Button1 cycles backward, Button2 (OnButton2Short())
                // cycles forward -- a two-direction cycle since, unlike
                // Layer:Filter/Granular:Filter's short mode lists,
                // stepping past algorithm 32 back around to 1 (or vice
                // versa) to reach a nearby one is a real, common need.
                int algo = (dexed_->GetPatchByte(134) + 31) % 32;
                dexed_->SetPatchByte(134, (uint8_t)algo);
            }
            else if(dexed_param_page_ == DexedParamPage::Filter && dexed_)
            {
                // Same "Button1 cycles" idiom as Layer:Filter/Granular:Filter.
                int n = (int)FilterMode::kNumModes;
                int m = ((int)dexed_->GetFilterMode() + 1) % n;
                dexed_->SetFilterMode((FilterMode)m);
            }
            else if(dexed_param_page_ == DexedParamPage::Preset)
            {
                if(save_load_mode_ == SaveLoadMode::BrowsingLoad && load_browsing_files_)
                {
                    // Two levels here, unlike Global:File's single
                    // numbered list -- see dexed_preset_folder_open_'s
                    // own comment. Folder open: back out to the folder
                    // list. Folder list: open the highlighted folder
                    // instead (Knob1 now scrolls the presets inside it).
                    if(dexed_preset_folder_open_)
                        dexed_preset_folder_open_ = false;
                    else
                    {
                        dexed_preset_folder_open_ = true;
                        dexed_preset_cursor_      = 0;
                    }
                }
                else if(save_load_mode_ == SaveLoadMode::BrowsingLoad && !load_new_selected_)
                {
                    // "Files" is highlighted -- drill into the folder
                    // list (Knob1 now scrolls dexed_preset_folder_cursor_
                    // directly), always starting at its very top.
                    load_browsing_files_     = true;
                    dexed_preset_folder_open_ = false;
                    dexed_preset_folder_cursor_ = 0;
                }
                else if(save_load_mode_ != SaveLoadMode::Idle)
                {
                    // Back -- Button2 confirms Save/Load (see
                    // HandleButton2()), so this tap always just returns
                    // to the page's own idle state, same as Global:File.
                    save_load_mode_ = SaveLoadMode::Idle;
                }
                else if(!PerformanceStore::IsCardPresent())
                    PerformanceStore::Remount();
                else
                {
                    // Reveal the Overwrite/Save New choice -- Button2's
                    // hold now confirms it (see HandleButton2()).
                    save_load_mode_ = SaveLoadMode::ChoosingSave;
                    save_as_new_    = dexed_loaded_preset_slot_ <= DexedSynth::GetNumFactoryPresets();
                }
            }
            break;
        case Screen::DexedOperator:
        {
            // Button1 cycles which of the 6 operators is being edited --
            // rotate stays reserved for DexedOpParamPage navigation, same
            // "Button1 cycles a non-page selector" convention as Algo's
            // own algorithm cycle above.
            dexed_op_index_ = (dexed_op_index_ + 1) % 6;
            // Real bug, confirmed on hardware: DexedOpRatioLevel/Detune/
            // EgRateAD/etc. are each ONE KnobContext shared across all 6
            // operators (branching internally on dexed_op_index_, same
            // idiom as LayerStatus/cursor_layer_) -- but unlike rotating
            // between LayerStatus's own layers (which always passes back
            // through Home, a genuine context change, first), changing
            // dexed_op_index_ here does NOT change which KnobContext
            // enum value CurrentKnobContext() returns, so ApplyKnobs()'s
            // own automatic "re-arm pickup on context change" never
            // fires. Left alone, an already-engaged knob would
            // immediately re-apply its last raw position against the
            // NEWLY selected operator's bytes on the very next tick --
            // exactly what turning one operator's Rate then cycling to
            // another showed on hardware ("all the other operators'
            // rates change to that value"). Screen::Mixer's own rotate
            // handling hit the identical class of bug for mixer_position_
            // and fixed it the same way: explicitly re-run the reset/
            // reseed here instead of relying on the automatic path.
            KnobContext ctx       = CurrentKnobContext();
            size_t      ctx_index = (size_t)ctx;
            k1_pickup_engaged_[ctx_index] = false;
            k2_pickup_engaged_[ctx_index] = false;
            SyncPickupTargets(ctx);
            last_knob_context_ = ctx;
            break;
        }
        case Screen::Mixer:
            mixer_target_reverb_ = false; // knobs -> Volume+Pan (ignored on Master)
            break;
    }
}

void Ui::OnButton1Long()
{
    // Long-press is only meaningful on the transport (arm to record /
    // start overdub) and SD MGMT's own hold-to-duplicate -- File/Grains
    // Preset don't use Button1's hold for anything any more (New is
    // reachable from the Load browse list's own trailing "New" entry
    // instead, see TriggerLoad()/TriggerNewGranularPreset()), so a long
    // hold there does nothing.
    if(screen_ == Screen::Home)
        layers_[cursor_layer_].OnRecordButtonLongPress(*tempo_);
    else if(screen_ == Screen::Layer && layer_page_ == LayerPage::Status)
        Cur().OnRecordButtonLongPress(*tempo_);
    else if(screen_ == Screen::Global && global_page_ == GlobalPage::SdMgmt
            && sd_mgmt_in_folder_)
        TriggerSdMgmtDuplicate();
}

void Ui::OnButton1Release()
{
    if(screen_ == Screen::Home)
        layers_[cursor_layer_].OnRecordButtonReleased();
    else if(screen_ == Screen::Layer && layer_page_ == LayerPage::Status)
        Cur().OnRecordButtonReleased();
}

void Ui::OnButton2Short()
{
    if(screen_ == Screen::Home)
        bypass_ = !bypass_;
    else if(screen_ == Screen::Global && global_page_ == GlobalPage::Speed)
    {
        SetProjectSpeed01(0.5f); // tap = reset to 1.0x, mirrors Layer:Speed's Button1 tap
        // Re-arm pickup so the reset actually sticks, same reasoning as
        // Layer:Speed's own reset above.
        k1_pickup_engaged_[(size_t)KnobContext::GlobalSpeed] = false;
        k1_pickup_raw_[(size_t)KnobContext::GlobalSpeed]     = 0.5f;
    }
    else if(screen_ == Screen::Global && global_page_ == GlobalPage::Export)
        TriggerExportMicroDexed();
    else if(screen_ == Screen::Dexed && dexed_param_page_ == DexedParamPage::Algo && dexed_)
    {
        // Forward direction of the Algo page's two-way cycle -- see
        // OnButton1Short()'s own comment for why this page (uniquely)
        // gets both directions.
        int algo = (dexed_->GetPatchByte(134) + 1) % 32;
        dexed_->SetPatchByte(134, (uint8_t)algo);
    }
    else if(screen_ == Screen::DexedOperator && dexed_op_page_ == DexedOpParamPage::EgRate)
        // Toggles (not just sets true) -- unlike Granular's own ADSR
        // page, Button1 is already committed to operator-select duty
        // here (see OnButton1Short()), so there's no second button free
        // to select AD explicitly; Button2 alone has to cover both
        // directions.
        dexed_op_egrate_target_sr_ = !dexed_op_egrate_target_sr_;
    else if(screen_ == Screen::DexedOperator && dexed_op_page_ == DexedOpParamPage::EgLevel)
        dexed_op_eglevel_target_sr_ = !dexed_op_eglevel_target_sr_;
    else if(screen_ == Screen::Granular && granular_param_page_ == GranularParamPage::Grain)
        granular_grain_target_gap_scan_ = true; // knobs -> Gap+Scan
    else if(screen_ == Screen::Granular && granular_param_page_ == GranularParamPage::Position
            && granular_)
        granular_->CycleGrainSpeed();
    else if(screen_ == Screen::Granular && granular_param_page_ == GranularParamPage::ADSR)
        granular_adsr_target_sr_ = true; // knobs -> Sustain/Release
    else if(screen_ == Screen::Granular && granular_param_page_ == GranularParamPage::Mix)
        granular_mix_target_reverb_ = true; // knobs -> Reverb Send
    else if(screen_ == Screen::Mixer)
        mixer_target_reverb_ = true; // knobs -> Reverb Send (ignored on Master)
    // File's and Pad Preset's short-tap behavior (Save As New) is
    // handled directly in HandleButton2() instead, alongside their own
    // hold-to-Load logic -- see there. All other screens: Button2 is
    // unused (Status page's Button2 is handled separately in
    // HandleButton2 as a hold-to-confirm clear).
}

Ui::KnobContext Ui::CurrentKnobContext() const
{
    switch(screen_)
    {
        case Screen::Home: return KnobContext::Home;
        case Screen::Layer:
            switch(layer_page_)
            {
                case LayerPage::Status: return KnobContext::LayerStatus;
                case LayerPage::Speed: return KnobContext::LayerSpeed;
                case LayerPage::Filter: return KnobContext::LayerFilter;
                case LayerPage::Effect: return KnobContext::LayerEffect;
                case LayerPage::Reverb: return KnobContext::LayerReverb;
                case LayerPage::Gain: return KnobContext::LayerGain;
                default: return KnobContext::LayerStatus;
            }
        case Screen::Global:
            switch(global_page_)
            {
                case GlobalPage::Tempo: return KnobContext::GlobalTempo;
                case GlobalPage::Filter: return KnobContext::GlobalFilter;
                case GlobalPage::Reverb: return KnobContext::GlobalReverb;
                case GlobalPage::Speed: return KnobContext::GlobalSpeed;
                case GlobalPage::File: return KnobContext::GlobalFile;
                case GlobalPage::Export: return KnobContext::GlobalExport;
                case GlobalPage::Granular: return KnobContext::GlobalGranular;
                case GlobalPage::Looper: return KnobContext::GlobalLooper;
                case GlobalPage::Mixer: return KnobContext::GlobalMixer;
                case GlobalPage::SdMgmt: return KnobContext::GlobalSdMgmt;
                case GlobalPage::Dexed: return KnobContext::GlobalDexed;
                default: return KnobContext::GlobalTempo;
            }
        case Screen::Dexed:
            switch(dexed_param_page_)
            {
                case DexedParamPage::Algo: return KnobContext::DexedAlgo;
                case DexedParamPage::Feedback: return KnobContext::DexedFeedback;
                case DexedParamPage::Vibrato: return KnobContext::DexedVibrato;
                case DexedParamPage::Brightness: return KnobContext::DexedBrightness;
                case DexedParamPage::EnvSpeed: return KnobContext::DexedEnvSpeed;
                case DexedParamPage::Filter: return KnobContext::DexedFilter;
                case DexedParamPage::Mix: return KnobContext::DexedMix;
                case DexedParamPage::Advanced: return KnobContext::DexedAdvanced;
                case DexedParamPage::Preset: return KnobContext::DexedPreset;
                default: return KnobContext::DexedAlgo;
            }
        case Screen::DexedOperator:
            switch(dexed_op_page_)
            {
                case DexedOpParamPage::RatioLevel: return KnobContext::DexedOpRatioLevel;
                case DexedOpParamPage::Detune: return KnobContext::DexedOpDetune;
                case DexedOpParamPage::EgRate:
                    return dexed_op_egrate_target_sr_ ? KnobContext::DexedOpEgRateSR
                                                          : KnobContext::DexedOpEgRateAD;
                case DexedOpParamPage::EgLevel:
                    return dexed_op_eglevel_target_sr_ ? KnobContext::DexedOpEgLevelSR
                                                           : KnobContext::DexedOpEgLevelAD;
                default: return KnobContext::DexedOpRatioLevel;
            }
        case Screen::Granular:
            switch(granular_param_page_)
            {
                case GranularParamPage::Grain:
                    return granular_grain_target_gap_scan_ ? KnobContext::GranularGrainGapScan
                                                              : KnobContext::GranularGrainSizeFill;
                case GranularParamPage::Position: return KnobContext::GranularPosition;
                case GranularParamPage::TuneDirection:
                    return KnobContext::GranularTuneDirection;
                case GranularParamPage::ADSR:
                    return granular_adsr_target_sr_ ? KnobContext::GranularEnvSR
                                                       : KnobContext::GranularEnvAD;
                case GranularParamPage::Filter: return KnobContext::GranularFilter;
                case GranularParamPage::Mix:
                    return granular_mix_target_reverb_ ? KnobContext::GranularMixReverb
                                                          : KnobContext::GranularMix;
                case GranularParamPage::Capture: return KnobContext::GranularCapture;
                case GranularParamPage::Trim: return KnobContext::GranularTrim;
                case GranularParamPage::Preset: return KnobContext::GranularPreset;
                default: return KnobContext::GranularGrainSizeFill;
            }
        case Screen::Mixer:
            if(mixer_position_ >= kNumMixerChannels) // Scope stop
                return KnobContext::MixerNoKnobs;
            if(mixer_position_ == kNumMixerChannels - 1) // Master
                return KnobContext::MixerMaster;
            return mixer_target_reverb_ ? KnobContext::MixerReverb : KnobContext::MixerVolPan;
    }
    return KnobContext::Home;
}

void Ui::SyncPickupTargets(KnobContext ctx)
{
    size_t i = (size_t)ctx;
    switch(ctx)
    {
        case KnobContext::Home:
            k1_pickup_raw_[i] = master_volume01_;
            k2_pickup_raw_[i] = tempo_->GetMetronomeVolume01();
            break;
        case KnobContext::LayerStatus:
            k1_pickup_raw_[i] = Cur().GetVolume01();
            k2_pickup_raw_[i] = Cur().GetPan01();
            break;
        case KnobContext::LayerSpeed:
            k1_pickup_raw_[i] = Cur().GetSpeed01();
            break;
        case KnobContext::LayerFilter:
            k1_pickup_raw_[i] = Cur().GetFilterCutoff01();
            k2_pickup_raw_[i] = Cur().GetFilterResonance01();
            break;
        case KnobContext::LayerEffect:
            k1_pickup_raw_[i] = Cur().GetEffectParamA01();
            k2_pickup_raw_[i] = Cur().GetEffectParamB01();
            break;
        case KnobContext::LayerReverb:
            // Knob2 does nothing here any more -- Size moved to
            // Global:Reverb (see the shared-bus comment on
            // LooperLayer::SetReverbSend01()).
            k1_pickup_raw_[i] = Cur().GetReverbSend01();
            break;
        case KnobContext::LayerGain:
            k1_pickup_raw_[i] = Cur().GetInputGain01();
            break;
        case KnobContext::GlobalTempo:
            // No raw01 storage for BPM/Bars (SetBpm/SetBars take the
            // actual value directly, unlike every LooperLayer control) --
            // invert their known forward mappings from ApplyKnobs() below.
            // Bars' inverse uses the bucket midpoint since its forward
            // map floors to an integer (a whole range of k2 produces the
            // same Bars value); BPM's is exact (SetBpm is continuous).
            k1_pickup_raw_[i] = Clampf((tempo_->GetBpm() - 40.f) / 200.f, 0.f, 1.f);
            k2_pickup_raw_[i]
                = Clampf(((float)(tempo_->GetBars() - 1) + 0.5f) / 15.99f, 0.f, 1.f);
            break;
        case KnobContext::GlobalFilter:
            k1_pickup_raw_[i] = master_filter_cutoff01_;
            k2_pickup_raw_[i] = master_filter_res01_;
            break;
        case KnobContext::GlobalReverb:
            k1_pickup_raw_[i] = reverb_size01_;
            k2_pickup_raw_[i] = bypass_reverb_send01_;
            break;
        case KnobContext::GlobalSpeed:
            k1_pickup_raw_[i] = project_speed01_;
            break;
        case KnobContext::GlobalFile: break; // browses a list directly, no pickup used
        case KnobContext::GlobalExport: break; // no continuous knob values, Button1 triggers it
        case KnobContext::GlobalGranular: break; // entry point only -- see Screen::Granular instead
        case KnobContext::GlobalLooper: break; // no continuous knobs, Button1 toggle only
        case KnobContext::GlobalMixer: break; // entry point only -- see Screen::Mixer instead
        case KnobContext::GlobalSdMgmt: break; // browses a list directly, no pickup used
        case KnobContext::GlobalDexed: break; // entry point only -- see Screen::Dexed instead
        case KnobContext::DexedAlgo:
            if(dexed_)
                k1_pickup_raw_[i] = (float)dexed_->GetPatchByte(134) / 32.f;
            break;
        case KnobContext::DexedFeedback:
            if(dexed_)
                k1_pickup_raw_[i] = (float)dexed_->GetPatchByte(135) / 7.f;
            break;
        case KnobContext::DexedVibrato:
            if(dexed_)
            {
                k1_pickup_raw_[i] = (float)dexed_->GetPatchByte(137) / 99.f;
                k2_pickup_raw_[i] = (float)dexed_->GetPatchByte(139) / 99.f;
            }
            break;
        case KnobContext::DexedBrightness:
            if(dexed_)
                k1_pickup_raw_[i] = dexed_->GetBrightness01();
            break;
        case KnobContext::DexedEnvSpeed:
            if(dexed_)
                k1_pickup_raw_[i] = dexed_->GetEnvSpeed01();
            break;
        case KnobContext::DexedFilter:
            if(dexed_)
            {
                k1_pickup_raw_[i] = dexed_->GetFilterCutoff01();
                k2_pickup_raw_[i] = dexed_->GetFilterResonance01();
            }
            break;
        case KnobContext::DexedMix:
            if(dexed_)
            {
                k1_pickup_raw_[i] = dexed_->GetReverbSend01();
                k2_pickup_raw_[i] = dexed_->GetOutputLevel01();
            }
            break;
        case KnobContext::DexedAdvanced: break; // entry point only -- see Screen::DexedOperator instead
        case KnobContext::DexedPreset: break; // browses a list directly, no pickup used
        case KnobContext::DexedOpRatioLevel:
            if(dexed_)
            {
                int base = dexed_op_index_ * 21;
                k1_pickup_raw_[i] = (float)dexed_->GetPatchByte(base + 18) / 32.f; // coarse
                k2_pickup_raw_[i] = (float)dexed_->GetPatchByte(base + 16) / 99.f; // output level
            }
            break;
        case KnobContext::DexedOpDetune:
            if(dexed_)
            {
                int base = dexed_op_index_ * 21;
                k1_pickup_raw_[i] = (float)dexed_->GetPatchByte(base + 20) / 14.f;
            }
            break;
        case KnobContext::DexedOpEgRateAD:
            if(dexed_)
            {
                int base = dexed_op_index_ * 21;
                k1_pickup_raw_[i] = (float)dexed_->GetPatchByte(base + 0) / 99.f; // Rate1
                k2_pickup_raw_[i] = (float)dexed_->GetPatchByte(base + 1) / 99.f; // Rate2
            }
            break;
        case KnobContext::DexedOpEgRateSR:
            if(dexed_)
            {
                int base = dexed_op_index_ * 21;
                k1_pickup_raw_[i] = (float)dexed_->GetPatchByte(base + 2) / 99.f; // Rate3
                k2_pickup_raw_[i] = (float)dexed_->GetPatchByte(base + 3) / 99.f; // Rate4
            }
            break;
        case KnobContext::DexedOpEgLevelAD:
            if(dexed_)
            {
                int base = dexed_op_index_ * 21;
                k1_pickup_raw_[i] = (float)dexed_->GetPatchByte(base + 4) / 99.f; // Level1
                k2_pickup_raw_[i] = (float)dexed_->GetPatchByte(base + 5) / 99.f; // Level2
            }
            break;
        case KnobContext::DexedOpEgLevelSR:
            if(dexed_)
            {
                int base = dexed_op_index_ * 21;
                k1_pickup_raw_[i] = (float)dexed_->GetPatchByte(base + 6) / 99.f; // Level3
                k2_pickup_raw_[i] = (float)dexed_->GetPatchByte(base + 7) / 99.f; // Level4
            }
            break;
        case KnobContext::GranularGrainSizeFill:
            if(granular_)
            {
                k1_pickup_raw_[i] = granular_->GetSize01();
                k2_pickup_raw_[i] = granular_->GetFill01();
            }
            break;
        case KnobContext::GranularGrainGapScan:
            if(granular_)
            {
                k1_pickup_raw_[i] = granular_->GetGap01();
                k2_pickup_raw_[i] = granular_->GetScan01();
            }
            break;
        case KnobContext::GranularPosition:
            if(granular_)
            {
                k1_pickup_raw_[i] = granular_->GetPosition01();
                k2_pickup_raw_[i] = granular_->GetScanPosition01();
            }
            break;
        case KnobContext::GranularTuneDirection:
            if(granular_)
            {
                k1_pickup_raw_[i] = granular_->GetGrainTuneSemitones01();
                k2_pickup_raw_[i] = granular_->GetDirection01();
            }
            break;
        case KnobContext::GranularEnvAD:
            if(granular_)
            {
                k1_pickup_raw_[i] = granular_->GetAttack01();
                k2_pickup_raw_[i] = granular_->GetDecay01();
            }
            break;
        case KnobContext::GranularEnvSR:
            if(granular_)
            {
                k1_pickup_raw_[i] = granular_->GetSustain01();
                k2_pickup_raw_[i] = granular_->GetRelease01();
            }
            break;
        case KnobContext::GranularFilter:
            if(granular_)
            {
                k1_pickup_raw_[i] = granular_->GetFilterCutoff01();
                k2_pickup_raw_[i] = granular_->GetFilterResonance01();
            }
            break;
        case KnobContext::GranularMix:
            if(granular_)
            {
                k1_pickup_raw_[i] = granular_->GetGrainVolume01();
                k2_pickup_raw_[i] = granular_->GetScanVolume01();
            }
            break;
        case KnobContext::GranularMixReverb:
            if(granular_)
                k1_pickup_raw_[i] = granular_->GetReverbSend01();
            break;
        case KnobContext::GranularCapture: break; // no continuous knobs, Button1/Button2 only
        case KnobContext::GranularTrim:
            k1_pickup_raw_[i] = granular_trim_start01_;
            k2_pickup_raw_[i] = granular_trim_end01_;
            break;
        case KnobContext::GranularPreset: break; // browses a list directly, no pickup used
        case KnobContext::MixerVolPan:
            k1_pickup_raw_[i] = MixerGetVolume01(mixer_position_);
            k2_pickup_raw_[i] = MixerGetPan01(mixer_position_);
            break;
        case KnobContext::MixerReverb:
            k1_pickup_raw_[i] = MixerGetSend01(mixer_position_);
            break;
        case KnobContext::MixerMaster:
            k1_pickup_raw_[i] = master_volume01_;
            k2_pickup_raw_[i] = reverb_size01_;
            break;
        case KnobContext::MixerNoKnobs: break; // Scope stop -- no continuous knobs
        default: break;
    }
}

bool Ui::KnobPickUp(float raw, float& stored_raw, bool& engaged)
{
    constexpr float kKnobPickupEpsilon = 0.04f;
    if(!engaged)
    {
        if(fabsf(raw - stored_raw) > kKnobPickupEpsilon)
            return false;
        engaged = true;
    }
    stored_raw = raw;
    return true;
}

void Ui::SetProjectSpeed01(float v)
{
    project_speed01_ = Clampf(v, 0.f, 1.f);
    project_speed_   = SpeedCurve01(project_speed01_);
}

void Ui::ScrubBy(int32_t inc)
{
    // ~50ms @48kHz per encoder detent at rest -- tune by feel.
    constexpr float kScrubSamplesPerClick = 2400.f;

    // Turn-speed acceleration: ticks arriving close together (a fast
    // spin) scrub much further per click than slow, deliberate ones --
    // otherwise covering real distance means a lot of turning. 150ms
    // between ticks or slower stays at the base 1x/~50ms-per-click feel;
    // under ~19ms between ticks (fast spin) ramps up to the 8x cap
    // (~400ms per click). Both constants are starting guesses, tune by
    // feel on real hardware.
    uint32_t now  = System::GetNow();
    uint32_t dt   = now - last_scrub_tick_ms_;
    last_scrub_tick_ms_ = now;
    float accel = dt > 0 ? Clampf(150.f / (float)dt, 1.f, 8.f) : 8.f;

    float delta = (float)inc * kScrubSamplesPerClick * accel;
    int   rep   = -1; // lowest-indexed non-empty layer, same one DrawSpeedScreen()
                       // reads for the playhead -- also used to re-sync
                       // TempoClock's phase below, since without that the
                       // metronome/beat indicator/bar-start would just keep
                       // free-running from wherever they already were,
                       // completely disconnected from where scrub moved
                       // the audio to (see TempoClock::SetPhaseToPosition()).
    for(int i = 0; i < num_layers_; i++)
    {
        if(!layers_[i].HasContent())
            continue;
        if(rep < 0)
            rep = i;
        float len = (float)layers_[i].GetRecordedLength();
        float pos = layers_[i].GetPlayPosRaw() + delta;
        while(pos >= len) pos -= len;
        while(pos < 0.f) pos += len;
        layers_[i].SetPlayPosRaw(pos);
        if(i == rep)
            tempo_->SetPhaseToPosition(pos);
    }
}

void Ui::TogglePauseAll()
{
    loop_paused_ = !loop_paused_;
    for(int i = 0; i < num_layers_; i++)
        layers_[i].SetPaused(loop_paused_);
}

void Ui::ApplyKnobs()
{
    // Deadband the raw pot reading itself, once, before it fans out to
    // every KnobContext below -- see k1_committed_/k2_committed_'s
    // comment in ui.h for why AnalogControl's own filter doesn't already
    // cover this. Small enough to be inaudible/invisible as a step (a
    // fraction of the 0.02 pickup epsilon) while absorbing the pot's
    // resting noise so parameters stop wobbling when the knob isn't
    // actually being turned.
    constexpr float kPotDriftDeadband = 0.004f;
    float            raw_k1           = pod_->GetKnobValue(DaisyPod::KNOB_1);
    float            raw_k2           = pod_->GetKnobValue(DaisyPod::KNOB_2);
    if(fabsf(raw_k1 - k1_committed_) > kPotDriftDeadband)
        k1_committed_ = raw_k1;
    if(fabsf(raw_k2 - k2_committed_) > kPotDriftDeadband)
        k2_committed_ = raw_k2;
    float k1 = k1_committed_;
    float k2 = k2_committed_;

    // Re-arm pickup on every context change -- see the KnobPickUp()
    // comment in ui.h for why this doesn't force an unnecessary wiggle
    // when the knob genuinely didn't move. Also re-seed the pickup
    // target to this context's actual current value (SyncPickupTargets)
    // so the knob regains control as soon as it crosses the real value,
    // not wherever it was last left for a different layer sharing this
    // same context (or 0 if this context has never been engaged yet).
    KnobContext ctx = CurrentKnobContext();
    if(ctx != last_knob_context_)
    {
        size_t i              = (size_t)ctx;
        k1_pickup_engaged_[i] = false;
        k2_pickup_engaged_[i] = false;
        SyncPickupTargets(ctx);
        last_knob_context_    = ctx;
    }
    size_t ci = (size_t)ctx;

    switch(screen_)
    {
        case Screen::Home:
            if(KnobPickUp(k1, k1_pickup_raw_[ci], k1_pickup_engaged_[ci]))
            {
                // Same 0..1.43x curve the original firmware used for
                // master volume, so overall loudness feels consistent.
                master_volume01_ = Clampf(k1, 0.f, 1.f);
                master_volume_   = powf(master_volume01_, 2.5f) * 1.43f;
                if(master_volume_ < 0.f)
                    master_volume_ = 0.f;
            }
            if(KnobPickUp(k2, k2_pickup_raw_[ci], k2_pickup_engaged_[ci]))
                tempo_->SetMetronomeVolume01(k2);
            break;

        case Screen::Layer:
            switch(layer_page_)
            {
                case LayerPage::Status:
                    if(KnobPickUp(k1, k1_pickup_raw_[ci], k1_pickup_engaged_[ci]))
                        Cur().SetVolume01(k1);
                    if(KnobPickUp(k2, k2_pickup_raw_[ci], k2_pickup_engaged_[ci]))
                        Cur().SetPan01(k2);
                    break;
                case LayerPage::Speed:
                    if(KnobPickUp(k1, k1_pickup_raw_[ci], k1_pickup_engaged_[ci]))
                        Cur().SetSpeed01(k1);
                    break;
                case LayerPage::Filter:
                    if(KnobPickUp(k1, k1_pickup_raw_[ci], k1_pickup_engaged_[ci]))
                        Cur().SetFilterCutoff01(k1);
                    if(KnobPickUp(k2, k2_pickup_raw_[ci], k2_pickup_engaged_[ci]))
                        Cur().SetFilterResonance01(k2);
                    break;
                case LayerPage::Effect:
                    if(KnobPickUp(k1, k1_pickup_raw_[ci], k1_pickup_engaged_[ci]))
                        Cur().SetEffectParamA01(k1);
                    if(KnobPickUp(k2, k2_pickup_raw_[ci], k2_pickup_engaged_[ci]))
                        Cur().SetEffectParamB01(k2);
                    break;
                case LayerPage::Reverb:
                    // Only Send is per-layer -- Size is a shared
                    // Global:Reverb setting now (see the shared-bus
                    // comment on LooperLayer::SetReverbSend01()), so
                    // Knob2 does nothing on this page.
                    if(KnobPickUp(k1, k1_pickup_raw_[ci], k1_pickup_engaged_[ci]))
                        Cur().SetReverbSend01(k1);
                    break;
                case LayerPage::Gain:
                    if(KnobPickUp(k1, k1_pickup_raw_[ci], k1_pickup_engaged_[ci]))
                        Cur().SetInputGain01(k1);
                    break;
                default: break;
            }
            break;

        case Screen::Global:
            if(global_page_ == GlobalPage::Tempo && !tempo_->IsLocked())
            {
                if(KnobPickUp(k1, k1_pickup_raw_[ci], k1_pickup_engaged_[ci]))
                    tempo_->SetBpm(40.f + k1 * (240.f - 40.f));
                if(KnobPickUp(k2, k2_pickup_raw_[ci], k2_pickup_engaged_[ci]))
                    tempo_->SetBars(1 + (int)(k2 * 15.99f));
            }
            else if(global_page_ == GlobalPage::Filter)
            {
                if(KnobPickUp(k1, k1_pickup_raw_[ci], k1_pickup_engaged_[ci]))
                    master_filter_cutoff01_ = Clampf(k1, 0.f, 1.f);
                if(KnobPickUp(k2, k2_pickup_raw_[ci], k2_pickup_engaged_[ci]))
                    master_filter_res01_ = Clampf(k2, 0.f, 1.f);
            }
            else if(global_page_ == GlobalPage::Reverb)
            {
                if(KnobPickUp(k1, k1_pickup_raw_[ci], k1_pickup_engaged_[ci]))
                    reverb_size01_ = Clampf(k1, 0.f, 1.f);
                if(KnobPickUp(k2, k2_pickup_raw_[ci], k2_pickup_engaged_[ci]))
                    bypass_reverb_send01_ = Clampf(k2, 0.f, 1.f);
            }
            else if(global_page_ == GlobalPage::Speed)
            {
                if(KnobPickUp(k1, k1_pickup_raw_[ci], k1_pickup_engaged_[ci]))
                    SetProjectSpeed01(k1);
            }
            else if(global_page_ == GlobalPage::File)
            {
                if(save_load_mode_ == SaveLoadMode::ChoosingSave)
                {
                    // Overwrite is only a real option once something's
                    // actually loaded -- otherwise force Save New so
                    // Knob1 can't land on a choice that doesn't exist.
                    save_as_new_ = loaded_slot_ < 0 ? true : k1 >= 0.5f;
                }
                else if(save_load_mode_ == SaveLoadMode::BrowsingLoad)
                {
                    if(!load_browsing_files_)
                    {
                        // Top-level chooser (mirrors ChoosingSave's own
                        // Overwrite/Save New pick above) -- forced to New
                        // when there's nothing to browse, same "can't
                        // select what doesn't exist" reasoning.
                        load_new_selected_ = file_slot_count_ == 0 ? true : k1 >= 0.5f;
                    }
                    else if(file_slot_count_ > 0)
                    {
                        // Drilled into the numbered list (Button1's own
                        // tap, see OnButton1Short()) -- discretized
                        // browse, not pickup-tracked, same idiom as
                        // every other list-browse knob in this project.
                        int idx = (int)(Clampf(k1, 0.f, 1.f) * file_slot_count_);
                        if(idx >= file_slot_count_)
                            idx = file_slot_count_ - 1;
                        file_cursor_ = idx;
                    }
                }
            }
            // (GlobalPage::Mixer has no knobs of its own any more -- entry
            // point only, same as GlobalPage::Granular; see Screen::Mixer
            // for the real editing surface. No branch needed for it here.)
            else if(global_page_ == GlobalPage::SdMgmt)
            {
                if(!sd_mgmt_in_folder_)
                {
                    // Knob1 picks which folder -- discretized among the 3
                    // categories, same idiom as every other list-browse
                    // knob in this project.
                    int n   = (int)SdMgmtFolder::kCount;
                    int idx = (int)(Clampf(k1, 0.f, 1.f) * n);
                    if(idx >= n)
                        idx = n - 1;
                    sd_mgmt_folder_ = (SdMgmtFolder)idx;
                }
                else
                {
                    int count = 0;
                    SdMgmtSlots(&count);
                    if(count > 0)
                    {
                        int idx = (int)(Clampf(k1, 0.f, 1.f) * count);
                        if(idx >= count)
                            idx = count - 1;
                        if(idx != sd_mgmt_cursor_)
                            sd_mgmt_status_[0] = '\0';
                        sd_mgmt_cursor_ = idx;
                    }
                }
            }
            break;

        case Screen::Granular:
            if(!granular_)
                break;
            switch(granular_param_page_)
            {
                case GranularParamPage::Grain:
                    if(!granular_grain_target_gap_scan_)
                    {
                        if(KnobPickUp(k1, k1_pickup_raw_[ci], k1_pickup_engaged_[ci]))
                            granular_->SetSize01(k1);
                        if(KnobPickUp(k2, k2_pickup_raw_[ci], k2_pickup_engaged_[ci]))
                            granular_->SetFill01(k2);
                    }
                    else
                    {
                        if(KnobPickUp(k1, k1_pickup_raw_[ci], k1_pickup_engaged_[ci]))
                            granular_->SetGap01(k1);
                        if(KnobPickUp(k2, k2_pickup_raw_[ci], k2_pickup_engaged_[ci]))
                            granular_->SetScan01(k2);
                    }
                    break;
                case GranularParamPage::Position:
                    if(KnobPickUp(k1, k1_pickup_raw_[ci], k1_pickup_engaged_[ci]))
                        granular_->SetPosition01(k1);
                    if(KnobPickUp(k2, k2_pickup_raw_[ci], k2_pickup_engaged_[ci]))
                        granular_->SetScanPosition01(k2);
                    break;
                case GranularParamPage::TuneDirection:
                    if(KnobPickUp(k1, k1_pickup_raw_[ci], k1_pickup_engaged_[ci]))
                        granular_->SetGrainTuneSemitones01(k1);
                    if(KnobPickUp(k2, k2_pickup_raw_[ci], k2_pickup_engaged_[ci]))
                        granular_->SetDirection01(k2);
                    break;
                case GranularParamPage::ADSR:
                    if(!granular_adsr_target_sr_)
                    {
                        if(KnobPickUp(k1, k1_pickup_raw_[ci], k1_pickup_engaged_[ci]))
                            granular_->SetAttack01(k1);
                        if(KnobPickUp(k2, k2_pickup_raw_[ci], k2_pickup_engaged_[ci]))
                            granular_->SetDecay01(k2);
                    }
                    else
                    {
                        if(KnobPickUp(k1, k1_pickup_raw_[ci], k1_pickup_engaged_[ci]))
                            granular_->SetSustain01(k1);
                        if(KnobPickUp(k2, k2_pickup_raw_[ci], k2_pickup_engaged_[ci]))
                            granular_->SetRelease01(k2);
                    }
                    break;
                case GranularParamPage::Filter:
                    if(KnobPickUp(k1, k1_pickup_raw_[ci], k1_pickup_engaged_[ci]))
                        granular_->SetFilterCutoff01(k1);
                    if(KnobPickUp(k2, k2_pickup_raw_[ci], k2_pickup_engaged_[ci]))
                        granular_->SetFilterResonance01(k2);
                    break;
                case GranularParamPage::Mix:
                    if(!granular_mix_target_reverb_)
                    {
                        if(KnobPickUp(k1, k1_pickup_raw_[ci], k1_pickup_engaged_[ci]))
                            granular_->SetGrainVolume01(k1);
                        if(KnobPickUp(k2, k2_pickup_raw_[ci], k2_pickup_engaged_[ci]))
                            granular_->SetScanVolume01(k2);
                    }
                    else
                    {
                        if(KnobPickUp(k1, k1_pickup_raw_[ci], k1_pickup_engaged_[ci]))
                            granular_->SetReverbSend01(k1);
                    }
                    break;
                case GranularParamPage::Capture:
                {
                    // Only meaningful in Import mode -- Direct Record and
                    // From Layer have no continuous knob use here. Same
                    // discretized "browse a list directly" idiom as
                    // Preset's own knob below, no pickup tracking.
                    if(granular_capture_source_ == -2)
                    {
                        int total = granular_import_file_count_;
                        if(total > 0)
                        {
                            int idx = (int)(Clampf(k1, 0.f, 1.f) * total);
                            if(idx >= total)
                                idx = total - 1;
                            if(idx != granular_import_cursor_)
                                granular_capture_status_[0] = '\0';
                            granular_import_cursor_ = idx;
                        }
                    }
                    break;
                }
                case GranularParamPage::Trim:
                {
                    bool changed = false;
                    if(KnobPickUp(k1, k1_pickup_raw_[ci], k1_pickup_engaged_[ci]))
                    {
                        granular_trim_start01_ = k1;
                        changed                = true;
                    }
                    if(KnobPickUp(k2, k2_pickup_raw_[ci], k2_pickup_engaged_[ci]))
                    {
                        granular_trim_end01_ = k2;
                        changed              = true;
                    }
                    if(changed)
                        ApplyGranularTrim();
                    break;
                }
                case GranularParamPage::Preset:
                {
                    if(save_load_mode_ == SaveLoadMode::ChoosingSave)
                    {
                        save_as_new_ = granular_loaded_preset_slot_ <= 0 ? true : k1 >= 0.5f;
                        break;
                    }
                    if(save_load_mode_ != SaveLoadMode::BrowsingLoad)
                        break;
                    if(!load_browsing_files_)
                    {
                        // Top-level chooser (mirrors ChoosingSave's own
                        // Overwrite/Save New pick above) -- forced to New
                        // when there's nothing to browse, same "can't
                        // select what doesn't exist" reasoning.
                        load_new_selected_
                            = granular_preset_user_slot_count_ == 0 ? true : k1 >= 0.5f;
                        break;
                    }
                    // Drilled into the numbered list (Button1's own tap,
                    // see OnButton1Short()) -- discretized browse, not
                    // pickup-tracked, same idiom as Global:File's own
                    // file_cursor_ (no factory range here to fold in).
                    if(granular_preset_user_slot_count_ > 0)
                    {
                        int total = granular_preset_user_slot_count_;
                        int idx   = (int)(Clampf(k1, 0.f, 1.f) * total);
                        if(idx >= total)
                            idx = total - 1;
                        if(idx != granular_preset_cursor_)
                            granular_preset_status_[0] = '\0';
                        granular_preset_cursor_ = idx;
                    }
                    break;
                }
                default: break;
            }
            break;

        case Screen::Dexed:
            if(!dexed_)
                break;
            switch(dexed_param_page_)
            {
                // Every case below that touches dexed_'s own
                // SetPatchByte()/SetBrightness01()/SetEnvSpeed01() gates
                // on a real "did this actually change" check before
                // calling it -- unlike every other engine's own knob-
                // driven setter in this project (a cheap float
                // assignment KnobPickUp()'s own lack of a change-check
                // was always harmless to call every tick against), these
                // trigger a real per-held-voice Dx7Note::update() call
                // (see ApplyPatchToHeldVoices()) inside a brief critical
                // section (see NoteOn()'s own comment on why that's
                // needed at all). Calling that every single main-loop
                // tick the knob is engaged -- including while sitting
                // still, since KnobPickUp() itself never checks for
                // real movement -- was enough cumulative critical-
                // section time with several notes held to audibly drop
                // notes while turning the knob, confirmed on real
                // hardware.
                case DexedParamPage::Algo:
                    if(KnobPickUp(k1, k1_pickup_raw_[ci], k1_pickup_engaged_[ci]))
                    {
                        int algo = (int)(Clampf(k1, 0.f, 1.f) * 32.f);
                        algo     = algo > 31 ? 31 : algo;
                        if((uint8_t)algo != dexed_->GetPatchByte(134))
                            dexed_->SetPatchByte(134, (uint8_t)algo);
                    }
                    break;
                case DexedParamPage::Feedback:
                    if(KnobPickUp(k1, k1_pickup_raw_[ci], k1_pickup_engaged_[ci]))
                    {
                        int fb = (int)(Clampf(k1, 0.f, 1.f) * 7.f + 0.5f);
                        fb     = fb > 7 ? 7 : fb;
                        if((uint8_t)fb != dexed_->GetPatchByte(135))
                            dexed_->SetPatchByte(135, (uint8_t)fb);
                    }
                    break;
                case DexedParamPage::Vibrato:
                    if(KnobPickUp(k1, k1_pickup_raw_[ci], k1_pickup_engaged_[ci]))
                    {
                        int speed = (int)(Clampf(k1, 0.f, 1.f) * 99.f + 0.5f);
                        speed     = speed > 99 ? 99 : speed;
                        if((uint8_t)speed != dexed_->GetPatchByte(137))
                            dexed_->SetPatchByte(137, (uint8_t)speed);
                    }
                    if(KnobPickUp(k2, k2_pickup_raw_[ci], k2_pickup_engaged_[ci]))
                    {
                        int depth = (int)(Clampf(k2, 0.f, 1.f) * 99.f + 0.5f);
                        depth     = depth > 99 ? 99 : depth;
                        if((uint8_t)depth != dexed_->GetPatchByte(139))
                            dexed_->SetPatchByte(139, (uint8_t)depth);
                    }
                    break;
                case DexedParamPage::Brightness:
                    if(KnobPickUp(k1, k1_pickup_raw_[ci], k1_pickup_engaged_[ci])
                       && fabsf(k1 - dexed_->GetBrightness01()) > 0.002f)
                        dexed_->SetBrightness01(k1);
                    break;
                case DexedParamPage::EnvSpeed:
                    if(KnobPickUp(k1, k1_pickup_raw_[ci], k1_pickup_engaged_[ci])
                       && fabsf(k1 - dexed_->GetEnvSpeed01()) > 0.002f)
                        dexed_->SetEnvSpeed01(k1);
                    break;
                case DexedParamPage::Filter:
                    if(KnobPickUp(k1, k1_pickup_raw_[ci], k1_pickup_engaged_[ci]))
                        dexed_->SetFilterCutoff01(k1);
                    if(KnobPickUp(k2, k2_pickup_raw_[ci], k2_pickup_engaged_[ci]))
                        dexed_->SetFilterResonance01(k2);
                    break;
                case DexedParamPage::Mix:
                    if(KnobPickUp(k1, k1_pickup_raw_[ci], k1_pickup_engaged_[ci]))
                        dexed_->SetReverbSend01(k1);
                    if(KnobPickUp(k2, k2_pickup_raw_[ci], k2_pickup_engaged_[ci]))
                        dexed_->SetOutputLevel01(k2);
                    break;
                case DexedParamPage::Advanced: break; // no knobs, encoder click drills in
                case DexedParamPage::Preset:
                {
                    if(save_load_mode_ == SaveLoadMode::ChoosingSave)
                    {
                        // Overwrite is only a real option once a real
                        // user slot (not a factory one) is loaded.
                        save_as_new_
                            = dexed_loaded_preset_slot_ <= DexedSynth::GetNumFactoryPresets()
                                  ? true
                                  : k1 >= 0.5f;
                        break;
                    }
                    if(save_load_mode_ != SaveLoadMode::BrowsingLoad)
                        break;
                    if(!load_browsing_files_)
                    {
                        // Top-level chooser (mirrors ChoosingSave's own
                        // Overwrite/Save New pick above) -- factory
                        // presets always exist, so "Files" is always a
                        // real option here.
                        load_new_selected_ = k1 >= 0.5f;
                        break;
                    }
                    // One extra folder level versus Global:File/Granular
                    // Preset's own single numbered list (see
                    // dexed_preset_folder_open_'s own comment) -- K1
                    // scrolls whichever of the two is currently active,
                    // discretized/not pickup-tracked, same idiom as
                    // Global:File's own file_cursor_.
                    if(!dexed_preset_folder_open_)
                    {
                        // Folder list -- kNumFactoryCategories named
                        // categories plus one trailing "User" folder.
                        int total = DexedSynth::kNumFactoryCategories + 1;
                        int idx   = (int)(Clampf(k1, 0.f, 1.f) * total);
                        if(idx >= total)
                            idx = total - 1;
                        dexed_preset_folder_cursor_ = idx;
                        break;
                    }
                    {
                        // Inside a folder -- browsing either one factory
                        // category's own presets, or (the trailing
                        // folder) every user-saved slot.
                        int total
                            = dexed_preset_folder_cursor_ < DexedSynth::kNumFactoryCategories
                                  ? DexedSynth::GetFactoryCategoryCount(
                                        dexed_preset_folder_cursor_)
                                  : dexed_preset_user_slot_count_;
                        if(total <= 0)
                            break;
                        int idx = (int)(Clampf(k1, 0.f, 1.f) * total);
                        if(idx >= total)
                            idx = total - 1;
                        // Browsing again -- clear the last save/load
                        // result so the "Load:" line (which shows what
                        // the knob is actually pointing at right now)
                        // comes back instead of staying stuck on a
                        // status message that never otherwise clears.
                        if(idx != dexed_preset_cursor_)
                            dexed_preset_status_[0] = '\0';
                        dexed_preset_cursor_ = idx;
                    }
                    break;
                }
                default: break;
            }
            break;

        case Screen::DexedOperator:
        {
            if(!dexed_)
                break;
            int base = dexed_op_index_ * 21;
            switch(dexed_op_page_)
            {
                case DexedOpParamPage::RatioLevel:
                    if(KnobPickUp(k1, k1_pickup_raw_[ci], k1_pickup_engaged_[ci]))
                    {
                        int coarse = (int)(Clampf(k1, 0.f, 1.f) * 32.f);
                        coarse     = coarse > 31 ? 31 : coarse;
                        if((uint8_t)coarse != dexed_->GetPatchByte(base + 18))
                            dexed_->SetPatchByte(base + 18, (uint8_t)coarse);
                    }
                    if(KnobPickUp(k2, k2_pickup_raw_[ci], k2_pickup_engaged_[ci]))
                    {
                        int level = (int)(Clampf(k2, 0.f, 1.f) * 99.f + 0.5f);
                        level     = level > 99 ? 99 : level;
                        if((uint8_t)level != dexed_->GetPatchByte(base + 16))
                            dexed_->SetPatchByte(base + 16, (uint8_t)level);
                    }
                    break;
                case DexedOpParamPage::Detune:
                    if(KnobPickUp(k1, k1_pickup_raw_[ci], k1_pickup_engaged_[ci]))
                    {
                        int detune = (int)(Clampf(k1, 0.f, 1.f) * 14.f + 0.5f);
                        detune     = detune > 14 ? 14 : detune;
                        if((uint8_t)detune != dexed_->GetPatchByte(base + 20))
                            dexed_->SetPatchByte(base + 20, (uint8_t)detune);
                    }
                    break;
                case DexedOpParamPage::EgRate:
                {
                    int off1 = dexed_op_egrate_target_sr_ ? base + 2 : base + 0;
                    int off2 = dexed_op_egrate_target_sr_ ? base + 3 : base + 1;
                    if(KnobPickUp(k1, k1_pickup_raw_[ci], k1_pickup_engaged_[ci]))
                    {
                        int rate = (int)(Clampf(k1, 0.f, 1.f) * 99.f + 0.5f);
                        rate     = rate > 99 ? 99 : rate;
                        if((uint8_t)rate != dexed_->GetPatchByte(off1))
                            dexed_->SetPatchByte(off1, (uint8_t)rate);
                    }
                    if(KnobPickUp(k2, k2_pickup_raw_[ci], k2_pickup_engaged_[ci]))
                    {
                        int rate = (int)(Clampf(k2, 0.f, 1.f) * 99.f + 0.5f);
                        rate     = rate > 99 ? 99 : rate;
                        if((uint8_t)rate != dexed_->GetPatchByte(off2))
                            dexed_->SetPatchByte(off2, (uint8_t)rate);
                    }
                    break;
                }
                case DexedOpParamPage::EgLevel:
                {
                    int off1 = dexed_op_eglevel_target_sr_ ? base + 6 : base + 4;
                    int off2 = dexed_op_eglevel_target_sr_ ? base + 7 : base + 5;
                    if(KnobPickUp(k1, k1_pickup_raw_[ci], k1_pickup_engaged_[ci]))
                    {
                        int level = (int)(Clampf(k1, 0.f, 1.f) * 99.f + 0.5f);
                        level     = level > 99 ? 99 : level;
                        if((uint8_t)level != dexed_->GetPatchByte(off1))
                            dexed_->SetPatchByte(off1, (uint8_t)level);
                    }
                    if(KnobPickUp(k2, k2_pickup_raw_[ci], k2_pickup_engaged_[ci]))
                    {
                        int level = (int)(Clampf(k2, 0.f, 1.f) * 99.f + 0.5f);
                        level     = level > 99 ? 99 : level;
                        if((uint8_t)level != dexed_->GetPatchByte(off2))
                            dexed_->SetPatchByte(off2, (uint8_t)level);
                    }
                    break;
                }
                default: break;
            }
            break;
        }

        case Screen::Mixer:
        {
            int ch = mixer_position_;
            if(ch >= kNumMixerChannels) // Scope stop -- no continuous knobs
                break;
            if(ch == kNumMixerChannels - 1) // Master: Volume + Reverb Size, no toggle
            {
                if(KnobPickUp(k1, k1_pickup_raw_[ci], k1_pickup_engaged_[ci]))
                {
                    master_volume01_ = Clampf(k1, 0.f, 1.f);
                    master_volume_   = powf(master_volume01_, 2.5f) * 1.43f;
                    if(master_volume_ < 0.f)
                        master_volume_ = 0.f;
                }
                if(KnobPickUp(k2, k2_pickup_raw_[ci], k2_pickup_engaged_[ci]))
                    reverb_size01_ = Clampf(k2, 0.f, 1.f);
            }
            else if(!mixer_target_reverb_)
            {
                if(KnobPickUp(k1, k1_pickup_raw_[ci], k1_pickup_engaged_[ci]))
                    MixerSetVolume01(ch, k1);
                if(KnobPickUp(k2, k2_pickup_raw_[ci], k2_pickup_engaged_[ci]))
                    MixerSetPan01(ch, k2);
            }
            else
            {
                if(KnobPickUp(k1, k1_pickup_raw_[ci], k1_pickup_engaged_[ci]))
                    MixerSetSend01(ch, k1);
            }
            break;
        }
    }
}

void Ui::UpdateLeds()
{
    // Led1: at-a-glance state of the cursor layer, visible without
    // squinting at the OLED.
    LayerState s = layers_[cursor_layer_].GetState();
    switch(s)
    {
        case LayerState::Empty: pod_->led1.Set(0.f, 0.f, 0.f); break;
        case LayerState::ArmedCountIn: pod_->led1.Set(1.f, 0.4f, 0.f); break; // amber
        case LayerState::Recording: pod_->led1.Set(1.f, 0.f, 0.f); break;
        case LayerState::Overdubbing: pod_->led1.Set(1.f, 0.5f, 0.f); break;
        case LayerState::Playing: pod_->led1.Set(0.f, 1.f, 0.f); break;
        case LayerState::Paused: pod_->led1.Set(0.f, 0.f, 0.6f); break;
    }

    // Led2: steady Bypass on/off indicator (Home screen's Button2) -- was
    // a metronome flash before, but that could only ever be as accurate
    // as the main loop's call rate allows (see UpdateLeds()'s old comment
    // history), and under load never quite landed on the beat. A plain
    // on/off readout has no timing to get wrong.
    if(bypass_)
        pod_->led2.Set(1.f, 1.f, 1.f);
    else
        pod_->led2.Set(0.f, 0.f, 0.f);

    pod_->UpdateLeds();
}

// --- Drawing --------------------------------------------------------------

void Ui::Draw()
{
    disp_->Fill(false);
    switch(screen_)
    {
        case Screen::Home: DrawHome(); break;
        case Screen::Layer: DrawLayerScreen(); break;
        case Screen::Global: DrawGlobalScreen(); break;
        case Screen::Granular: DrawGranularScreen(); break;
        case Screen::Mixer: DrawMixerScreen(); break;
        case Screen::Dexed: DrawDexedScreen(); break;
        case Screen::DexedOperator: DrawDexedOperatorScreen(); break;
    }
    disp_->Update();
}

void Ui::WriteUpper(const char* text)
{
    char   buf[64];
    size_t n = strlen(text);
    if(n >= sizeof(buf))
        n = sizeof(buf) - 1;
    for(size_t i = 0; i < n; i++)
        buf[i] = (char)toupper((unsigned char)text[i]);
    buf[n] = '\0';
    disp_->WriteString(buf, Font_6x8, true);
}

void Ui::DrawControlRow(int         row_y,
                         bool       square_icon,
                         int        divider_y,
                         const char* left_label,
                         const char* left_value,
                         const char* right_value,
                         const char* right_label)
{
    // Geometry verified pixel-by-pixel against the real font metrics
    // (see the chat history this was designed in, or font_tomthumb.h):
    // baseline at the row's bottom pixel (NOT row_h-2 -- that crashes
    // tall glyphs into the divider above), icon vertically centered on
    // the rows text actually occupies (row_y..row_y+4, not the full
    // row_h-tall cell, which would sit the icon low against the margin).
    constexpr int kRowH      = kTomThumbRowHeight; // 6
    constexpr int kIconR     = 2;
    constexpr int kClearance = 2;
    constexpr int kMargin    = 1;

    if(divider_y >= 0)
        disp_->DrawLine(0, divider_y, disp_->Width() - 1, divider_y, true);

    const int baseline = row_y + kRowH - 1;
    const int icon_cx  = disp_->Width() / 2;
    const int icon_cy  = row_y + 2;

    TomThumbDrawText(disp_, kMargin, baseline, left_label, true);
    if(right_label[0] != '\0')
    {
        int rw = TomThumbAdvanceWidth(right_label);
        TomThumbDrawText(disp_, disp_->Width() - kMargin - rw, baseline, right_label, true);
    }

    if(left_value[0] != '\0')
    {
        int left_ink_w         = TomThumbInkWidth(left_value);
        int left_exclusive_end = icon_cx - kIconR - kClearance;
        TomThumbDrawText(disp_, left_exclusive_end - left_ink_w, baseline, left_value, true);
    }
    if(right_value[0] != '\0')
    {
        int right_start = icon_cx + kIconR + kClearance + 1;
        TomThumbDrawText(disp_, right_start, baseline, right_value, true);
    }

    if(square_icon)
        disp_->DrawRect(icon_cx - kIconR, icon_cy - kIconR, icon_cx + kIconR,
                         icon_cy + kIconR, true, false);
    else
        disp_->DrawCircle(icon_cx, icon_cy, kIconR, true);
}

void Ui::DrawBeatIndicator(int x, int y, int dot_size)
{
    // Reserve fixed space for the "B<n>" label -- up to "B16" (3 chars)
    // -- whether or not it's actually drawn this call, so the dots
    // never shift position as the bar number's digit count changes
    // (e.g. "B9" -> "B10") mid-performance.
    if(tempo_->GetBars() > 1)
    {
        char label[6];
        snprintf(label, sizeof(label), "B%d", tempo_->GetBarInLoop() + 1);
        disp_->SetCursor(x, y);
        WriteUpper(label);
    }
    int dots_x = x + 3 * 6 + 3; // 3 reserved chars @ Font_6x8 + a small gap

    const int gap = dot_size >= 5 ? 3 : 2;
    int       beat = tempo_->GetBeatInBar();
    for(int i = 0; i < TempoClock::kBeatsPerBar; i++)
    {
        bool filled = (i == beat);
        disp_->DrawRect(
            dots_x, y, dots_x + dot_size - 1, y + dot_size - 1, true, filled);
        dots_x += dot_size + gap;
    }
}

void Ui::DrawHome()
{
    // Beat position, prominent (dot_size=6) -- this is the primary
    // "where am I" readout. BPM/Bars/Metro share its row in Tom Thumb
    // (verified to fit: the beat indicator -- 3-char "B<n>" reserve plus
    // 4 dots -- occupies x2-55, leaving ~70px, and "BPM:240 BARS:16 [M]"
    // (worst case: max BPM, max Bars) is 67px) instead of getting its own
    // row below.
    DrawBeatIndicator(2, 0, 6);

    char line[24];
    snprintf(line, sizeof(line), "BPM:%d BARS:%d %s",
              (int)(tempo_->GetBpm() + 0.5f), tempo_->GetBars(),
              tempo_->IsMetronomeEnabled() ? "[M]" : "[ ]");
    // Right-aligned, 1px from the edge -- same margin convention as the
    // footer rows. Worst case (max BPM/Bars) is 67px, so this still
    // clears the beat indicator's right edge (x55) with room to spare.
    int line_w = TomThumbAdvanceWidth(line);
    TomThumbDrawText(disp_, disp_->Width() - 1 - line_w, kTomThumbRowHeight - 1, line, true);

    // Layer boxes, sized to fill the full display width evenly no
    // matter how many layers there are -- kNumLayers has changed once
    // already (5 -> 4) and might again, so this shouldn't hard-code a
    // box width tuned for one specific count.
    const int box_gap = 4;
    const int box_w = (disp_->Width() - (num_layers_ - 1) * box_gap) / num_layers_;
    // Vertically centered in the space between the top row's content
    // (beat dots + BPM/Bars/Metro text, rows 0-5) and the footer divider
    // (row kFooterDividerY=46) -- that's 40 rows (6..45) for a 24px box,
    // so top=14 splits the remaining 16px into an 8px gap on each side.
    const int box_h = 24, top = 14;
    for(int i = 0; i < num_layers_; i++)
    {
        int x0 = i * (box_w + box_gap);
        int x1 = x0 + box_w - 1;
        int y1 = top + box_h - 1;
        bool selected = (i == cursor_layer_);
        disp_->DrawRect(x0, top, x1, y1, true, false);
        if(selected)
            disp_->DrawRect(x0 + 1, top + 1, x1 - 1, y1 - 1, true, false);

        char label[8];
        snprintf(label, sizeof(label), "%d%s", i + 1, StateGlyph(layers_[i].GetState()));
        disp_->SetCursor(x0 + 4, top + 8);
        WriteUpper(label);
    }

    // Master Volume/Metronome Volume shown live from their actual
    // values, NOT k1_pickup_raw_/k2_pickup_raw_ -- those track the raw
    // knob position for the pickup/catch mechanism (see KnobPickUp()'s
    // comment in ui.h) and only update once a knob is actually caught,
    // so on first landing here after a layer switch they'd still be
    // sitting at their stale default instead of the real value.
    char vol_val[8], metro_val[8];
    snprintf(vol_val, sizeof(vol_val), "%d%%", (int)(master_volume01_ * 100.f + 0.5f));
    snprintf(metro_val, sizeof(metro_val), "%d%%",
              (int)(tempo_->GetMetronomeVolume01() * 100.f + 0.5f));
    DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "Vol", vol_val, metro_val, "Metro");

    // Button1's label reflects what a tap/hold will actually do right now
    // (see LooperLayer::OnRecordButtonPressed/LongPress) rather than a
    // static "Rec/Play" covering three different behaviours -- once
    // there's content, a tap toggles Pause/Play and a hold starts an
    // overdub, both unchanged; this only changes what the label says.
    const char* rec_label = "Rec";
    switch(layers_[cursor_layer_].GetState())
    {
        case LayerState::Empty: rec_label = "Rec"; break;
        case LayerState::Playing:
        case LayerState::Paused: rec_label = "Pause/Overdub"; break;
        case LayerState::ArmedCountIn: rec_label = "Count-in"; break;
        case LayerState::Recording: rec_label = "Stop"; break;
        case LayerState::Overdubbing: rec_label = "Overdubbing"; break;
    }
    char byp_label[12];
    snprintf(byp_label, sizeof(byp_label), "Bypass:%s", bypass_ ? "On" : "Off");
    DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, rec_label, "", "", byp_label);
}

// Downsampled peak-bar waveform + optional moving playhead tick, shared
// by Layer:Status (one layer's own peaks/position) and Global:Speed (a
// composite max across all 4 layers -- see DrawSpeedScreen()). Centered
// in the full gap between the title divider (y9) and the footer divider
// (kFooterDividerY), 1px bar + 1px gap. Auto-scaled to the loudest of the
// 63 cached buckets, so quiet input still uses the full height instead of
// reading as "barely there" -- cheap to do here (63 floats, ~30Hz redraw)
// versus the buffer itself (up to 1.6M raw samples), which is exactly why
// the cache exists in the first place.
void Ui::DrawWaveform(const float* peaks, bool draw_playhead, float playhead_pos01)
{
    float max_peak = 0.f;
    for(int col = 0; col < LooperLayer::kWaveformCols; col++)
        if(peaks[col] > max_peak)
            max_peak = peaks[col];
    float scale = max_peak > 0.001f ? 1.f / max_peak : 0.f;

    const int kBandTop    = 14;
    const int kBandBottom = 44;
    const int center_y    = (kBandTop + kBandBottom) / 2;
    const int half_h      = (kBandBottom - kBandTop) / 2;
    for(int col = 0; col < LooperLayer::kWaveformCols; col++)
    {
        int x = 1 + col * 2;
        int h = (int)(Clampf(peaks[col] * scale, 0.f, 1.f) * half_h + 0.5f);
        if(h <= 0)
            disp_->DrawPixel(x, center_y, true);
        else
            disp_->DrawLine(x, center_y - h, x, center_y + h, true);
    }
    if(draw_playhead)
    {
        int col = (int)(playhead_pos01 * LooperLayer::kWaveformCols);
        if(col >= LooperLayer::kWaveformCols)
            col = LooperLayer::kWaveformCols - 1;
        int px = 1 + col * 2;
        disp_->DrawLine(px, 10, px, 11, true);
    }
}

// Same downsampled peak-bar technique as DrawWaveform() above, but with
// TWO live markers (the Grain layer's fixed anchor, the Scan layer's
// sweeping one) and its own vertical geometry -- this page also shows a
// live Size/Fill/Gap/Scan text readout above the waveform, which
// DrawWaveform()'s other callers don't need, so the band starts lower
// (kBandTop=24 vs 14) to leave room for it.
void Ui::DrawGranularWaveform(const float* peaks, float grain_anchor01, float scan_anchor01,
                                bool has_source)
{
    float max_peak = 0.f;
    for(int col = 0; col < GranularEngine::kWaveformCols; col++)
        if(peaks[col] > max_peak)
            max_peak = peaks[col];
    float scale = max_peak > 0.001f ? 1.f / max_peak : 0.f;

    // kBandTop pushed down to 27 (vs DrawWaveform()'s 14) -- this page's
    // header is now two Tom Thumb rows (label + centered value) instead
    // of one, ending around y21, so this needs the extra clearance.
    const int kBandTop    = 27;
    const int kBandBottom = 44;
    const int center_y    = (kBandTop + kBandBottom) / 2;
    const int half_h      = (kBandBottom - kBandTop) / 2;
    for(int col = 0; col < GranularEngine::kWaveformCols; col++)
    {
        int x = 1 + col * 2;
        int h = (int)(Clampf(peaks[col] * scale, 0.f, 1.f) * half_h + 0.5f);
        if(h <= 0)
            disp_->DrawPixel(x, center_y, true);
        else
            disp_->DrawLine(x, center_y - h, x, center_y + h, true);
    }
    if(has_source)
    {
        // Scan's marker sits one row above Grain's -- kept visually
        // distinct even when both anchors land on the same column
        // (e.g. right after NoteOn, before Scan has swept away).
        int col_s = (int)(Clampf(scan_anchor01, 0.f, 1.f) * GranularEngine::kWaveformCols);
        if(col_s >= GranularEngine::kWaveformCols)
            col_s = GranularEngine::kWaveformCols - 1;
        int px_s = 1 + col_s * 2;
        disp_->DrawLine(px_s, kBandTop - 4, px_s, kBandTop - 3, true);

        int col_g = (int)(Clampf(grain_anchor01, 0.f, 1.f) * GranularEngine::kWaveformCols);
        if(col_g >= GranularEngine::kWaveformCols)
            col_g = GranularEngine::kWaveformCols - 1;
        int px_g = 1 + col_g * 2;
        disp_->DrawLine(px_g, kBandTop - 2, px_g, kBandTop - 1, true);
    }
}

void Ui::DrawLayerScreen()
{
    char title[32];
    const char* page_name = "Status";
    switch(layer_page_)
    {
        case LayerPage::Status: page_name = "Status"; break;
        case LayerPage::Speed: page_name = "Speed"; break;
        case LayerPage::Filter: page_name = "Filter"; break;
        case LayerPage::Effect: page_name = "Effect"; break;
        case LayerPage::Reverb: page_name = "Reverb"; break;
        case LayerPage::Gain: page_name = "Gain"; break;
        default: break;
    }
    // Compact "N:Page" (not "Layer N - Page") specifically to leave room
    // for the beat indicator on the same row -- see DrawBeatIndicator().
    snprintf(title, sizeof(title), "%d:%s", cursor_layer_ + 1, page_name);
    disp_->SetCursor(0, 0);
    WriteUpper(title);
    DrawBeatIndicator(disp_->Width() - 41, 0, 3);
    disp_->DrawLine(0, 9, disp_->Width() - 1, 9, true);

    char line1[32];
    switch(layer_page_)
    {
        case LayerPage::Status:
        {
            LayerState st = Cur().GetState();

            // Long-press-to-clear takes over this whole body area instead
            // of sharing cramped space with the waveform -- it's a
            // destructive action, so it gets the user's full attention
            // while they're deciding whether to keep holding.
            if(pod_->button2.Pressed())
            {
                float held = pod_->button2.TimeHeldMs();
                int   w    = (int)(Clampf(held / 800.f, 0.f, 1.f) * (disp_->Width() - 2));
                const char* msg   = "Hold to clear...";
                int         msg_w = (int)strlen(msg) * 6; // Font_6x8
                disp_->SetCursor((disp_->Width() - msg_w) / 2, 16);
                WriteUpper(msg);
                disp_->DrawRect(0, 28, disp_->Width() - 1, 42, true, false);
                if(w > 0)
                    disp_->DrawRect(1, 29, w, 41, true, true);
            }
            else
            {
                // Downsampled waveform with a moving playhead tick above
                // it (see LooperLayer::GetWaveformPeaks()/GetPlayPos01()),
                // replacing the old meter bar -- and the state heading
                // that used to live here is gone too, since the button
                // row's label below is state-dependent and says it
                // instead. See DrawWaveform() (shared with Global:Speed's
                // composite view) for the actual drawing/auto-scaling.
                bool draw_ph = (st == LayerState::Playing || st == LayerState::Paused
                                || st == LayerState::Overdubbing);
                DrawWaveform(Cur().GetWaveformPeaks(), draw_ph, Cur().GetPlayPos01());
            }

            // Actual values, not the pickup-tracking array -- see the
            // comment on DrawHome()'s equivalent Vol readout.
            char vol_val[8], pan_val[8];
            snprintf(vol_val, sizeof(vol_val), "%d%%", (int)(Cur().GetVolume01() * 100.f + 0.5f));
            snprintf(pan_val, sizeof(pan_val), "%d%%", (int)(Cur().GetPan01() * 100.f + 0.5f));
            DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "Vol", vol_val, pan_val, "Pan");

            // Label reflects what tap/hold actually do right now (see
            // LooperLayer::OnRecordButtonPressed/LongPress) -- split
            // Playing/Paused apart (unlike Home's combined label) so it
            // says which of Pause/Play a tap will do, not just that one
            // of them will happen.
            const char* rec_label = "Rec";
            switch(st)
            {
                case LayerState::Empty: rec_label = "Rec"; break;
                case LayerState::ArmedCountIn: rec_label = "Cancel"; break;
                case LayerState::Recording: rec_label = "Stop"; break;
                case LayerState::Playing: rec_label = "Overdub/Pause"; break;
                case LayerState::Paused: rec_label = "Overdub/Play"; break;
                case LayerState::Overdubbing: rec_label = "Overdubbing"; break;
            }
            DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, rec_label, "", "", "Hold=Clear");
            break;
        }
        case LayerPage::Speed:
        {
            // Live readout instead of a static instruction -- also fixes
            // this line running past the display's ~21-char width at
            // 6px/char on a 128px screen (same class of bug as the
            // Reverb page's "(shared)" getting clipped).
            // %f is used nowhere in this file on purpose: this build
            // links --specs=nano.specs, whose snprintf doesn't support
            // float conversions without extra flash we'd rather not
            // spend on it, so it silently prints nothing for %f. Integer
            // whole/hundredths math instead, same trick used for BPM etc.
            int speed_x100 = (int)(Cur().GetSpeed() * 100.f + 0.5f);
            snprintf(line1, sizeof(line1), "Speed: %d.%02dx", speed_x100 / 100,
                      speed_x100 % 100);
            disp_->SetCursor(0, 20);
            WriteUpper(line1);

            char speed_val[10];
            snprintf(speed_val, sizeof(speed_val), "%d.%02dx", speed_x100 / 100,
                      speed_x100 % 100);
            // Knob2 does nothing on this page -- "" draws nothing rather
            // than a fake value (see DrawControlRow()'s comment).
            DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "Speed", speed_val, "", "");
            DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, "Reset", "", "", "");
            break;
        }
        case LayerPage::Filter:
        {
            snprintf(line1, sizeof(line1), "Mode: %s", FilterModeName(Cur().GetFilterMode()));
            disp_->SetCursor(0, 20);
            WriteUpper(line1);

            // Actual values, not the pickup-tracking array -- see the
            // comment on DrawHome()'s equivalent Vol readout.
            char cutoff_val[8], res_val[8];
            snprintf(cutoff_val, sizeof(cutoff_val), "%d%%",
                      (int)(Cur().GetFilterCutoff01() * 100.f + 0.5f));
            snprintf(res_val, sizeof(res_val), "%d%%",
                      (int)(Cur().GetFilterResonance01() * 100.f + 0.5f));
            DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "Cutoff", cutoff_val, res_val, "Res");
            DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, "Cycle mode", "", "", "");
            break;
        }
        case LayerPage::Effect:
        {
            snprintf(line1, sizeof(line1), "FX: %s", EffectName(Cur().GetEffect()));
            disp_->SetCursor(0, 20);
            WriteUpper(line1);

            // EffectParamA/BLabel() return "-" for a knob this effect
            // doesn't use (e.g. Drive ignores ParamB) -- treat that as
            // "does nothing" too, same as Speed/Gain's idle knob2.
            const char* a_label = EffectParamALabel(Cur().GetEffect());
            const char* b_label = EffectParamBLabel(Cur().GetEffect());
            // Actual values, not the pickup-tracking array -- see the
            // comment on DrawHome()'s equivalent Vol readout.
            char a_val[8] = "", b_val[8] = "";
            if(strcmp(a_label, "-") != 0)
                snprintf(a_val, sizeof(a_val), "%d%%",
                          (int)(Cur().GetEffectParamA01() * 100.f + 0.5f));
            if(strcmp(b_label, "-") != 0)
                snprintf(b_val, sizeof(b_val), "%d%%",
                          (int)(Cur().GetEffectParamB01() * 100.f + 0.5f));
            DrawControlRow(kFooterRow1Y, false, kFooterDividerY,
                             strcmp(a_label, "-") == 0 ? "" : a_label, a_val,
                             b_val, strcmp(b_label, "-") == 0 ? "" : b_label);
            DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, "Cycle FX", "", "", "");
            break;
        }
        case LayerPage::Reverb:
        {
            // Send only -- Size is a shared Global:Reverb setting now
            // (every layer sends into ONE reverb bus, see the shared-bus
            // comment on LooperLayer::SetReverbSend01()), so Knob2 does
            // nothing on this page, same as Speed/Gain's idle knob2.
            snprintf(line1, sizeof(line1), "Send: %d%%",
                      (int)(Cur().GetReverbSend01() * 100.f + 0.5f));
            disp_->SetCursor(0, 20);
            WriteUpper(line1);

            char send_val[8];
            snprintf(send_val, sizeof(send_val), "%d%%",
                      (int)(Cur().GetReverbSend01() * 100.f + 0.5f));
            DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "Send", send_val, "", "");
            DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, "", "", "", "");
            break;
        }
        case LayerPage::Gain:
        {
            // See the Speed page's comment above on why not %f.
            int gain_x10 = (int)(Cur().GetInputGain() * 10.f + 0.5f);
            snprintf(line1, sizeof(line1), "Gain: %d.%dx", gain_x10 / 10, gain_x10 % 10);
            disp_->SetCursor(0, 20);
            WriteUpper(line1);

            char gain_val[8];
            snprintf(gain_val, sizeof(gain_val), "%d.%dx", gain_x10 / 10, gain_x10 % 10);
            DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "Gain", gain_val, "", "");
            DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, "", "", "", "");
            break;
        }
        default: break;
    }
}

void Ui::DrawGlobalScreen()
{
    if(global_page_ == GlobalPage::File)
    {
        DrawFileScreen();
        return;
    }
    if(global_page_ == GlobalPage::SdMgmt)
    {
        DrawSdMgmtScreen();
        return;
    }
    if(global_page_ == GlobalPage::Export)
    {
        DrawExportScreen();
        return;
    }
    if(global_page_ == GlobalPage::Speed)
    {
        DrawSpeedScreen();
        return;
    }
    if(global_page_ == GlobalPage::Granular)
    {
        // Entry point into Screen::Granular, plus the on/off toggle --
        // same treatment as Global:Pad above.
        disp_->SetCursor(0, 0);
        WriteUpper("Global:Grains");
        DrawBeatIndicator(disp_->Width() - 41, 0, 3);
        disp_->DrawLine(0, 9, disp_->Width() - 1, 9, true);
        disp_->SetCursor(0, 20);
        WriteUpper(granular_enabled_ ? "Enabled" : "Disabled");
        disp_->SetCursor(0, 30);
        WriteUpper("Click to open Grains");
        DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "", "", "", "");
        DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, "Toggle On/Off", "", "", "");
        return;
    }
    if(global_page_ == GlobalPage::Looper)
    {
        // Toggle only, no screen to drill into -- see IsLooperEnabled()'s
        // own comment for the pause-vs-full-stop distinction.
        disp_->SetCursor(0, 0);
        WriteUpper("Global:Looper");
        DrawBeatIndicator(disp_->Width() - 41, 0, 3);
        disp_->DrawLine(0, 9, disp_->Width() - 1, 9, true);
        disp_->SetCursor(0, 20);
        WriteUpper(looper_enabled_ ? "Enabled" : "Disabled");
        DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "", "", "", "");
        DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, "Toggle On/Off", "", "", "");
        return;
    }
    if(global_page_ == GlobalPage::Mixer)
    {
        // Entry point only, same treatment as Global:Pad/Granular -- an
        // at-a-glance summary (same 8-channel grid used elsewhere, see
        // DrawMixerOverviewGrid()) plus a hint that clicking drops into
        // the real editing surface, drawn as real content in the body
        // (not disguised as a Button1/Button2 footer label -- this is
        // about the ENCODER, and DrawControlRow's row grammar specifically
        // means "this label describes the button next to it"). No knobs
        // of its own any more.
        disp_->SetCursor(0, 0);
        WriteUpper("Global:Mixer");
        DrawBeatIndicator(disp_->Width() - 41, 0, 3);
        disp_->DrawLine(0, 9, disp_->Width() - 1, 9, true);

        DrawMixerOverviewGrid(15);
        {
            const char* hint = "PUSH ENC TO ENTER";
            int         hw   = TomThumbAdvanceWidth(hint);
            TomThumbDrawText(disp_, (disp_->Width() - hw) / 2, 44, hint, true);
        }

        DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "", "", "", "");
        DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, "", "", "", "");
        return;
    }
    if(global_page_ == GlobalPage::Dexed)
    {
        // Entry point into Screen::Dexed, plus the on/off toggle -- same
        // treatment as Global:Granular above.
        disp_->SetCursor(0, 0);
        WriteUpper("Global:Dexed");
        DrawBeatIndicator(disp_->Width() - 41, 0, 3);
        disp_->DrawLine(0, 9, disp_->Width() - 1, 9, true);
        disp_->SetCursor(0, 20);
        WriteUpper(dexed_enabled_ ? "Enabled" : "Disabled");
        disp_->SetCursor(0, 30);
        WriteUpper("Click to open Dexed");
        DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "", "", "", "");
        DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, "Toggle On/Off", "", "", "");
        return;
    }

    // Compact form (not "Global Settings") for the same reason as the
    // Layer screen's title -- leaves room for the beat indicator on the
    // same row.
    const char* title = "Global:Tempo";
    if(global_page_ == GlobalPage::Filter)
        title = "Global:Filter";
    else if(global_page_ == GlobalPage::Reverb)
        title = "Global:Reverb";
    disp_->SetCursor(0, 0);
    WriteUpper(title);
    DrawBeatIndicator(disp_->Width() - 41, 0, 3);
    disp_->DrawLine(0, 9, disp_->Width() - 1, 9, true);

    char line1[32], line2[32];
    if(global_page_ == GlobalPage::Tempo)
    {
        // Held Button2 takes over this space with a hold-to-confirm
        // progress bar, same pattern as Global:File's Hold=New/Load --
        // see TriggerSaveDefaults()'s comment for why this is a hold,
        // not a tap.
        if(pod_->button2.Pressed())
        {
            float held = pod_->button2.TimeHeldMs();
            int   w    = (int)(Clampf(held / 800.f, 0.f, 1.f) * (disp_->Width() - 2));
            disp_->SetCursor(0, 20);
            WriteUpper("Hold: Default...");
            disp_->DrawRect(0, 30, disp_->Width() - 1, 34, true, false);
            if(w > 0)
                disp_->DrawRect(1, 31, w, 33, true, true);
            DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "", "", "", "");
            DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, "", "", "", "");
            return;
        }

        // Bars right-aligned on the same row as BPM -- it was previously
        // the only thing on this page not shown in the "big" Font_6x8
        // text, only as a small footer readout. "(locked)" dropped from
        // here since the "Clear layers first" line below already says
        // the same thing when it's relevant, and dropping it keeps this
        // row's left side free of a variable-length suffix that would
        // otherwise sometimes collide with the right-aligned Bars text.
        snprintf(line1, sizeof(line1), "BPM: %d", (int)(tempo_->GetBpm() + 0.5f));
        char bars_line[12];
        snprintf(bars_line, sizeof(bars_line), "Bars: %d", tempo_->GetBars());
        snprintf(line2, sizeof(line2), "Metro: %s",
                  tempo_->IsMetronomeEnabled() ? "On" : "Off");
        disp_->SetCursor(0, 12);
        WriteUpper(line1);
        // Font_6x8 is fixed-width (6px/char), so right-aligning is a
        // plain strlen() * 6 -- same trick DrawExportScreen() uses.
        disp_->SetCursor(disp_->Width() - 6 * (int)strlen(bars_line), 12);
        WriteUpper(bars_line);
        disp_->SetCursor(0, 24);
        WriteUpper(line2);
        // Same row either way -- the "save as default" result (if any)
        // is a direct response to something the user just did, so it
        // takes priority over the locked hint on the rare chance both
        // are true at once.
        if(tempo_status_[0] != '\0')
        {
            disp_->SetCursor(0, 36);
            WriteUpper(tempo_status_);
        }
        else if(tempo_->IsLocked())
        {
            disp_->SetCursor(0, 36);
            WriteUpper("Clear layers first");
        }

        char bpm_val[8], bars_val[8];
        snprintf(bpm_val, sizeof(bpm_val), "%d", (int)(tempo_->GetBpm() + 0.5f));
        snprintf(bars_val, sizeof(bars_val), "%d", tempo_->GetBars());
        DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "BPM", bpm_val, bars_val, "Bars");
        DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, "Metro on/off", "", "",
                         "Hold=Default");
    }
    else if(global_page_ == GlobalPage::Filter) // master-bus filter, applied
         // once to the full mix in main.cpp, not per-layer like the Layer
         // Filter page.
    {
        snprintf(line1, sizeof(line1), "Mode: %s", FilterModeName(master_filter_mode_));
        disp_->SetCursor(0, 16);
        WriteUpper(line1);

        // Actual values, not the pickup-tracking array -- see the
        // comment on DrawHome()'s equivalent Vol readout.
        char cutoff_val[8], res_val[8];
        snprintf(cutoff_val, sizeof(cutoff_val), "%d%%",
                  (int)(master_filter_cutoff01_ * 100.f + 0.5f));
        snprintf(res_val, sizeof(res_val), "%d%%", (int)(master_filter_res01_ * 100.f + 0.5f));
        DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "Cutoff", cutoff_val, res_val, "Res");
        DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, "Cycle mode", "", "", "");
    }
    else // GlobalPage::Reverb -- Size/decay for the ONE reverb bus every
         // layer's Send feeds (see LooperLayer::SetReverbSend01()'s
         // comment for why this isn't per-layer any more), plus Bypass's
         // own independent Send into that same bus.
    {
        char size_val[8], byp_send_val[8];
        snprintf(size_val, sizeof(size_val), "%d%%", (int)(reverb_size01_ * 100.f + 0.5f));
        snprintf(byp_send_val, sizeof(byp_send_val), "%d%%",
                  (int)(bypass_reverb_send01_ * 100.f + 0.5f));
        DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "Size", size_val, byp_send_val,
                         "Byp Send");
        DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, "", "", "", "");
    }
}

void Ui::DrawFileScreen()
{
    disp_->SetCursor(0, 0);
    WriteUpper("Global:File");
    DrawBeatIndicator(disp_->Width() - 41, 0, 3);
    disp_->DrawLine(0, 9, disp_->Width() - 1, 9, true);

    if(!PerformanceStore::IsCardPresent())
    {
        // Centered the same way Global:Export's own "No SD card" is --
        // see its own comment for the exact math.
        const char* msg = "No SD card";
        int         x   = (disp_->Width() - 6 * (int)strlen(msg)) / 2;
        disp_->SetCursor(x < 0 ? 0 : x, 24);
        WriteUpper(msg);
        DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "", "", "", "");
        DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, "Retry", "", "", "");
        return;
    }

    if(file_slots_dirty_)
        RefreshFileSlots();

    // Whichever button is actually held wins the space (hold-to-confirm
    // progress, same idea as the Status page's Clear); otherwise this
    // area shows whichever of Idle/ChoosingSave/BrowsingLoad is active
    // (see SaveLoadMode's own comment).
    bool choosing_save = save_load_mode_ == SaveLoadMode::ChoosingSave;
    bool browsing_load = save_load_mode_ == SaveLoadMode::BrowsingLoad;
    // Load's own top-level chooser (Files/New, mirrors ChoosingSave's own
    // Overwrite/Save New) -- see load_new_selected_/load_browsing_files_'s
    // own comment in ui.h.
    bool load_chooser  = browsing_load && !load_browsing_files_;
    bool can_hold_load = browsing_load && (load_browsing_files_ || load_new_selected_);
    // Button1 has no hold action any more -- its tap means "Save" (Idle),
    // "drill into the file list" (Load chooser with Files highlighted),
    // or "Back" (everywhere else revealed, see OnButton1Short()). Button2
    // confirms Save and Load (see HandleButton2()) once there's something
    // specific to confirm.
    if(pod_->button2.Pressed() && (choosing_save || can_hold_load))
    {
        float       held     = pod_->button2.TimeHeldMs();
        int         w        = (int)(Clampf(held / 800.f, 0.f, 1.f) * (disp_->Width() - 2));
        const char* hold_msg = choosing_save ? "Hold: Save..." : "Hold: Load...";
        disp_->SetCursor(0, 20);
        WriteUpper(hold_msg);
        disp_->DrawRect(0, 30, disp_->Width() - 1, 34, true, false);
        if(w > 0)
            disp_->DrawRect(1, 31, w, 33, true, true);
    }
    else if(choosing_save)
    {
        // Overwrite only shown as a real option once something's loaded
        // -- Knob1's own ApplyKnobs() handling already forces Save New
        // when it isn't, this just reflects that on screen too.
        bool can_overwrite = loaded_slot_ >= 0;
        char line1[24], line2[16];
        snprintf(line1, sizeof(line1), "%c Overwrite%s", !save_as_new_ ? '>' : ' ',
                  can_overwrite ? "" : " (n/a)");
        snprintf(line2, sizeof(line2), "%c Save New", save_as_new_ ? '>' : ' ');
        disp_->SetCursor(0, 16);
        WriteUpper(line1);
        disp_->SetCursor(0, 28);
        WriteUpper(line2);
    }
    else if(load_chooser)
    {
        // Same shape as ChoosingSave's own Overwrite/Save New -- Files
        // only a real option once something's actually saved, same
        // "can't select what doesn't exist" reasoning as Overwrite's own.
        char line1[24], line2[16];
        snprintf(line1, sizeof(line1), "%c Files%s", !load_new_selected_ ? '>' : ' ',
                  file_slot_count_ > 0 ? "" : " (n/a)");
        snprintf(line2, sizeof(line2), "%c Load New", load_new_selected_ ? '>' : ' ');
        disp_->SetCursor(0, 16);
        WriteUpper(line1);
        disp_->SetCursor(0, 28);
        WriteUpper(line2);
    }
    else if(browsing_load)
    {
        // Drilled into the numbered list (load_browsing_files_ == true).
        char line2[32];
        snprintf(line2, sizeof(line2), "Load: %d - Perf", file_slots_[file_cursor_]);
        disp_->SetCursor(0, 20);
        WriteUpper(line2);
    }
    else
    {
        char line1[32];
        if(loaded_slot_ >= 0)
            snprintf(line1, sizeof(line1), "Now: %d - Perf", loaded_slot_);
        else
            snprintf(line1, sizeof(line1), "Now: (unsaved)");
        disp_->SetCursor(0, 20);
        WriteUpper(line1);

        if(file_status_[0] != '\0')
        {
            disp_->SetCursor(0, 32);
            WriteUpper(file_status_);
        }
    }

    const char* b1_label;
    if(save_load_mode_ == SaveLoadMode::Idle)
        b1_label = "Save";
    else if(load_chooser && !load_new_selected_)
        b1_label = "Select"; // drills into the numbered list
    else
        b1_label = "Back";
    const char* b2_label;
    if(save_load_mode_ == SaveLoadMode::Idle)
        b2_label = "Load";
    else if(choosing_save)
        b2_label = "Hold=Save";
    else if(can_hold_load)
        b2_label = "Hold=Load";
    else
        b2_label = ""; // Load chooser with Files highlighted -- nothing to confirm yet

    DrawControlRow(kFooterRow1Y, false, kFooterDividerY,
                     (choosing_save || browsing_load) ? "Scroll" : "", "", "", "");
    DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, b1_label, "", "", b2_label);
}

const char* Ui::SdMgmtFolderName(SdMgmtFolder f)
{
    switch(f)
    {
        case SdMgmtFolder::Performances: return "Performances";
        case SdMgmtFolder::GranularPresets: return "Grains Presets";
        default: return "?";
    }
}

void Ui::DrawSdMgmtScreen()
{
    disp_->SetCursor(0, 0);
    WriteUpper("Global:SD Mgmt");
    DrawBeatIndicator(disp_->Width() - 41, 0, 3);
    disp_->DrawLine(0, 9, disp_->Width() - 1, 9, true);

    if(!PerformanceStore::IsCardPresent())
    {
        // Centered the same way Global:File/Export's own "No SD card" is.
        const char* msg = "No SD card";
        int         x   = (disp_->Width() - 6 * (int)strlen(msg)) / 2;
        disp_->SetCursor(x < 0 ? 0 : x, 24);
        WriteUpper(msg);
        DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "", "", "", "");
        DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, "Retry", "", "", "");
        return;
    }

    // Whichever button is actually held wins the space, same hold-to-
    // confirm-progress idiom as every other destructive/blocking action.
    if(pod_->button1.Pressed() && sd_mgmt_in_folder_)
    {
        float held = pod_->button1.TimeHeldMs();
        int   w    = (int)(Clampf(held / 400.f, 0.f, 1.f) * (disp_->Width() - 2));
        disp_->SetCursor(0, 20);
        WriteUpper("Hold: Duplicate...");
        disp_->DrawRect(0, 30, disp_->Width() - 1, 34, true, false);
        if(w > 0)
            disp_->DrawRect(1, 31, w, 33, true, true);
        DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "", "", "", "");
        DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, "", "", "", "");
        return;
    }
    if(pod_->button2.Pressed() && sd_mgmt_in_folder_)
    {
        float held = pod_->button2.TimeHeldMs();
        int   w    = (int)(Clampf(held / kSdMgmtDeleteHoldMs, 0.f, 1.f) * (disp_->Width() - 2));
        disp_->SetCursor(0, 20);
        WriteUpper("Hold: Delete...");
        disp_->DrawRect(0, 30, disp_->Width() - 1, 34, true, false);
        if(w > 0)
            disp_->DrawRect(1, 31, w, 33, true, true);
        DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "", "", "", "");
        DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, "", "", "", "");
        return;
    }

    if(!sd_mgmt_in_folder_)
    {
        // Same ">"-highlighted-list convention as ChoosingSave's own
        // Overwrite/Save New options.
        for(int i = 0; i < (int)SdMgmtFolder::kCount; i++)
        {
            char line[20];
            snprintf(line, sizeof(line), "%c %s", i == (int)sd_mgmt_folder_ ? '>' : ' ',
                      SdMgmtFolderName((SdMgmtFolder)i));
            disp_->SetCursor(0, 14 + i * 10);
            WriteUpper(line);
        }
    }
    else
    {
        char line1[24];
        snprintf(line1, sizeof(line1), "%s:", SdMgmtFolderName(sd_mgmt_folder_));
        disp_->SetCursor(0, 14);
        WriteUpper(line1);

        if(sd_mgmt_status_[0] != '\0')
        {
            disp_->SetCursor(0, 26);
            WriteUpper(sd_mgmt_status_);
        }
        else
        {
            int        count = 0;
            const int* slots = SdMgmtSlots(&count);
            char       line2[16];
            if(slots && count > 0)
                snprintf(line2, sizeof(line2), "%d", slots[sd_mgmt_cursor_]);
            else
                snprintf(line2, sizeof(line2), "(empty)");
            disp_->SetCursor(0, 26);
            WriteUpper(line2);
        }
    }

    DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "Scroll", "", "", "");
    DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY,
                     sd_mgmt_in_folder_ ? "Back/Hold=Dup" : "Select", "", "",
                     sd_mgmt_in_folder_ ? "Hold=Delete" : "");
}

void Ui::DrawExportScreen()
{
    disp_->SetCursor(0, 0);
    WriteUpper("Global:Export");
    DrawBeatIndicator(disp_->Width() - 41, 0, 3);
    disp_->DrawLine(0, 9, disp_->Width() - 1, 9, true);

    // One line, centered in the space between the title divider (y9) and
    // the footer divider (kFooterDividerY=46) -- 36 rows (10..45) for an
    // 8px-tall Font_6x8 line, so y=24 splits the remaining 28px evenly.
    // Font_6x8 is fixed-width (6px/char), so horizontal centering is a
    // plain strlen() * 6.
    const char* msg = PerformanceStore::IsCardPresent()
                           ? (export_status_[0] != '\0' ? export_status_ : "Export WAV File")
                           : "No SD card";
    int x = (disp_->Width() - 6 * (int)strlen(msg)) / 2;
    disp_->SetCursor(x < 0 ? 0 : x, 24);
    WriteUpper(msg);

    if(!PerformanceStore::IsCardPresent())
    {
        DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "", "", "", "");
        DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, "Retry", "", "", "");
        return;
    }

    // No hold-to-confirm here (unlike File's New/Load) -- Export always
    // creates a new numbered file and never overwrites/destroys anything,
    // so a plain tap is safe.
    DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "", "", "", "");
    DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, "STUDIO 48K", "", "", "CD 44.1K");
}

void Ui::DrawSpeedScreen()
{
    disp_->SetCursor(0, 0);
    WriteUpper("Global:Speed");
    DrawBeatIndicator(disp_->Width() - 41, 0, 3);
    disp_->DrawLine(0, 9, disp_->Width() - 1, 9, true);

    // Composite waveform (max across all 4 layers' own caches, not just
    // the cursor layer) -- matches vari-speed/scrub's whole premise that
    // everything on the shared loop moves together, one tape. Playhead
    // comes from the lowest-indexed non-empty layer's own position --
    // there's no separate tracked "shared position" (each layer's
    // play_pos_ is independent state, kept roughly in sync only by
    // sharing record_len_ and starting at 0), and scrub nudges already
    // write directly into that same layer's play_pos_ (see ScrubBy()), so
    // this reflects live scrub movement automatically with no special
    // casing here.
    float composite_peaks[LooperLayer::kWaveformCols];
    for(int col = 0; col < LooperLayer::kWaveformCols; col++)
    {
        float m = 0.f;
        for(int L = 0; L < num_layers_; L++)
        {
            const float* p = layers_[L].GetWaveformPeaks();
            if(p[col] > m)
                m = p[col];
        }
        composite_peaks[col] = m;
    }
    int rep = -1;
    for(int L = 0; L < num_layers_; L++)
    {
        if(layers_[L].HasContent())
        {
            rep = L;
            break;
        }
    }
    DrawWaveform(composite_peaks, rep >= 0, rep >= 0 ? layers_[rep].GetPlayPos01() : 0.f);

    char speed_val[10];
    int  speed_x100 = (int)(project_speed_ * 100.f + 0.5f);
    snprintf(speed_val, sizeof(speed_val), "%d.%02dx", speed_x100 / 100, speed_x100 % 100);
    DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "Speed", speed_val, "", "");
    DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY,
                     scrub_mode_active_ ? "Scrub:On" : "Scrub:Off", "", "", "Reset");
}

void Ui::DrawGranularScreen()
{
    if(!granular_)
        return;

    const char* page_name = "Grain";
    switch(granular_param_page_)
    {
        case GranularParamPage::Grain: page_name = "Grain"; break;
        case GranularParamPage::Position: page_name = "POS+RHY"; break;
        case GranularParamPage::TuneDirection: page_name = "Tune"; break;
        case GranularParamPage::ADSR: page_name = "ADSR"; break;
        case GranularParamPage::Filter: page_name = "Filter"; break;
        case GranularParamPage::Mix: page_name = "Mix"; break;
        case GranularParamPage::Capture: page_name = "Capture"; break;
        case GranularParamPage::Trim: page_name = "Trim"; break;
        case GranularParamPage::Preset: page_name = "Preset"; break;
        default: break;
    }
    // Shared content band for the simpler pages below (ADSR's graph,
    // Filter's oscilloscope) -- same values as Pad's own kBandTop/
    // kBandBottom. Grain's own page uses a deeper band (see
    // DrawGranularWaveform()) since it also has the two-row Size/Fill/
    // Gap/Scan header above it that these pages don't.
    const int kBandTop = 22, kBandBottom = 44;
    char title[24];
    snprintf(title, sizeof(title), "Grains:%s", page_name);
    disp_->SetCursor(0, 0);
    WriteUpper(title);
    DrawBeatIndicator(disp_->Width() - 41, 0, 3);
    disp_->DrawLine(0, 9, disp_->Width() - 1, 9, true);

    switch(granular_param_page_)
    {
        case GranularParamPage::Grain:
        {
            // Live values for all four params, always shown regardless of
            // which pair the knobs currently target -- same "readout
            // shows everything, buttons only change what the knobs
            // reach" idiom as Pad's own merged ADSR page. Tom Thumb (not
            // the big Font_6x8) so all four fit as a proper label-over-
            // value layout instead of one cramped abbreviated line; four
            // equal-width columns so they land evenly across the display,
            // and each value is centered on its own label's center (via
            // TomThumbAdvanceWidth()), not just left-aligned under it, so
            // differently-wide values (e.g. "3" vs "100%") still read as
            // belonging to the word above them.
            const char* grain_labels[4] = {"SIZE", "FILL", "GAP", "SCAN"};
            char        grain_values[4][8];
            snprintf(grain_values[0], sizeof(grain_values[0]), "%d%%",
                      (int)(granular_->GetSize01() * 100.f + 0.5f));
            snprintf(grain_values[1], sizeof(grain_values[1]), "%d", granular_->GetFill());
            snprintf(grain_values[2], sizeof(grain_values[2]), "%d%%",
                      (int)(granular_->GetGap01() * 100.f + 0.5f));
            snprintf(grain_values[3], sizeof(grain_values[3]), "%d%%",
                      (int)(granular_->GetScan01() * 100.f + 0.5f));
            const int kGrainLabelBaseline = 15;
            const int kGrainValueBaseline = 21;
            const int kGrainColWidth      = disp_->Width() / 4;
            for(int i = 0; i < 4; i++)
            {
                int center_x = kGrainColWidth * i + kGrainColWidth / 2;
                int lw       = TomThumbAdvanceWidth(grain_labels[i]);
                TomThumbDrawText(disp_, center_x - lw / 2, kGrainLabelBaseline, grain_labels[i],
                                  true);
                int vw = TomThumbAdvanceWidth(grain_values[i]);
                TomThumbDrawText(disp_, center_x - vw / 2, kGrainValueBaseline, grain_values[i],
                                  true);
            }

            DrawGranularWaveform(granular_->GetWaveformPeaks(), granular_->GetGrainAnchor01(),
                                  granular_->GetScanAnchor01(), granular_->HasSource());

            if(!granular_grain_target_gap_scan_)
            {
                char sz_val[8], fl_val[8];
                snprintf(sz_val, sizeof(sz_val), "%d%%",
                          (int)(granular_->GetSize01() * 100.f + 0.5f));
                snprintf(fl_val, sizeof(fl_val), "%d", granular_->GetFill());
                DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "Size", sz_val, fl_val,
                                 "Fill");
            }
            else
            {
                char gp_val[8], sc_val[8];
                snprintf(gp_val, sizeof(gp_val), "%d%%",
                          (int)(granular_->GetGap01() * 100.f + 0.5f));
                snprintf(sc_val, sizeof(sc_val), "%d%%",
                          (int)(granular_->GetScan01() * 100.f + 0.5f));
                DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "Gap", gp_val, sc_val,
                                 "Scan");
            }
            // Marks which pair Button1/Button2 currently map the knobs
            // to, same "*" convention as Pad's own ADSR page.
            DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY,
                             granular_grain_target_gap_scan_ ? "SzFl" : "SzFl*", "", "",
                             granular_grain_target_gap_scan_ ? "GpSc*" : "GpSc");
            break;
        }
        case GranularParamPage::Position:
        {
            // Same full-sample waveform + live anchor markers as the
            // Grain page (just DrawGranularWaveform() again -- it already
            // draws both the Grain layer's anchor and the Scan layer's),
            // so turning either knob visibly moves its own marker across
            // the sample instead of only being a number.
            //
            // Rhythm (Button1) and Speed (Button2) both share this page
            // as button-cycled modes -- same "top text shows the
            // button-controlled mode" treatment as Filter's "Mode: X"
            // and Capture's "Source: X", just two of them since there
            // are two buttons doing mode-cycling duty here now.
            char line1[24];
            snprintf(line1, sizeof(line1), "Rhy:%s  Spd:%s", granular_->GetRhythmName(),
                      granular_->GetGrainSpeedName());
            TomThumbDrawText(disp_, 0, 15, line1, true);

            DrawGranularWaveform(granular_->GetWaveformPeaks(), granular_->GetGrainAnchor01(),
                                  granular_->GetScanAnchor01(), granular_->HasSource());

            char pos_val[8], scan_val[8];
            snprintf(pos_val, sizeof(pos_val), "%d%%",
                      (int)(granular_->GetPosition01() * 100.f + 0.5f));
            if(granular_->IsScanMuted())
                snprintf(scan_val, sizeof(scan_val), "Mute");
            else
                snprintf(scan_val, sizeof(scan_val), "%d%%",
                          (int)(granular_->GetScanPosition01() * 100.f + 0.5f));
            DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "Position", pos_val, scan_val,
                             "ScanPos");
            // Button hints, same "left label = Button1, right label =
            // Button2" idiom as DexedOperator's own "Op"/"AD/SR" row.
            DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, "Rhythm", "", "",
                             "Speed");
            break;
        }
        case GranularParamPage::TuneDirection:
        {
            int semis = granular_->GetGrainTuneSemitones();
            const char* dir_name = GranularDirectionName(granular_->GetDirection());
            char line1[24];
            snprintf(line1, sizeof(line1), "Tune:%+d  Dir:%s", semis, dir_name);
            TomThumbDrawText(disp_, 0, 15, line1, true);
            TomThumbDrawText(disp_, 0, 22,
                               granular_->GetGrainFollowsNote() ? "Map to note: On"
                                                                  : "Map to note: Off",
                               true);

            // Same waveform + anchor markers as Grain/Position -- this
            // page's own two knobs (Tune, Direction) don't move an
            // anchor, but showing it anyway keeps every Grains page from
            // looking empty and still reflects Direction's effect
            // (reversed grains) once that's audible.
            DrawGranularWaveform(granular_->GetWaveformPeaks(), granular_->GetGrainAnchor01(),
                                  granular_->GetScanAnchor01(), granular_->HasSource());

            char tune_val[8];
            snprintf(tune_val, sizeof(tune_val), "%+d", semis);
            DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "Tune", tune_val, dir_name,
                             "Dir");
            DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY,
                             granular_->GetGrainFollowsNote() ? "Map:On" : "Map:Off", "", "", "");
            break;
        }
        case GranularParamPage::ADSR:
        {
            // All four stages always shown together, same idiom as Pad's
            // own merged ADSR page and Grain's Size/Fill/Gap/Scan readout.
            DrawAdsrShape(kBandTop, kBandBottom, granular_->GetAttackSeconds(),
                          granular_->GetDecaySeconds(), granular_->GetSustain01(),
                          granular_->GetReleaseSeconds());
            if(!granular_adsr_target_sr_)
            {
                char a_val[8], d_val[8];
                snprintf(a_val, sizeof(a_val), "%d%%",
                          (int)(granular_->GetAttack01() * 100.f + 0.5f));
                snprintf(d_val, sizeof(d_val), "%d%%",
                          (int)(granular_->GetDecay01() * 100.f + 0.5f));
                DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "Attack", a_val, d_val,
                                 "Decay");
            }
            else
            {
                char s_val[8], r_val[8];
                snprintf(s_val, sizeof(s_val), "%d%%",
                          (int)(granular_->GetSustain01() * 100.f + 0.5f));
                snprintf(r_val, sizeof(r_val), "%d%%",
                          (int)(granular_->GetRelease01() * 100.f + 0.5f));
                DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "Sustain", s_val, r_val,
                                 "Release");
            }
            DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY,
                             granular_adsr_target_sr_ ? "AD" : "AD*", "", "",
                             granular_adsr_target_sr_ ? "SR*" : "SR");
            break;
        }
        case GranularParamPage::Filter:
        {
            // Mode name in Tom Thumb at the top (was the big font, eating
            // into the oscilloscope's own space) -- frees up real room
            // for the scope, which now gets almost the whole band instead
            // of losing 9px to a tall text line.
            char mode_line[20];
            snprintf(mode_line, sizeof(mode_line), "Mode: %s",
                      FilterModeName(granular_->GetFilterMode()));
            TomThumbDrawText(disp_, 0, 15, mode_line, true);
            DrawOscilloscope(18, kBandBottom, granular_scope_buf_, granular_scope_capacity_);

            char cutoff_val[8], res_val[8];
            snprintf(cutoff_val, sizeof(cutoff_val), "%d%%",
                      (int)(granular_->GetFilterCutoff01() * 100.f + 0.5f));
            snprintf(res_val, sizeof(res_val), "%d%%",
                      (int)(granular_->GetFilterResonance01() * 100.f + 0.5f));
            DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "Cutoff", cutoff_val, res_val,
                             "Res");
            DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, "Cycle mode", "", "",
                             "");
            break;
        }
        case GranularParamPage::Mix:
        {
            // Same label-over-value column layout as the Grain page's
            // Size/Fill/Gap/Scan readout, plus a level bar under each
            // value -- same outline+fill technique as this project's own
            // hold-to-confirm progress bars. All three always shown
            // (same "graph shows everything, buttons only change what
            // the knobs reach" idiom as Pad's own ADSR page) -- Reverb
            // Send here is the SAME granular_->GetReverbSend01() also
            // reachable from Global:Mixer, so changing it here updates
            // there too with no extra sync needed.
            const char* mix_labels[3] = {"Grain", "Scan", "Reverb"};
            float       mix_vals01[3] = {granular_->GetGrainVolume01(),
                                          granular_->GetScanVolume01(),
                                          granular_->GetReverbSend01()};
            char        mix_values[3][8];
            for(int i = 0; i < 3; i++)
                snprintf(mix_values[i], sizeof(mix_values[i]), "%d%%",
                          (int)(mix_vals01[i] * 100.f + 0.5f));

            const int kMixLabelBaseline = 15;
            const int kMixValueBaseline = 21;
            const int kMixColWidth      = disp_->Width() / 3;
            const int kBarWidth = 34, kBarHeight = 6, kBarTop = 25;
            for(int i = 0; i < 3; i++)
            {
                int center_x = kMixColWidth * i + kMixColWidth / 2;
                int lw       = TomThumbAdvanceWidth(mix_labels[i]);
                TomThumbDrawText(disp_, center_x - lw / 2, kMixLabelBaseline, mix_labels[i],
                                  true);
                int vw = TomThumbAdvanceWidth(mix_values[i]);
                TomThumbDrawText(disp_, center_x - vw / 2, kMixValueBaseline, mix_values[i],
                                  true);

                int bar_x0 = center_x - kBarWidth / 2;
                int bar_x1 = bar_x0 + kBarWidth - 1;
                disp_->DrawRect(bar_x0, kBarTop, bar_x1, kBarTop + kBarHeight - 1, true, false);
                int fill_w
                    = (int)(Clampf(mix_vals01[i], 0.f, 1.f) * (float)(kBarWidth - 2) + 0.5f);
                if(fill_w > 0)
                    disp_->DrawRect(bar_x0 + 1, kBarTop + 1, bar_x0 + fill_w,
                                      kBarTop + kBarHeight - 2, true, true);
            }

            if(!granular_mix_target_reverb_)
                DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "Grain", mix_values[0],
                                 mix_values[1], "Scan");
            else
                DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "Reverb", mix_values[2],
                                 "", "");
            DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY,
                             granular_mix_target_reverb_ ? "Grain/Scan" : "Grain/Scan*", "", "",
                             granular_mix_target_reverb_ ? "Reverb*" : "Reverb");
            break;
        }
        case GranularParamPage::Capture:
        {
            // Hold-to-confirm progress bar for the two fire-once gestures
            // (From Layer, Import) -- same pattern as Preset's own
            // "Hold: Load..." bar. Direct Record is deliberately excluded:
            // holding Button2 there IS the recording itself (see the
            // "Recording %" bar further down), not a confirm-then-fire
            // action.
            if(pod_->button2.Pressed()
               && (granular_capture_source_ >= 0 || granular_capture_source_ == -2))
            {
                float held = pod_->button2.TimeHeldMs();
                int   w    = (int)(Clampf(held / 800.f, 0.f, 1.f) * (disp_->Width() - 2));
                const char* hold_msg
                    = granular_capture_source_ == -2 ? "Hold: Import..." : "Hold: Capture...";
                TomThumbDrawText(disp_, 0, 15, hold_msg, true);
                disp_->DrawRect(0, 20, disp_->Width() - 1, 24, true, false);
                if(w > 0)
                    disp_->DrawRect(1, 21, w, 23, true, true);
                DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "", "", "", "");
                DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, "", "", "", "");
                break;
            }

            char src_line[24];
            if(granular_capture_source_ >= 0)
                snprintf(src_line, sizeof(src_line), "Source: Layer %d",
                          granular_capture_source_ + 1);
            else if(granular_capture_source_ == -2)
                snprintf(src_line, sizeof(src_line), "Source: Import");
            else
                snprintf(src_line, sizeof(src_line), "Source: Direct Record");
            TomThumbDrawText(disp_, 0, 15, src_line, true);

            if(granular_capture_source_ == -2 && granular_import_files_dirty_)
                RefreshGranularImportFiles();

            bool recording = granular_capture_source_ == -1 && granular_capturing_
                              && *granular_capturing_;
            if(granular_capture_source_ == -2)
            {
                // Browse the list of .wav files found in IMPORT/ --
                // same "Load: <name>"-style live feedback as Preset's
                // own browsing, status takes priority when there is one.
                if(granular_capture_status_[0] != '\0')
                {
                    TomThumbDrawText(disp_, 0, 22, granular_capture_status_, true);
                }
                else if(granular_import_file_count_ > 0)
                {
                    // Scrollable list, not just the single selected name --
                    // up to kVisibleRows entries at once (baselines 22, 28,
                    // 34, 40 -- 6px TomThumb rows, fits between the "Source:"
                    // line above and the footer divider below), with the
                    // window centered on the cursor and clamped to the
                    // list's own ends so it doesn't scroll past them.
                    constexpr int kVisibleRows = 4;
                    int           count        = granular_import_file_count_;
                    int           start        = granular_import_cursor_ - kVisibleRows / 2;
                    if(start > count - kVisibleRows)
                        start = count - kVisibleRows;
                    if(start < 0)
                        start = 0;
                    int rows = count < kVisibleRows ? count : kVisibleRows;
                    for(int row = 0; row < rows; row++)
                    {
                        int  idx      = start + row;
                        bool selected = idx == granular_import_cursor_;
                        char line[64];
                        snprintf(line, sizeof(line), "%c%s", selected ? '>' : ' ',
                                  granular_import_names_[idx]);
                        // Truncate from the end until it fits -- a long
                        // sample filename (e.g. exported from a DAW)
                        // otherwise runs straight past the 128px display
                        // width.
                        while(line[0] != '\0' && TomThumbAdvanceWidth(line) > disp_->Width())
                            line[strlen(line) - 1] = '\0';
                        TomThumbDrawText(disp_, 0, 22 + row * 6, line, true);
                    }
                }
                else
                {
                    TomThumbDrawText(disp_, 0, 22, "No .wav files in IMPORT/", true);
                }
            }
            else if(recording)
            {
                // Holding Button2 IS the recording -- this bar is both
                // progress feedback and the "how much of the 5s cap is
                // left" readout, live-updating from the ISR's own
                // write_pos as it fills.
                float frac = granular_capture_capacity_ > 0
                                 ? (float)*granular_capture_write_pos_
                                       / (float)granular_capture_capacity_
                                 : 0.f;
                char rec_line[20];
                snprintf(rec_line, sizeof(rec_line), "Recording %d%%",
                          (int)(frac * 100.f + 0.5f));
                TomThumbDrawText(disp_, 0, 22, rec_line, true);

                const int kBarX0 = 0, kBarX1 = disp_->Width() - 1, kBarY0 = 28, kBarY1 = 36;
                disp_->DrawRect(kBarX0, kBarY0, kBarX1, kBarY1, true, false);
                int fill_w
                    = (int)(Clampf(frac, 0.f, 1.f) * (float)(kBarX1 - kBarX0 - 1) + 0.5f);
                if(fill_w > 0)
                    disp_->DrawRect(kBarX0 + 1, kBarY0 + 1, kBarX0 + fill_w, kBarY1 - 1, true,
                                      true);
            }
            else
            {
                if(granular_capture_status_[0] != '\0')
                    TomThumbDrawText(disp_, 0, 22, granular_capture_status_, true);
                DrawGranularWaveform(granular_->GetWaveformPeaks(), granular_->GetGrainAnchor01(),
                                      granular_->GetScanAnchor01(), granular_->HasSource());
            }

            const char* hold_label = "Hold=Record";
            if(granular_capture_source_ >= 0)
                hold_label = "Hold=Capture";
            else if(granular_capture_source_ == -2)
                hold_label = "Hold=Import";
            DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "", "", "", "");
            DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, "Cycle Source", "", "",
                             hold_label);
            break;
        }
        case GranularParamPage::Trim:
        {
            char line[24];
            snprintf(line, sizeof(line), "Start%d%% End%d%%",
                      (int)(granular_trim_start01_ * 100.f + 0.5f),
                      (int)(granular_trim_end01_ * 100.f + 0.5f));
            TomThumbDrawText(disp_, 0, 15, line, true);

            // Reuses DrawGranularWaveform()'s two-marker drawing as-is --
            // here the markers are the trim start/end points rather than
            // Grain/Scan anchors, over the FULL untrimmed capture (see
            // granular_trim_full_peaks_'s own comment), not whatever
            // sub-range is currently active.
            DrawGranularWaveform(granular_trim_full_peaks_, granular_trim_start01_,
                                  granular_trim_end01_, granular_capture_full_len_ > 0);

            char s_val[8], e_val[8];
            snprintf(s_val, sizeof(s_val), "%d%%", (int)(granular_trim_start01_ * 100.f + 0.5f));
            snprintf(e_val, sizeof(e_val), "%d%%", (int)(granular_trim_end01_ * 100.f + 0.5f));
            DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "Start", s_val, e_val, "End");
            DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, "", "", "", "");
            break;
        }
        case GranularParamPage::Preset:
        {
            if(!PerformanceStore::IsCardPresent())
            {
                disp_->SetCursor(0, 20);
                WriteUpper("No card");
                DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "", "", "", "");
                DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, "Retry", "", "", "");
                break;
            }
            if(granular_preset_slots_dirty_)
                RefreshGranularPresetSlots();

            bool choosing_save = save_load_mode_ == SaveLoadMode::ChoosingSave;
            bool browsing_load = save_load_mode_ == SaveLoadMode::BrowsingLoad;
            bool load_chooser  = browsing_load && !load_browsing_files_;
            bool can_hold_load = browsing_load && (load_browsing_files_ || load_new_selected_);

            // Body text uses the same bigger Font_6x8/WriteUpper
            // convention as Global:File (not TomThumb, unlike the rest of
            // this screen's pages) -- same reasoning: this is the one
            // page where matching Global:File's exact look matters more
            // than matching Grains' own other pages.
            //
            // Button2 confirms whichever of Save/Load is active (Button1
            // is always "Back" (or "Select" on the Load chooser) once
            // inside either state -- see OnButton1Short()/
            // HandleButton2()). The actual SD transfer (once the hold
            // fires) takes over the WHOLE display via
            // Ui::OnSaveLoadProgress() instead, since it can run long
            // enough (up to ~1.9MB of audio) to need its own feedback,
            // unlike Pad's instant tiny-struct load.
            if(pod_->button2.Pressed() && (choosing_save || can_hold_load))
            {
                float       held     = pod_->button2.TimeHeldMs();
                int         w        = (int)(Clampf(held / 800.f, 0.f, 1.f) * (disp_->Width() - 2));
                const char* hold_msg = choosing_save ? "Hold: Save..." : "Hold: Load...";
                disp_->SetCursor(0, 20);
                WriteUpper(hold_msg);
                disp_->DrawRect(0, 30, disp_->Width() - 1, 34, true, false);
                if(w > 0)
                    disp_->DrawRect(1, 31, w, 33, true, true);
                DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "", "", "", "");
                DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, "", "", "", "");
                break;
            }

            if(choosing_save)
            {
                bool can_overwrite = granular_loaded_preset_slot_ > 0;
                char line1[24], line2[16];
                snprintf(line1, sizeof(line1), "%c Overwrite%s", !save_as_new_ ? '>' : ' ',
                          can_overwrite ? "" : " (n/a)");
                snprintf(line2, sizeof(line2), "%c Save New", save_as_new_ ? '>' : ' ');
                disp_->SetCursor(0, 16);
                WriteUpper(line1);
                disp_->SetCursor(0, 28);
                WriteUpper(line2);
            }
            else if(load_chooser)
            {
                // Same shape as ChoosingSave's own Overwrite/Save New --
                // Files only a real option once something's actually
                // saved, same "can't select what doesn't exist" reasoning
                // as Overwrite's own.
                char line1[24], line2[16];
                snprintf(line1, sizeof(line1), "%c Files%s", !load_new_selected_ ? '>' : ' ',
                          granular_preset_user_slot_count_ > 0 ? "" : " (n/a)");
                snprintf(line2, sizeof(line2), "%c Load New", load_new_selected_ ? '>' : ' ');
                disp_->SetCursor(0, 16);
                WriteUpper(line1);
                disp_->SetCursor(0, 28);
                WriteUpper(line2);
            }
            else if(browsing_load)
            {
                // Drilled into the numbered list (load_browsing_files_ ==
                // true).
                char line2[24];
                snprintf(line2, sizeof(line2), "Load: %d",
                          granular_preset_user_slots_[granular_preset_cursor_]);
                disp_->SetCursor(0, 20);
                WriteUpper(line2);
            }
            else
            {
                char line1[24];
                if(granular_loaded_preset_slot_ <= 0)
                    snprintf(line1, sizeof(line1), "Now: (custom)");
                else
                    snprintf(line1, sizeof(line1), "Now: %d", granular_loaded_preset_slot_);
                disp_->SetCursor(0, 20);
                WriteUpper(line1);
                if(granular_preset_status_[0] != '\0')
                {
                    disp_->SetCursor(0, 32);
                    WriteUpper(granular_preset_status_);
                }
            }

            {
                const char* b1_label;
                if(save_load_mode_ == SaveLoadMode::Idle)
                    b1_label = "Save";
                else if(load_chooser && !load_new_selected_)
                    b1_label = "Select";
                else
                    b1_label = "Back";
                const char* b2_label;
                if(save_load_mode_ == SaveLoadMode::Idle)
                    b2_label = "Load";
                else if(choosing_save)
                    b2_label = "Hold=Save";
                else if(can_hold_load)
                    b2_label = "Hold=Load";
                else
                    b2_label = "";

                DrawControlRow(kFooterRow1Y, false, kFooterDividerY,
                                 (choosing_save || browsing_load) ? "Scroll" : "", "", "", "");
                DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, b1_label, "", "",
                                 b2_label);
            }
            break;
        }
        default: break;
    }
}

const char* Ui::MixerChannelName(int ch) const
{
    switch(ch)
    {
        case 0: return "L1";
        case 1: return "L2";
        case 2: return "L3";
        case 3: return "L4";
        case 4: return "GR";
        case 5: return "DXD";
        case 6: return "BYP";
        case 7: return "MAS";
        default: return "?";
    }
}

float Ui::MixerGetVolume01(int ch) const
{
    switch(ch)
    {
        case 0: case 1: case 2: case 3: return layers_[ch].GetVolume01();
        case 4: return granular_ ? granular_->GetOutputLevel01() : 0.f;
        case 5: return dexed_ ? dexed_->GetOutputLevel01() : 0.f;
        case 6: return bypass_mix_volume01_;
        case 7: return master_volume01_;
        default: return 0.f;
    }
}

float Ui::MixerGetPan01(int ch) const
{
    switch(ch)
    {
        case 0: case 1: case 2: case 3: return layers_[ch].GetPan01();
        case 4: return granular_ ? granular_->GetPan01() : 0.5f;
        case 5: return dexed_ ? dexed_->GetPan01() : 0.5f;
        case 6: return bypass_pan01_;
        default: return 0.5f; // Master has none
    }
}

float Ui::MixerGetSend01(int ch) const
{
    switch(ch)
    {
        case 0: case 1: case 2: case 3: return layers_[ch].GetReverbSend01();
        case 4: return granular_ ? granular_->GetReverbSend01() : 0.f;
        case 5: return dexed_ ? dexed_->GetReverbSend01() : 0.f;
        case 6: return bypass_reverb_send01_;
        default: return 0.f; // Master uses Reverb Size instead (see reverb_size01_)
    }
}

void Ui::MixerSetVolume01(int ch, float v01)
{
    switch(ch)
    {
        case 0: case 1: case 2: case 3: layers_[ch].SetVolume01(v01); break;
        case 4: if(granular_) granular_->SetOutputLevel01(v01); break;
        case 5: if(dexed_) dexed_->SetOutputLevel01(v01); break;
        case 6: SetBypassMixVolume01(v01); break;
        case 7:
            master_volume01_ = Clampf(v01, 0.f, 1.f);
            master_volume_   = powf(master_volume01_, 2.5f) * 1.43f;
            if(master_volume_ < 0.f)
                master_volume_ = 0.f;
            break;
        default: break;
    }
}

void Ui::MixerSetPan01(int ch, float v01)
{
    switch(ch)
    {
        case 0: case 1: case 2: case 3: layers_[ch].SetPan01(v01); break;
        case 4: if(granular_) granular_->SetPan01(v01); break;
        case 5: if(dexed_) dexed_->SetPan01(v01); break;
        case 6: SetBypassPan01(v01); break;
        default: break; // Master has no Pan -- no-op
    }
}

void Ui::MixerSetSend01(int ch, float v01)
{
    switch(ch)
    {
        case 0: case 1: case 2: case 3: layers_[ch].SetReverbSend01(v01); break;
        case 4: if(granular_) granular_->SetReverbSend01(v01); break;
        case 5: if(dexed_) dexed_->SetReverbSend01(v01); break;
        case 6: bypass_reverb_send01_ = Clampf(v01, 0.f, 1.f); break;
        default: break; // Master uses Reverb Size instead, set directly in ApplyKnobs()
    }
}

bool Ui::MixerChannelHasPan(int ch) const
{
    // Only Master lacks a Pan concept now -- Dexed got a real Pan
    // control after a user report that its live output, having no pan
    // stage at all, reached the mix disproportionately loud next to
    // every other source's own center-pan-attenuated signal.
    return ch != kNumMixerChannels - 1;
}

// Vertical fader-style bar -- (x0,y0) is the box's top-left corner, fills
// from the BOTTOM up (unlike every other bar in this project, which fills
// left-to-right -- see the Mixer design discussion this was built from
// for why a vertical fader reads better for a per-channel level here).
void Ui::DrawMixerVBar(int x0, int y0, int w, int h, float v01)
{
    int x1 = x0 + w - 1;
    int y1 = y0 + h - 1;
    disp_->DrawRect(x0, y0, x1, y1, true, false);
    int fill_h = (int)(Clampf(v01, 0.f, 1.f) * (float)(h - 2) + 0.5f);
    if(fill_h > 0)
        disp_->DrawRect(x0 + 1, y1 - fill_h, x1 - 1, y1 - 1, true, true);
}

void Ui::DrawMixerOverviewGrid(int top_y)
{
    // All 8 names across in one row, same per-channel aesthetic as
    // Screen::Mixer's own Detail page (name on top, a real vertical bar
    // below whose own outline shows the full min/max range) -- just one
    // bar per column instead of two, and no numeric readout (the bar
    // itself is the readout here). No selection box here -- unlike
    // Screen::Mixer's own Detail page, there's no meaningful "currently
    // selected" channel on this at-a-glance summary; mixer_position_ is
    // just wherever Screen::Mixer was last left, not something being
    // actively chosen from here.
    // Bars stop at y=37, not the usual y=45 footer-divider margin -- this
    // grid is also used above the "PUSH ENC TO ENTER" caption on
    // Global:Mixer (see DrawGlobalScreen()), which needs its own clear
    // strip beneath.
    const int kColW  = disp_->Width() / kNumMixerChannels;
    const int kBarW  = 8;
    const int kBarY0 = top_y + 4;
    const int kBarH  = 37 - kBarY0;
    for(int ch = 0; ch < kNumMixerChannels; ch++)
    {
        int col_x    = kColW * ch;
        int center_x = col_x + kColW / 2;

        const char* name = MixerChannelName(ch);
        int         nw   = TomThumbAdvanceWidth(name);
        TomThumbDrawText(disp_, center_x - nw / 2, top_y + 1, name, true);

        DrawMixerVBar(center_x - kBarW / 2, kBarY0, kBarW, kBarH, MixerGetVolume01(ch));
    }
}

void Ui::DrawDexedScreen()
{
    if(!dexed_)
        return;

    const char* page_name = "Algo";
    switch(dexed_param_page_)
    {
        case DexedParamPage::Algo: page_name = "Algo"; break;
        case DexedParamPage::Feedback: page_name = "Feedback"; break;
        case DexedParamPage::Vibrato: page_name = "Vibrato"; break;
        case DexedParamPage::Brightness: page_name = "Bright"; break;
        case DexedParamPage::EnvSpeed: page_name = "EnvSpd"; break;
        case DexedParamPage::Filter: page_name = "Filter"; break;
        case DexedParamPage::Mix: page_name = "Mix"; break;
        case DexedParamPage::Advanced: page_name = "Advanced"; break;
        case DexedParamPage::Preset: page_name = "Preset"; break;
        default: break;
    }
    char title[24];
    snprintf(title, sizeof(title), "Dexed:%s", page_name);
    disp_->SetCursor(0, 0);
    WriteUpper(title);
    DrawBeatIndicator(disp_->Width() - 41, 0, 3);
    disp_->DrawLine(0, 9, disp_->Width() - 1, 9, true);

    switch(dexed_param_page_)
    {
        case DexedParamPage::Algo:
        {
            // Algorithm number (top-left, below the header) plus a real
            // box diagram of its exact operator routing filling the rest
            // of the page -- generically derived per-algorithm from
            // FmCore::get_operator_routing() (never hand-authored per
            // algorithm), so this is always exactly correct for whichever
            // of the 32 is loaded.
            int  algo0 = dexed_->GetPatchByte(134);
            char line1[16];
            snprintf(line1, sizeof(line1), "ALGO %d/32", algo0 + 1);
            TomThumbDrawText(disp_, 0, 15, line1, true);

            // --- Reconstruct the signal-flow graph -------------------
            // Array index 0 = hardware OP6, index 5 = OP1 (confirmed from
            // the real DX7 SysEx spec), matching render()'s own
            // processing order (index 0..5) -- a bus can only ever be
            // read by an operator whose index is higher than whichever
            // operator(s) wrote it, so a single forward pass is enough to
            // resolve every edge and depth.
            int  input_bus[6], output_bus[6];
            bool sums[6], is_carrier[6] = {}, has_fb[6];
            for(int i = 0; i < 6; i++)
            {
                FmOperatorRouting r = FmCore::get_operator_routing((uint8_t)algo0, i);
                input_bus[i]  = r.input_bus;
                output_bus[i] = r.output_bus;
                sums[i]       = r.sums;
                has_fb[i]     = r.has_feedback;
            }

            int  edge_count[6] = {};
            int  edge_from[6][6];
            int  cur_writer_count[3] = {0, 0, 0}; // indexed by bus 1/2 (0 unused)
            int  cur_writers[3][6];
            for(int i = 0; i < 6; i++)
            {
                int ib = input_bus[i];
                if(ib != 0)
                {
                    for(int k = 0; k < cur_writer_count[ib]; k++)
                        edge_from[i][edge_count[i]++] = cur_writers[ib][k];
                }
                int ob = output_bus[i];
                if(ob == 0)
                {
                    is_carrier[i] = true;
                }
                else if(sums[i] && cur_writer_count[ob] > 0)
                {
                    cur_writers[ob][cur_writer_count[ob]++] = i;
                }
                else
                {
                    cur_writers[ob][0] = i;
                    cur_writer_count[ob] = 1;
                }
            }

            // Row = depth from this operator's own chain root.
            int depth[6];
            for(int i = 0; i < 6; i++)
            {
                if(edge_count[i] == 0)
                {
                    depth[i] = 0;
                }
                else
                {
                    int maxd = 0;
                    for(int k = 0; k < edge_count[i]; k++)
                    {
                        int pd = depth[edge_from[i][k]];
                        if(pd > maxd)
                            maxd = pd;
                    }
                    depth[i] = maxd + 1;
                }
            }

            // Column: each independent chain root gets the next free
            // column (in processing order, i.e. HW op6 first); an
            // operator with a single parent extends that parent's column
            // (straight chain) UNLESS the parent already has an earlier
            // child claiming that column -- a parent feeding more than
            // one child (e.g. algorithm 19's op6 feeding both op5 and
            // op4) branches its later children into fresh columns
            // instead of stacking them on top of each other. An operator
            // with more than one parent (a converging algorithm, e.g.
            // 16-18) sits centered between its parents' columns instead.
            float column[6];
            bool  column_claimed[6] = {};
            int   next_root_col     = 0;
            for(int i = 0; i < 6; i++)
            {
                if(edge_count[i] == 0)
                {
                    column[i] = (float)(next_root_col++);
                }
                else if(edge_count[i] == 1)
                {
                    int p = edge_from[i][0];
                    if(!column_claimed[p])
                    {
                        column[i]         = column[p];
                        column_claimed[p] = true;
                    }
                    else
                    {
                        column[i] = (float)(next_root_col++);
                    }
                }
                else
                {
                    float sum = 0.f;
                    for(int k = 0; k < edge_count[i]; k++)
                        sum += column[edge_from[i][k]];
                    column[i] = sum / (float)edge_count[i];
                }
            }
            int num_columns = next_root_col;
            int max_depth   = 0;
            for(int i = 0; i < 6; i++)
                if(depth[i] > max_depth)
                    max_depth = depth[i];
            int num_rows = max_depth + 1;

            // --- Pixel layout, orthogonal connectors only -------------
            // Diagram width stops short of the full screen width, and the
            // OUT bus line only spans that same reduced width -- reserves
            // a strip on the right no box or connector ever enters, so
            // the "OUT" label can sit there with no risk of collision
            // regardless of algorithm/column count.
            const int kOutLabelW = 16;
            const int kDiagTop   = 17;
            const int kOutLineY  = kFooterDividerY - 1; // 45, just above the footer rule
            const int kDiagWidth = disp_->Width() - kOutLabelW;
            const float col_pitch = (float)kDiagWidth / (float)num_columns;
            const float row_pitch = (float)(kOutLineY - kDiagTop) / (float)num_rows;
            // Width is fixed and snug around the number's own ink (3px
            // wide, see font_tomthumb.cpp's glyph table) plus a real 1px
            // gap and a 1px border on each side -- not stretched to the
            // column pitch, which has far more room to spare than the
            // number ever needs and was what made boxes read as too
            // wide. Height instead scales with row_pitch (rows are the
            // genuinely scarce dimension), so the 29 algorithms with more
            // vertical room to spare get visibly taller boxes; the floor
            // (7 = 5px ink + 1px border each side, no extra gap left) is
            // only actually reached by the three 4-row algorithms
            // (1, 2, 18), where there's no more height to give.
            const int box_w = 7;
            int       box_h = (int)row_pitch;
            if(box_h > 11) box_h = 11;
            if(box_h < 7) box_h = 7;

            int cx[6], cy[6];
            for(int i = 0; i < 6; i++)
            {
                cx[i] = (int)(col_pitch * (column[i] + 0.5f));
                cy[i] = kDiagTop + (int)(row_pitch * ((float)depth[i] + 0.5f));
            }

            // Connectors first (drawn under the boxes). A same-column
            // parent drops a plain straight vertical line into the
            // child's top edge. A cross-column parent (a converging
            // algorithm, e.g. 16-18, or a parent branching into more than
            // one child, e.g. 19) instead drops down to the CHILD's own
            // row height and runs a horizontal segment straight into the
            // middle of the child's near side edge -- no separate gutter
            // row needed above the child, which is what leaves the extra
            // vertical room for bigger boxes. Horizontal/vertical
            // segments only, as required.
            for(int i = 0; i < 6; i++)
            {
                for(int k = 0; k < edge_count[i]; k++)
                {
                    int p  = edge_from[i][k];
                    int py = cy[p] + box_h / 2;
                    if(cx[p] == cx[i])
                    {
                        disp_->DrawLine(cx[p], py, cx[i], cy[i] - box_h / 2, true);
                    }
                    else
                    {
                        int side_x = cx[p] < cx[i] ? cx[i] - box_w / 2 : cx[i] + box_w / 2;
                        disp_->DrawLine(cx[p], py, cx[p], cy[i], true);
                        disp_->DrawLine(cx[p], cy[i], side_x, cy[i], true);
                    }
                }
            }

            // OUT bus: every carrier drops a straight vertical line down
            // to one shared horizontal line at the bottom of the diagram
            // -- stops at kDiagWidth, clear of the reserved "OUT" label
            // strip (see kOutLabelW above).
            disp_->DrawLine(0, kOutLineY, kDiagWidth - 1, kOutLineY, true);
            TomThumbDrawText(disp_, kDiagWidth + 2, kOutLineY - 1, "OUT", true);
            for(int i = 0; i < 6; i++)
                if(is_carrier[i])
                    disp_->DrawLine(cx[i], cy[i] + box_h / 2, cx[i], kOutLineY, true);

            // Boxes: filled = carrier (reaches the ear directly), outline
            // = modulator. The real HW operator number (6-index) is
            // punched through as "off" pixels when the box is filled so
            // it still reads clearly against the solid fill.
            for(int i = 0; i < 6; i++)
            {
                int x0 = cx[i] - box_w / 2, x1 = cx[i] + box_w / 2;
                int y0 = cy[i] - box_h / 2, y1 = cy[i] + box_h / 2;
                disp_->DrawRect(x0, y0, x1, y1, true, is_carrier[i]);
                char num[2]  = {(char)('0' + (6 - i)), 0};
                int  ink_w   = TomThumbInkWidth(num);
                // Centered on ink width (not advance width) so the
                // number sits with a true, symmetric 1px-or-more gap
                // from the border on both sides -- box_w (7) is sized
                // for the common 3px-wide digits, so '1' (2px ink) gets
                // an extra spare pixel on the right rather than being
                // pushed off-center.
                int pen_x = x0 + 1 + (box_w - 2 - ink_w) / 2;
                // Same margin-based centering vertically: ink is a fixed
                // 5px tall, baseline sits 5 rows below its top -- when
                // box_h is at its 7px floor this comes out flush against
                // the border (no room left for a gap that small), but it
                // grows into a real, symmetric gap on every algorithm
                // with a taller box to spare.
                int pen_y = y0 + 6 + (box_h - 7) / 2;
                TomThumbDrawText(disp_, pen_x, pen_y, num, !is_carrier[i]);

                // Feedback: an explicit "FB" tag beside the one operator
                // (if any) with real self-feedback -- a small pixel
                // bracket read poorly at this box size across several
                // algorithms, so this uses actual text instead, which
                // stays legible regardless of where that operator lands.
                // Feedback operators are always chain roots (no incoming
                // edges, confirmed from the routing data), so connectors
                // only ever leave from their BOTTOM -- both side edges
                // are always free for this label. Prefers the right side;
                // falls back to the left if that would run past the
                // reserved OUT-label strip.
                if(has_fb[i])
                {
                    const char* fb = "FB";
                    int         fb_w = TomThumbInkWidth(fb);
                    int         fb_x = x1 + 2;
                    if(fb_x + fb_w > kDiagWidth)
                        fb_x = x0 - 2 - fb_w;
                    TomThumbDrawText(disp_, fb_x, pen_y, fb, true);
                }
            }

            char algo_val[8];
            snprintf(algo_val, sizeof(algo_val), "%d", algo0 + 1);
            DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "Algo", algo_val, "", "");
            DrawControlRow(
                kFooterRow2Y, true, kFooterInterRowDividerY, "Cycle -", "", "", "Cycle +");
            break;
        }
        case DexedParamPage::Feedback:
        {
            char val[8];
            snprintf(val, sizeof(val), "%d", dexed_->GetPatchByte(135));
            disp_->SetCursor(0, 20);
            WriteUpper("Feedback");
            DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "Amt", val, "", "");
            DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, "", "", "", "");
            break;
        }
        case DexedParamPage::Vibrato:
        {
            char speed_val[8], depth_val[8];
            snprintf(speed_val, sizeof(speed_val), "%d", dexed_->GetPatchByte(137));
            snprintf(depth_val, sizeof(depth_val), "%d", dexed_->GetPatchByte(139));
            disp_->SetCursor(0, 20);
            WriteUpper("Vibrato (automatic)");
            DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "Speed", speed_val, depth_val,
                             "Depth");
            DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, "", "", "", "");
            break;
        }
        case DexedParamPage::Brightness:
        {
            char val[8];
            snprintf(val, sizeof(val), "%d%%", (int)(dexed_->GetBrightness01() * 100.f + 0.5f));
            disp_->SetCursor(0, 20);
            WriteUpper("Brightness");
            disp_->SetCursor(0, 30);
            WriteUpper("50% = as saved");
            DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "Amt", val, "", "");
            DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, "", "", "", "");
            break;
        }
        case DexedParamPage::EnvSpeed:
        {
            char val[8];
            snprintf(val, sizeof(val), "%d%%", (int)(dexed_->GetEnvSpeed01() * 100.f + 0.5f));
            disp_->SetCursor(0, 20);
            WriteUpper("Envelope Speed");
            disp_->SetCursor(0, 30);
            WriteUpper("50% = as saved");
            DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "Amt", val, "", "");
            DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, "", "", "", "");
            break;
        }
        case DexedParamPage::Filter:
        {
            char mode_line[20];
            snprintf(mode_line, sizeof(mode_line), "Mode: %s",
                      FilterModeName(dexed_->GetFilterMode()));
            disp_->SetCursor(0, 20);
            WriteUpper(mode_line);

            char cutoff_val[8], res_val[8];
            snprintf(cutoff_val, sizeof(cutoff_val), "%d%%",
                      (int)(dexed_->GetFilterCutoff01() * 100.f + 0.5f));
            snprintf(res_val, sizeof(res_val), "%d%%",
                      (int)(dexed_->GetFilterResonance01() * 100.f + 0.5f));
            DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "Cutoff", cutoff_val, res_val,
                             "Res");
            DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, "Cycle mode", "", "", "");
            break;
        }
        case DexedParamPage::Mix:
        {
            char send_val[8], out_val[8];
            snprintf(send_val, sizeof(send_val), "%d%%",
                      (int)(dexed_->GetReverbSend01() * 100.f + 0.5f));
            snprintf(out_val, sizeof(out_val), "%d%%",
                      (int)(dexed_->GetOutputLevel01() * 100.f + 0.5f));
            disp_->SetCursor(0, 20);
            WriteUpper("Mix");
            DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "Rev", send_val, out_val,
                             "Level");
            DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, "", "", "", "");
            break;
        }
        case DexedParamPage::Advanced:
        {
            // Entry point into Screen::DexedOperator -- same "Click to
            // open..." idiom as GlobalPage::Granular/Dexed's own entry
            // pages, just reached by a click on this page instead of a
            // Global one.
            disp_->SetCursor(0, 20);
            WriteUpper("Per-operator editor");
            disp_->SetCursor(0, 30);
            WriteUpper("Click to open");
            DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "", "", "", "");
            DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, "", "", "", "");
            break;
        }
        case DexedParamPage::Preset:
        {
            if(!PerformanceStore::IsCardPresent())
            {
                disp_->SetCursor(0, 20);
                WriteUpper("No card");
                DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "", "", "", "");
                DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, "Retry", "", "", "");
                break;
            }
            if(dexed_preset_slots_dirty_)
                RefreshDexedPresetSlots();

            bool choosing_save    = save_load_mode_ == SaveLoadMode::ChoosingSave;
            bool browsing_load    = save_load_mode_ == SaveLoadMode::BrowsingLoad;
            bool load_chooser     = browsing_load && !load_browsing_files_;
            bool browsing_folders
                = browsing_load && load_browsing_files_ && !dexed_preset_folder_open_;
            // A folder name isn't a preset -- nothing to confirm-load
            // until "New" is picked at the top chooser, or a folder is
            // actually open onto a real preset (see
            // dexed_preset_folder_open_'s own comment).
            bool can_hold_load = browsing_load
                                  && (load_new_selected_
                                      || (load_browsing_files_ && dexed_preset_folder_open_));

            if(pod_->button2.Pressed() && (choosing_save || can_hold_load))
            {
                float       held     = pod_->button2.TimeHeldMs();
                int         w = (int)(Clampf(held / 800.f, 0.f, 1.f) * (disp_->Width() - 2));
                const char* hold_msg = choosing_save ? "Hold: Save..." : "Hold: Load...";
                disp_->SetCursor(0, 20);
                WriteUpper(hold_msg);
                disp_->DrawRect(0, 30, disp_->Width() - 1, 34, true, false);
                if(w > 0)
                    disp_->DrawRect(1, 31, w, 33, true, true);
                DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "", "", "", "");
                DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, "", "", "", "");
                break;
            }

            if(choosing_save)
            {
                bool can_overwrite
                    = dexed_loaded_preset_slot_ > DexedSynth::GetNumFactoryPresets();
                char line1[24], line2[16];
                snprintf(line1, sizeof(line1), "%c Overwrite%s", !save_as_new_ ? '>' : ' ',
                          can_overwrite ? "" : " (n/a)");
                snprintf(line2, sizeof(line2), "%c Save New", save_as_new_ ? '>' : ' ');
                disp_->SetCursor(0, 16);
                WriteUpper(line1);
                disp_->SetCursor(0, 28);
                WriteUpper(line2);
            }
            else if(load_chooser)
            {
                char line1[24], line2[16];
                snprintf(line1, sizeof(line1), "%c Files", !load_new_selected_ ? '>' : ' ');
                snprintf(line2, sizeof(line2), "%c Load New", load_new_selected_ ? '>' : ' ');
                disp_->SetCursor(0, 16);
                WriteUpper(line1);
                disp_->SetCursor(0, 28);
                WriteUpper(line2);
            }
            else if(browsing_folders)
            {
                int count = dexed_preset_folder_cursor_ < DexedSynth::kNumFactoryCategories
                                ? DexedSynth::GetFactoryCategoryCount(dexed_preset_folder_cursor_)
                                : dexed_preset_user_slot_count_;
                // No "Folder: " label -- Font_6x8 is fixed 6px/char, and
                // "Folder: Woodwind 3 (64)" (23 chars) overflows the
                // 128px display (21 chars max), pushing the count off
                // the right edge. The longest real name+count ("Woodwind
                // 3 (64)", 15 chars) fits comfortably without the label.
                char line2[24];
                snprintf(line2, sizeof(line2), "%s (%d)",
                          dexed_preset_folder_cursor_ < DexedSynth::kNumFactoryCategories
                              ? DexedSynth::GetFactoryCategoryName(dexed_preset_folder_cursor_)
                              : "User",
                          count);
                disp_->SetCursor(0, 20);
                WriteUpper(line2);
            }
            else if(browsing_load) // load_browsing_files_ && dexed_preset_folder_open_
            {
                int  browsed_slot = ResolveDexedPresetSlot();
                char line2[24];
                if(browsed_slot < 0)
                    snprintf(line2, sizeof(line2), "(empty)");
                else if(browsed_slot <= DexedSynth::GetNumFactoryPresets())
                    snprintf(line2, sizeof(line2), "Load: %s",
                              DexedSynth::GetFactoryPresetName(browsed_slot - 1));
                else
                    snprintf(line2, sizeof(line2), "Load: %d", browsed_slot);
                disp_->SetCursor(0, 20);
                WriteUpper(line2);
            }
            else
            {
                char line1[24];
                if(dexed_loaded_preset_slot_ <= 0)
                    snprintf(line1, sizeof(line1), "Now: (custom)");
                else if(dexed_loaded_preset_slot_ <= DexedSynth::GetNumFactoryPresets())
                    snprintf(line1, sizeof(line1), "Now: %s",
                              DexedSynth::GetFactoryPresetName(dexed_loaded_preset_slot_ - 1));
                else
                    snprintf(line1, sizeof(line1), "Now: %d", dexed_loaded_preset_slot_);
                disp_->SetCursor(0, 20);
                WriteUpper(line1);
                if(dexed_preset_status_[0] != '\0')
                {
                    disp_->SetCursor(0, 32);
                    WriteUpper(dexed_preset_status_);
                }
            }

            {
                const char* b1_label;
                if(save_load_mode_ == SaveLoadMode::Idle)
                    b1_label = "Save";
                else if(load_chooser && !load_new_selected_)
                    b1_label = "Select";
                else if(browsing_folders)
                    b1_label = "Open";
                else
                    b1_label = "Back";
                const char* b2_label;
                if(save_load_mode_ == SaveLoadMode::Idle)
                    b2_label = "Load";
                else if(choosing_save)
                    b2_label = "Hold=Save";
                else if(load_browsing_files_ && dexed_preset_folder_open_)
                    b2_label = "Prev./Hold=Load";
                else if(can_hold_load)
                    b2_label = "Hold=Load";
                else
                    b2_label = "";

                DrawControlRow(kFooterRow1Y, false, kFooterDividerY,
                                 (choosing_save || browsing_load) ? "Scroll" : "", "", "", "");
                DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, b1_label, "", "",
                                 b2_label);
            }
            break;
        }
        default: break;
    }
}

void Ui::DrawDexedOperatorScreen()
{
    if(!dexed_)
        return;

    // Real HW op number (6-index), same convention the Algo diagram
    // already uses -- confirmed from the real DX7 SysEx spec that array
    // index 0 is HW OP6, index 5 is OP1.
    int hw_op = 6 - dexed_op_index_;
    int base  = dexed_op_index_ * 21;

    const char* page_name = "Ratio";
    switch(dexed_op_page_)
    {
        case DexedOpParamPage::RatioLevel: page_name = "Ratio"; break;
        case DexedOpParamPage::Detune: page_name = "Detune"; break;
        case DexedOpParamPage::EgRate: page_name = "EG Rate"; break;
        case DexedOpParamPage::EgLevel: page_name = "EG Level"; break;
        default: break;
    }
    char title[24];
    snprintf(title, sizeof(title), "Op %d:%s", hw_op, page_name);
    disp_->SetCursor(0, 0);
    WriteUpper(title);
    DrawBeatIndicator(disp_->Width() - 41, 0, 3);
    disp_->DrawLine(0, 9, disp_->Width() - 1, 9, true);

    switch(dexed_op_page_)
    {
        case DexedOpParamPage::RatioLevel:
        {
            char ratio_val[8], level_val[8];
            snprintf(ratio_val, sizeof(ratio_val), "%d", dexed_->GetPatchByte(base + 18));
            snprintf(level_val, sizeof(level_val), "%d", dexed_->GetPatchByte(base + 16));
            DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "Ratio", ratio_val, level_val,
                             "Level");
            DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, "Op", "", "", "");
            break;
        }
        case DexedOpParamPage::Detune:
        {
            char val[8];
            snprintf(val, sizeof(val), "%d", dexed_->GetPatchByte(base + 20));
            disp_->SetCursor(0, 20);
            WriteUpper("7 = centered");
            DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "Detune", val, "", "");
            DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, "Op", "", "", "");
            break;
        }
        case DexedOpParamPage::EgRate:
        {
            // Real DX7 R1-R4 aren't literally an ADSR envelope, but
            // naming them Attack/Decay/Sustain/Release (matching this
            // same AD/SR-toggle idiom's own "AD"/"SR" pairing) reads far
            // more clearly than "Rate1..4" -- requested directly after
            // real hardware testing.
            bool    sr = dexed_op_egrate_target_sr_;
            uint8_t rates[4], levels[4];
            for(int i = 0; i < 4; i++)
            {
                rates[i]  = dexed_->GetPatchByte(base + i);
                levels[i] = dexed_->GetPatchByte(base + 4 + i);
            }
            DrawDx7EnvelopeShape(14, 44, rates, levels);
            char v1[8], v2[8];
            snprintf(v1, sizeof(v1), "%d", dexed_->GetPatchByte(sr ? base + 2 : base + 0));
            snprintf(v2, sizeof(v2), "%d", dexed_->GetPatchByte(sr ? base + 3 : base + 1));
            DrawControlRow(kFooterRow1Y, false, kFooterDividerY, sr ? "Sustain" : "Attack", v1, v2,
                             sr ? "Release" : "Decay");
            DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, "Op", "", "", "AD/SR");
            break;
        }
        case DexedOpParamPage::EgLevel:
        {
            // Same Attack/Decay/Sustain/Release naming as the EgRate
            // page above, and for the same reason -- the title bar
            // ("Op N:EG Level" vs "...EG Rate") already disambiguates
            // level from rate, so reusing identical stage names here
            // instead of "Level1..4" is consistent, not ambiguous.
            bool    sr = dexed_op_eglevel_target_sr_;
            uint8_t rates[4], levels[4];
            for(int i = 0; i < 4; i++)
            {
                rates[i]  = dexed_->GetPatchByte(base + i);
                levels[i] = dexed_->GetPatchByte(base + 4 + i);
            }
            DrawDx7EnvelopeShape(14, 44, rates, levels);
            char v1[8], v2[8];
            snprintf(v1, sizeof(v1), "%d", dexed_->GetPatchByte(sr ? base + 6 : base + 4));
            snprintf(v2, sizeof(v2), "%d", dexed_->GetPatchByte(sr ? base + 7 : base + 5));
            DrawControlRow(kFooterRow1Y, false, kFooterDividerY, sr ? "Sustain" : "Attack", v1, v2,
                             sr ? "Release" : "Decay");
            DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, "Op", "", "", "AD/SR");
            break;
        }
        default: break;
    }
}

void Ui::DrawMixerScreen()
{
    DrawBeatIndicator(disp_->Width() - 41, 0, 3);
    disp_->DrawLine(0, 9, disp_->Width() - 1, 9, true);

    if(mixer_position_ == kNumMixerChannels) // Scope stop
    {
        disp_->SetCursor(0, 0);
        WriteUpper("Mixer:Scope");
        TomThumbDrawText(disp_, 0, 15, "Final Mix", true);
        DrawOscilloscope(19, 44, master_scope_buf_, master_scope_capacity_);
        DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "", "", "", "");
        DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, "", "", "", "");
        return;
    }

    // A real channel -- whichever group of 4 it belongs to (0-3 = the
    // loop layers, 4-7 = Plaits/Grains/Bypass/Master), with real Vol/
    // Rev/Pan bars for each of those 4 at once.
    bool is_master = mixer_position_ == kNumMixerChannels - 1;
    char title[24];
    snprintf(title, sizeof(title), "Mixer:%s", MixerChannelName(mixer_position_));
    disp_->SetCursor(0, 0);
    WriteUpper(title);

    int       group_start = mixer_position_ < 4 ? 0 : 4;
    const int kColW       = disp_->Width() / 4;
    // Centered between the header divider (y=9) and footer divider
    // (kFooterDividerY=46) -- no per-bar "Vol"/"Rev" text any more (the
    // footer already spells those out for the selected channel), so the
    // freed-up room goes to real vertical centering instead of cramming
    // everything against the top.
    const int kBarW = 12, kBarGap = 2, kBarY0 = 20, kBarH = 18;
    const int kPanY = 39, kPanH = 4;

    for(int col = 0; col < 4; col++)
    {
        int  ch       = group_start + col;
        bool selected = ch == mixer_position_;
        int  col_x    = kColW * col;
        int  bars_w   = kBarW * 2 + kBarGap;
        int  bars_x0  = col_x + (kColW - bars_w) / 2;

        if(selected)
            disp_->DrawRect(col_x + 1, 11, col_x + kColW - 2, 44, true, false);

        const char* name   = MixerChannelName(ch);
        int         nw     = TomThumbAdvanceWidth(name);
        int         name_x = col_x + (kColW - nw) / 2;
        TomThumbDrawText(disp_, name_x, 17, name, true);

        bool  channel_is_master = ch == kNumMixerChannels - 1;
        float vol01             = MixerGetVolume01(ch);
        float second01          = channel_is_master ? reverb_size01_ : MixerGetSend01(ch);

        DrawMixerVBar(bars_x0, kBarY0, kBarW, kBarH, vol01);
        DrawMixerVBar(bars_x0 + kBarW + kBarGap, kBarY0, kBarW, kBarH, second01);

        if(MixerChannelHasPan(ch))
        {
            float pan01 = MixerGetPan01(ch);
            disp_->DrawRect(bars_x0, kPanY, bars_x0 + bars_w - 1, kPanY + kPanH - 1, true, false);
            int tick_x = bars_x0 + 1
                         + (int)(Clampf(pan01, 0.f, 1.f) * (float)(bars_w - 3) + 0.5f);
            disp_->DrawRect(tick_x, kPanY + 1, tick_x, kPanY + kPanH - 2, true, true);
        }
    }

    char vol_val[8], second_val[8];
    snprintf(vol_val, sizeof(vol_val), "%d%%",
              (int)(MixerGetVolume01(mixer_position_) * 100.f + 0.5f));
    if(is_master)
    {
        snprintf(second_val, sizeof(second_val), "%d%%", (int)(reverb_size01_ * 100.f + 0.5f));
        DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "Vol", vol_val, second_val, "RevSz");
        DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, "", "", "", "");
    }
    else if(!mixer_target_reverb_)
    {
        if(MixerChannelHasPan(mixer_position_))
        {
            char pan_val[8];
            snprintf(pan_val, sizeof(pan_val), "%d%%",
                      (int)(MixerGetPan01(mixer_position_) * 100.f + 0.5f));
            DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "Vol", vol_val, pan_val, "Pan");
            DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, "Vol/Pan*", "", "", "Rev");
        }
        else
        {
            // No channel currently lacks Pan besides Master (handled
            // above via is_master) -- kept as a real fallback rather
            // than an assert, in case that ever changes again.
            DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "Vol", vol_val, "", "");
            DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, "Vol*", "", "", "Rev");
        }
    }
    else
    {
        snprintf(second_val, sizeof(second_val), "%d%%",
                  (int)(MixerGetSend01(mixer_position_) * 100.f + 0.5f));
        DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "Rev", second_val, "", "");
        DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, "Vol/Pan", "", "", "Rev*");
    }
}

void Ui::DrawAdsrShape(int top, int bottom, float attack_s, float decay_s, float sustain01,
                        float release_s)
{
    // Duplicated from GranularEngine's own private range (same "duplicate
    // rather than expose internals" convention this project already uses
    // elsewhere) -- must match GranularEngine::kMinAdsrSeconds/
    // kMaxAdsrSeconds exactly or this graph and the footer's own seconds
    // readout would silently disagree about where a given knob position
    // lands.
    const float kMinAdsrSeconds = 0.005f;
    const float kMaxAdsrSeconds = 3.f;

    sustain01    = Clampf(sustain01, 0.f, 1.f);
    float attack01
        = Clampf((attack_s - kMinAdsrSeconds) / (kMaxAdsrSeconds - kMinAdsrSeconds), 0.f, 1.f);
    float decay01
        = Clampf((decay_s - kMinAdsrSeconds) / (kMaxAdsrSeconds - kMinAdsrSeconds), 0.f, 1.f);
    float release01
        = Clampf((release_s - kMinAdsrSeconds) / (kMaxAdsrSeconds - kMinAdsrSeconds), 0.f, 1.f);

    const int total_w = disp_->Width() - 2; // matches every other band's 1..Width()-2 span

    // Each stage gets a fixed, EQUAL quarter of the width -- a genuine
    // zone boundary that never moves regardless of what any of the other
    // knobs are set to (turning Decay never visibly shifts the Attack
    // ramp or Release ramp, even though neither changed).
    int x0    = 1;
    int zone1 = x0 + total_w * 1 / 4; // end of Attack's zone
    int zone2 = x0 + total_w * 2 / 4; // end of Decay's zone
    int zone3 = x0 + total_w * 3 / 4; // end of Sustain's zone
    int zone4 = x0 + total_w;         // end of Release's zone

    const int bottom_y  = bottom;
    const int top_y     = top;
    const int sustain_y = bottom - (int)(sustain01 * (float)(bottom - top));

    int x_attack_peak  = x0 + (int)(attack01 * (float)(zone1 - x0));
    int x_decay_peak   = zone1 + (int)(decay01 * (float)(zone2 - zone1));
    int x_release_peak = zone3 + (int)(release01 * (float)(zone4 - zone3));

    disp_->DrawLine(x0, bottom_y, x_attack_peak, top_y, true);         // Attack ramp
    disp_->DrawLine(x_attack_peak, top_y, zone1, top_y, true);         // hold at peak
    disp_->DrawLine(zone1, top_y, x_decay_peak, sustain_y, true);      // Decay ramp
    disp_->DrawLine(x_decay_peak, sustain_y, zone2, sustain_y, true);  // hold at sustain
    disp_->DrawLine(zone2, sustain_y, zone3, sustain_y, true);         // Sustain (flat, untimed)
    disp_->DrawLine(zone3, sustain_y, x_release_peak, bottom_y, true); // Release ramp
    disp_->DrawLine(x_release_peak, bottom_y, zone4, bottom_y, true);  // hold at 0
}

void Ui::DrawDx7EnvelopeShape(int top, int bottom, const uint8_t rates[4], const uint8_t levels[4])
{
    const int total_w = disp_->Width() - 2;
    const int x0       = 1;

    int prev_x = x0;
    int prev_y = bottom; // the envelope always starts silent before Rate1/Level1 fires

    for(int seg = 0; seg < 4; seg++)
    {
        int zone_start = x0 + total_w * seg / 4;
        int zone_end   = x0 + total_w * (seg + 1) / 4;

        // Higher rate = faster = reaches its own target level sooner
        // within the zone (a narrower ramp, more flat hold afterward) --
        // same "ramp then hold flat for the rest of the zone" idiom
        // DrawAdsrShape() above uses, just driven by a 0-99 rate byte
        // (bigger = faster) instead of a seconds value.
        float rate01 = Clampf((float)rates[seg] / 99.f, 0.f, 1.f);
        int   ramp_w = (int)((1.f - rate01) * (float)(zone_end - zone_start));
        if(ramp_w < 1)
            ramp_w = 1;
        int target_x = zone_start + ramp_w;
        if(target_x > zone_end)
            target_x = zone_end;
        int target_y
            = bottom - (int)(Clampf((float)levels[seg] / 99.f, 0.f, 1.f) * (float)(bottom - top));

        disp_->DrawLine(prev_x, prev_y, target_x, target_y, true); // this segment's own ramp
        if(target_x < zone_end)
            disp_->DrawLine(target_x, target_y, zone_end, target_y, true); // hold for the rest

        prev_x = zone_end;
        prev_y = target_y;
    }
}

void Ui::DrawOscilloscope(int top, int bottom, const float* buf, size_t capacity)
{
    const int center_y = (top + bottom) / 2;
    const int half_h    = (bottom - top) / 2;

    if(!buf || capacity == 0)
    {
        disp_->DrawLine(1, center_y, 1 + (LooperLayer::kWaveformCols - 1) * 2, center_y, true);
        return;
    }

    const size_t stride = capacity / (size_t)LooperLayer::kWaveformCols;

    // Auto-scale to the loudest sample currently in the capture -- a
    // fixed scale would either clip the trace off-screen or leave it
    // looking flat depending on how loud the pad's output currently is.
    float max_abs = 0.f;
    for(size_t i = 0; i < capacity; i++)
    {
        float a = fabsf(buf[i]);
        if(a > max_abs)
            max_abs = a;
    }
    float scale = max_abs > 0.001f ? 1.f / max_abs : 0.f;

    int prev_x = 1, prev_y = center_y;
    for(int col = 0; col < LooperLayer::kWaveformCols; col++)
    {
        size_t idx = (size_t)col * stride;
        if(idx >= capacity)
            idx = capacity - 1;
        float sample = Clampf(buf[idx] * scale, -1.f, 1.f);
        int   x      = 1 + col * 2;
        int   y      = center_y - (int)(sample * half_h);
        if(col > 0)
            disp_->DrawLine(prev_x, prev_y, x, y, true);
        prev_x = x;
        prev_y = y;
    }
}

void Ui::RefreshFileSlots()
{
    file_slot_count_ = PerformanceStore::ListSlots(file_slots_, kMaxFileSlots);
    if(file_cursor_ >= file_slot_count_)
        file_cursor_ = file_slot_count_ > 0 ? file_slot_count_ - 1 : 0;
    file_slots_dirty_ = false;
}

void Ui::TriggerSave(bool force_new)
{
    int slot = (!force_new && loaded_slot_ >= 0) ? loaded_slot_ : PerformanceStore::NextFreeSlot();
    if(slot < 0)
    {
        snprintf(file_status_, sizeof(file_status_), "Card full/missing");
        return;
    }

    g_progress_disp      = disp_;
    file_op_in_progress_ = true;
    bool ok = PerformanceStore::Save(slot, *tempo_, layers_, num_layers_, master_volume01_,
                                       bypass_, master_filter_mode_, master_filter_cutoff01_,
                                       master_filter_res01_, reverb_size01_,
                                       bypass_reverb_send01_, &Ui::OnSaveLoadProgress);
    file_op_in_progress_ = false;
    g_progress_disp       = nullptr;

    if(ok)
    {
        loaded_slot_ = slot;
        snprintf(file_status_, sizeof(file_status_), "Saved %d", slot);
        file_slots_dirty_ = true; // a new slot may now exist
    }
    else
    {
        snprintf(file_status_, sizeof(file_status_), "Fail:%s", PerformanceStore::GetLastError());
    }
}

void Ui::TriggerNew()
{
    for(int i = 0; i < num_layers_; i++)
        layers_[i].Clear();
    loaded_slot_ = -1; // next Save lands in a new slot, not over the old one
    snprintf(file_status_, sizeof(file_status_), "New perf");
}

void Ui::TriggerLoad()
{
    // !load_browsing_files_ means the top-level chooser confirmed with
    // "New" highlighted (can_confirm_load only allows a confirm here when
    // that's the case -- see HandleButton2()) -- confirming it is exactly
    // TriggerNew()'s own wipe.
    if(!load_browsing_files_)
    {
        TriggerNew();
        return;
    }
    int slot = file_slots_[file_cursor_];

    // Freeze the audio engine for the whole load: it restores layers one
    // at a time and each can take a while (streaming its recorded audio
    // off the SD card), and the audio ISR keeps running throughout a
    // blocking main-loop call like this one -- without suspending it, a
    // layer already restored earlier in Load() starts playing (and the
    // tempo clock keeps ticking) while later layers are still being
    // read, so every layer would resume at a different sample offset.
    // See audio_engine.h and TempoClock::ResetPhase().
    g_audio_suspended    = true;
    g_progress_disp      = disp_;
    file_op_in_progress_ = true;
    bool ok = PerformanceStore::Load(slot, *tempo_, layers_, num_layers_, &master_volume01_,
                                       &bypass_, &master_filter_mode_, &master_filter_cutoff01_,
                                       &master_filter_res01_, &reverb_size01_,
                                       &bypass_reverb_send01_, &Ui::OnSaveLoadProgress);
    file_op_in_progress_ = false;
    g_progress_disp       = nullptr;
    g_audio_suspended     = false; // every layer + tempo phase is consistent now

    // Project vari-speed is a live-performance control, not part of a
    // saved performance (same rule as master volume) -- always back to
    // 1.0x after a Load, same as at boot, regardless of success/failure.
    project_speed01_   = 0.5f;
    project_speed_     = 1.f;
    scrub_mode_active_ = false;

    if(ok)
    {
        loaded_slot_ = slot;
        // PerformanceStore::Load() only writes the raw 0..1 value; Ui
        // owns the volume curve, same as ApplyKnobs()'s Home-screen case.
        master_volume_ = powf(Clampf(master_volume01_, 0.f, 1.f), 2.5f) * 1.43f;
        if(master_volume_ < 0.f)
            master_volume_ = 0.f;
        snprintf(file_status_, sizeof(file_status_), "Loaded %d", slot);
    }
    else
    {
        snprintf(file_status_, sizeof(file_status_), "Fail:%s", PerformanceStore::GetLastError());
    }
}

const int* Ui::SdMgmtSlots(int* out_count)
{
    switch(sd_mgmt_folder_)
    {
        case SdMgmtFolder::Performances:
            if(file_slots_dirty_)
                RefreshFileSlots();
            *out_count = file_slot_count_;
            return file_slots_;
        case SdMgmtFolder::GranularPresets:
            if(granular_preset_slots_dirty_)
                RefreshGranularPresetSlots();
            *out_count = granular_preset_user_slot_count_;
            return granular_preset_user_slots_;
        default:
            *out_count = 0;
            return nullptr;
    }
}

void Ui::TriggerSdMgmtDuplicate()
{
    int        count = 0;
    const int* slots = SdMgmtSlots(&count);
    if(!slots || sd_mgmt_cursor_ >= count)
        return;
    int  slot     = slots[sd_mgmt_cursor_];
    int  new_slot = -1;
    bool ok;
    g_progress_disp = disp_;
    switch(sd_mgmt_folder_)
    {
        case SdMgmtFolder::Performances:
            ok = PerformanceStore::DuplicateSlot(slot, &new_slot, &Ui::OnSaveLoadProgress);
            break;
        case SdMgmtFolder::GranularPresets:
            ok = PerformanceStore::DuplicateGranularPreset(slot, &new_slot,
                                                              &Ui::OnSaveLoadProgress);
            break;
        default: ok = false; break;
    }
    g_progress_disp = nullptr;

    if(ok)
    {
        snprintf(sd_mgmt_status_, sizeof(sd_mgmt_status_), "Copied to %d", new_slot);
        switch(sd_mgmt_folder_)
        {
            case SdMgmtFolder::Performances: file_slots_dirty_ = true; break;
            case SdMgmtFolder::GranularPresets: granular_preset_slots_dirty_ = true; break;
            default: break;
        }
    }
    else
    {
        snprintf(sd_mgmt_status_, sizeof(sd_mgmt_status_), "Fail:%s",
                  PerformanceStore::GetLastError());
    }
}

void Ui::TriggerSdMgmtDelete()
{
    int        count = 0;
    const int* slots = SdMgmtSlots(&count);
    if(!slots || sd_mgmt_cursor_ >= count)
        return;
    int  slot = slots[sd_mgmt_cursor_];
    bool ok;
    // Passing the relevant page's own "currently loaded" tracker lets
    // Delete*() keep it correct across the gap-closing renumbering it
    // does (see its own doc comment) -- otherwise a delete elsewhere in
    // the list could silently leave e.g. the File page's "Now: N" naming
    // the wrong performance (or Overwrite writing to it) after everything
    // above it shifts down.
    switch(sd_mgmt_folder_)
    {
        case SdMgmtFolder::Performances:
            ok = PerformanceStore::DeleteSlot(slot, &loaded_slot_);
            break;
        case SdMgmtFolder::GranularPresets:
            ok = PerformanceStore::DeleteGranularPreset(slot, &granular_loaded_preset_slot_);
            break;
        default: ok = false; break;
    }

    if(ok)
    {
        snprintf(sd_mgmt_status_, sizeof(sd_mgmt_status_), "Deleted %d", slot);
        switch(sd_mgmt_folder_)
        {
            case SdMgmtFolder::Performances: file_slots_dirty_ = true; break;
            case SdMgmtFolder::GranularPresets: granular_preset_slots_dirty_ = true; break;
            default: break;
        }
        // The list just shrank -- pull the cursor back in range rather
        // than leaving it pointing past the end (SdMgmtSlots() will
        // re-scan on the very next call, so the shrunk count is already
        // current the moment Draw() next asks for it).
        if(sd_mgmt_cursor_ > 0)
            sd_mgmt_cursor_--;
    }
    else
    {
        snprintf(sd_mgmt_status_, sizeof(sd_mgmt_status_), "Fail:%s",
                  PerformanceStore::GetLastError());
    }
}

void Ui::TriggerGranularCaptureFromLayer()
{
    if(!granular_ || !granular_capture_buf_l_ || !granular_capture_buf_r_)
        return;
    if(granular_capture_source_ < 0 || granular_capture_source_ >= num_layers_)
        return;
    int          layer = granular_capture_source_;
    size_t       len   = layers_[layer].GetRecordedLength();
    if(len > granular_capture_capacity_)
        len = granular_capture_capacity_;
    const float* src_l = layers_[layer].GetBufferL();
    const float* src_r = layers_[layer].GetBufferR();
    for(size_t i = 0; i < len; i++)
    {
        granular_capture_buf_l_[i] = src_l[i];
        granular_capture_buf_r_[i] = src_r[i];
    }
    granular_->SetSource(granular_capture_buf_l_, granular_capture_buf_r_, len);
    snprintf(granular_capture_status_, sizeof(granular_capture_status_), "Captured L%d",
              layer + 1);
    OnNewGranularCapture(len);
}

void Ui::RefreshGranularImportFiles()
{
    granular_import_file_count_
        = PerformanceStore::ListImportWavFiles(granular_import_names_, kMaxImportFiles);
    if(granular_import_cursor_ >= granular_import_file_count_)
        granular_import_cursor_ = granular_import_file_count_ > 0 ? granular_import_file_count_ - 1 : 0;
    granular_import_files_dirty_ = false;
}

void Ui::TriggerGranularImport()
{
    if(!granular_ || !granular_capture_buf_l_ || !granular_capture_buf_r_)
        return;
    if(granular_import_cursor_ < 0 || granular_import_cursor_ >= granular_import_file_count_)
        return;

    const char* filename = granular_import_names_[granular_import_cursor_];
    size_t      len       = 0;
    g_progress_disp = disp_;
    bool ok = PerformanceStore::ImportWav(filename, granular_capture_buf_l_,
                                            granular_capture_buf_r_, granular_capture_capacity_,
                                            &len, &Ui::OnSaveLoadProgress);
    g_progress_disp = nullptr;

    if(ok)
    {
        granular_->SetSource(granular_capture_buf_l_, granular_capture_buf_r_, len);
        OnNewGranularCapture(len);
        snprintf(granular_capture_status_, sizeof(granular_capture_status_), "Imported");
    }
    else
    {
        snprintf(granular_capture_status_, sizeof(granular_capture_status_), "Fail:%s",
                  PerformanceStore::GetLastError());
    }
}

void Ui::OnNewGranularCapture(size_t full_len)
{
    granular_capture_full_len_ = full_len;
    granular_trim_start01_     = 0.f;
    granular_trim_end01_       = 1.f;

    // Same downsampled-peak technique GranularEngine::SetSource() uses
    // internally, duplicated here rather than exposed from the engine --
    // this one always covers the FULL capture regardless of what trim is
    // currently active, which is the whole point of keeping it separate.
    for(int col = 0; col < GranularEngine::kWaveformCols; col++)
        granular_trim_full_peaks_[col] = 0.f;
    if(full_len > 0 && granular_capture_buf_l_)
    {
        size_t per_col = full_len / (size_t)GranularEngine::kWaveformCols;
        if(per_col < 1)
            per_col = 1;
        for(int col = 0; col < GranularEngine::kWaveformCols; col++)
        {
            size_t start = (size_t)col * per_col;
            size_t end   = start + per_col;
            if(end > full_len)
                end = full_len;
            float peak = 0.f;
            for(size_t i = start; i < end; i++)
            {
                float a = fabsf(granular_capture_buf_l_[i]);
                if(a > peak)
                    peak = a;
            }
            granular_trim_full_peaks_[col] = peak;
        }
    }
}

void Ui::ApplyGranularTrim()
{
    if(!granular_ || granular_capture_full_len_ == 0)
        return;
    // Minimum gap so the two points can never cross/collapse to nothing
    // audible -- 1% of the full capture, same spirit as this project's
    // other min-range guards (e.g. LooperLayer's own filter cutoff clamp).
    const float kMinGap = 0.01f;
    float       s       = Clampf(granular_trim_start01_, 0.f, 1.f);
    float       e       = Clampf(granular_trim_end01_, 0.f, 1.f);
    if(e < s + kMinGap)
        e = s + kMinGap > 1.f ? 1.f : s + kMinGap;

    size_t full = granular_capture_full_len_;
    size_t start = (size_t)(s * (float)full);
    size_t end   = (size_t)(e * (float)full);
    if(end > full)
        end = full;
    if(end <= start)
        end = start + 1 <= full ? start + 1 : full;

    granular_->SetSource(granular_capture_buf_l_ + start, granular_capture_buf_r_ + start,
                          end - start);
}

void Ui::RefreshGranularPresetSlots()
{
    granular_preset_user_slot_count_ = PerformanceStore::ListGranularPresets(
        granular_preset_user_slots_, kMaxGranularPresetSlots);
    if(granular_preset_cursor_ >= granular_preset_user_slot_count_)
        granular_preset_cursor_
            = granular_preset_user_slot_count_ > 0 ? granular_preset_user_slot_count_ - 1 : 0;
    granular_preset_slots_dirty_ = false;
}

void Ui::TriggerSaveGranularPreset(bool force_new)
{
    if(!granular_)
        return;
    // Smart save (force_new=false): overwrite granular_loaded_preset_slot_
    // if it names a real slot; nothing loaded (-1) has no slot to
    // overwrite, so fall back to a new one -- same reasoning TriggerSave()
    // uses for its own loaded-slot tracking.
    int slot = (!force_new && granular_loaded_preset_slot_ > 0)
                   ? granular_loaded_preset_slot_
                   : PerformanceStore::NextFreeGranularPresetSlot();
    if(slot < 0)
    {
        snprintf(granular_preset_status_, sizeof(granular_preset_status_), "Card full/missing");
        return;
    }

    GranularEngine::GranularPresetData preset = granular_->CapturePreset();
    g_progress_disp = disp_;
    bool ok = PerformanceStore::SaveGranularPreset(
        slot, preset, granular_->GetSourceL(), granular_->GetSourceR(), granular_->GetSourceLen(),
        &Ui::OnSaveLoadProgress);
    g_progress_disp = nullptr;

    if(ok)
    {
        granular_loaded_preset_slot_ = slot;
        snprintf(granular_preset_status_, sizeof(granular_preset_status_), "Saved %d", slot);
        granular_preset_slots_dirty_ = true; // a new slot may now exist
    }
    else
    {
        snprintf(granular_preset_status_, sizeof(granular_preset_status_), "Fail:%s",
                  PerformanceStore::GetLastError());
    }
}

void Ui::TriggerNewGranularPreset()
{
    if(!granular_)
        return;
    // The Load chooser's "New" pick (see ApplyKnobs()'s BrowsingLoad
    // handling) -- clears the captured audio and resets every param to
    // the engine's own defaults, same fresh-start spirit as Global:File's
    // own TriggerNew().
    granular_->ApplyPreset(GranularEngine::GranularPresetData{});
    granular_->SetSource(granular_capture_buf_l_, granular_capture_buf_r_, 0);
    OnNewGranularCapture(0);
    granular_loaded_preset_slot_ = 0;
    snprintf(granular_preset_status_, sizeof(granular_preset_status_), "New");
}

void Ui::TriggerLoadGranularPreset()
{
    if(!granular_)
        return;
    // !load_browsing_files_ means the top-level chooser confirmed with
    // "New" highlighted (can_confirm_load only allows a confirm here when
    // that's the case -- see HandleButton2()).
    if(!load_browsing_files_)
    {
        TriggerNewGranularPreset();
        return;
    }
    if(granular_preset_cursor_ < 0
       || granular_preset_cursor_ >= granular_preset_user_slot_count_)
        return;
    int slot = granular_preset_user_slots_[granular_preset_cursor_];

    GranularEngine::GranularPresetData preset;
    size_t audio_len = 0;
    g_progress_disp = disp_;
    // Loaded audio lands in the same owned capture buffer Direct Record/
    // From-Layer already use -- whichever source most recently filled it
    // is exactly what SetSource() should be pointing at.
    bool ok = PerformanceStore::LoadGranularPreset(
        slot, &preset, granular_capture_buf_l_, granular_capture_buf_r_,
        granular_capture_capacity_, &audio_len, &Ui::OnSaveLoadProgress);
    g_progress_disp = nullptr;

    if(ok)
    {
        granular_->ApplyPreset(preset);
        granular_->SetSource(granular_capture_buf_l_, granular_capture_buf_r_, audio_len);
        OnNewGranularCapture(audio_len);
        granular_loaded_preset_slot_ = slot;
        snprintf(granular_preset_status_, sizeof(granular_preset_status_), "Loaded %d", slot);
    }
    else
    {
        snprintf(granular_preset_status_, sizeof(granular_preset_status_), "Fail:%s",
                  PerformanceStore::GetLastError());
    }
}

void Ui::RefreshDexedPresetSlots()
{
    dexed_preset_user_slot_count_
        = PerformanceStore::ListDexedPresets(dexed_preset_user_slots_, kMaxDexedPresetSlots);
    int folder_count = dexed_preset_folder_cursor_ < DexedSynth::kNumFactoryCategories
                            ? DexedSynth::GetFactoryCategoryCount(dexed_preset_folder_cursor_)
                            : dexed_preset_user_slot_count_;
    if(dexed_preset_cursor_ >= folder_count)
        dexed_preset_cursor_ = folder_count > 0 ? folder_count - 1 : 0;
    dexed_preset_slots_dirty_ = false;
}

int Ui::ResolveDexedPresetSlot() const
{
    if(dexed_preset_folder_cursor_ < DexedSynth::kNumFactoryCategories)
        return DexedSynth::GetFactoryCategorySlot(dexed_preset_folder_cursor_, dexed_preset_cursor_);
    if(dexed_preset_cursor_ < 0 || dexed_preset_cursor_ >= dexed_preset_user_slot_count_)
        return -1;
    return dexed_preset_user_slots_[dexed_preset_cursor_];
}

void Ui::TriggerSaveDexedPreset(bool force_new)
{
    if(!dexed_)
        return;
    int slot = (!force_new && dexed_loaded_preset_slot_ > DexedSynth::GetNumFactoryPresets())
                   ? dexed_loaded_preset_slot_
                   : PerformanceStore::NextFreeDexedPresetSlot();
    if(slot < 0)
    {
        snprintf(dexed_preset_status_, sizeof(dexed_preset_status_), "Card full/missing");
        return;
    }
    bool ok = PerformanceStore::SaveDexedPreset(slot, dexed_->CapturePreset());
    if(ok)
    {
        dexed_loaded_preset_slot_ = slot;
        snprintf(dexed_preset_status_, sizeof(dexed_preset_status_), "Saved %d", slot);
        dexed_preset_slots_dirty_ = true; // a new slot may now exist
    }
    else
    {
        snprintf(dexed_preset_status_, sizeof(dexed_preset_status_), "Fail:%s",
                  PerformanceStore::GetLastError());
    }
}

void Ui::TriggerNewDexedPreset()
{
    if(!dexed_)
        return;
    dexed_->ApplyPreset(DexedSynth::GetFactoryPreset(0));
    dexed_loaded_preset_slot_ = 1;
    snprintf(dexed_preset_status_, sizeof(dexed_preset_status_), "New (%s)",
              DexedSynth::GetFactoryPresetName(0));
}

void Ui::TriggerLoadDexedPreset()
{
    if(!dexed_)
        return;
    if(!load_browsing_files_)
    {
        TriggerNewDexedPreset();
        return;
    }
    int slot = ResolveDexedPresetSlot();
    if(slot < 0)
        return;

    DexedSynth::DexedPresetData preset;
    bool ok = PerformanceStore::LoadDexedPreset(slot, &preset);
    if(ok)
    {
        dexed_->ApplyPreset(preset);
        dexed_loaded_preset_slot_ = slot;
        if(slot <= DexedSynth::GetNumFactoryPresets())
            snprintf(dexed_preset_status_, sizeof(dexed_preset_status_), "Loaded %s",
                      DexedSynth::GetFactoryPresetName(slot - 1));
        else
            snprintf(dexed_preset_status_, sizeof(dexed_preset_status_), "Loaded %d", slot);
    }
    else
    {
        snprintf(dexed_preset_status_, sizeof(dexed_preset_status_), "Fail:%s",
                  PerformanceStore::GetLastError());
    }
}

void Ui::TriggerSaveDefaults()
{
    bool ok = PerformanceStore::SavePrefs(*tempo_, master_volume01_, bypass_,
                                            master_filter_mode_, master_filter_cutoff01_,
                                            master_filter_res01_, reverb_size01_,
                                            bypass_reverb_send01_);
    if(ok)
        snprintf(tempo_status_, sizeof(tempo_status_), "Saved as default");
    else
        snprintf(tempo_status_, sizeof(tempo_status_), "Fail:%s", PerformanceStore::GetLastError());
}

void Ui::TriggerExport()
{
    // ExportWav() drives every layer's real Process() an extra time from
    // the main loop to render the mix -- must not race the live ISR
    // doing the same on the same objects, same reasoning as TriggerLoad().
    g_audio_suspended       = true;
    g_progress_disp         = disp_;
    export_op_in_progress_  = true;
    bool ok = PerformanceStore::ExportWav(*tempo_, layers_, num_layers_, master_filter_mode_,
                                            master_filter_cutoff01_, master_filter_res01_,
                                            reverb_size01_, /*for_microdexed=*/false,
                                            project_speed_, &Ui::OnSaveLoadProgress);
    export_op_in_progress_  = false;
    g_progress_disp         = nullptr;
    g_audio_suspended       = false;

    if(ok)
        snprintf(export_status_, sizeof(export_status_), "Exported WAV");
    else
        snprintf(export_status_, sizeof(export_status_), "Fail:%s",
                  PerformanceStore::GetLastError());
}

void Ui::TriggerExportMicroDexed()
{
    // Same reasoning as TriggerExport() above -- must not race the live
    // audio ISR while this drives an extra offline render pass.
    g_audio_suspended       = true;
    g_progress_disp         = disp_;
    export_op_in_progress_  = true;
    bool ok = PerformanceStore::ExportWav(*tempo_, layers_, num_layers_, master_filter_mode_,
                                            master_filter_cutoff01_, master_filter_res01_,
                                            reverb_size01_, /*for_microdexed=*/true,
                                            project_speed_, &Ui::OnSaveLoadProgress);
    export_op_in_progress_  = false;
    g_progress_disp         = nullptr;
    g_audio_suspended       = false;

    if(ok)
        snprintf(export_status_, sizeof(export_status_), "Exported 44.1k");
    else
        snprintf(export_status_, sizeof(export_status_), "Fail:%s",
                  PerformanceStore::GetLastError());
}

void Ui::OnSaveLoadProgress(float progress01)
{
    if(!g_progress_disp)
        return;
    // Throttled: a full 4-layer stereo loop streams in ~4096-sample
    // chunks, and redrawing the (blocking I2C) OLED on every single
    // chunk would noticeably slow the transfer down for no visible
    // benefit -- a few redraws a second is plenty for a progress bar.
    static uint32_t call_count = 0;
    call_count++;
    if(call_count % 8 != 0 && progress01 < 0.999f)
        return;

    OneBitGraphicsDisplay* d = g_progress_disp;
    d->Fill(false);
    d->SetCursor(0, 14);
    // Font_7x10, not the footer's usual Font_6x8 -- bigger, more visible
    // for a modal progress overlay. Uppercase like everything else drawn
    // with Font_6x8 (see Ui::WriteUpper()) -- this one's Font_7x10 and a
    // literal, not routed through that helper, so it's just spelled
    // uppercase directly here instead.
    d->WriteString("WORKING...", Font_7x10, true);
    int w = (int)(Clampf(progress01, 0.f, 1.f) * (d->Width() - 2));
    d->DrawRect(0, 32, d->Width() - 1, 40, true, false);
    if(w > 0)
        d->DrawRect(1, 33, w, 39, true, true);
    d->Update();
}
