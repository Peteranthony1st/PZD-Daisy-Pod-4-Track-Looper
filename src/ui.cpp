#include "ui.h"
#include "performance_store.h"
#include "audio_engine.h"
#include "pad_synth.h"
#include "granular_engine.h"
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
const char* ModDestName(PadSynth::ModDestination d)
{
    switch(d)
    {
        case PadSynth::ModDestination::Vibrato: return "Vibrato";
        case PadSynth::ModDestination::FilterCutoff: return "Filter";
        case PadSynth::ModDestination::ChorusDepth: return "Chorus";
        default: return "?";
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

// Maps a browsable list index (0..kNumFactoryPresets-1 = factory,
// beyond that = user slots) to the actual slot number
// LoadPadPreset()/SavePadPreset() expect.
int PadPresetCursorToSlot(int cursor, const int* user_slots, int user_count)
{
    if(cursor < PadSynth::kNumFactoryPresets)
        return cursor + 1;
    int ui2 = cursor - PadSynth::kNumFactoryPresets;
    if(ui2 < 0 || ui2 >= user_count)
        return -1;
    return user_slots[ui2];
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
              volatile size_t*              granular_capture_write_pos)
{
    pod_        = pod;
    disp_       = display;
    tempo_      = tempo;
    layers_     = layers;
    num_layers_ = num_layers;
    pad_synth_          = pad_synth;
    pad_scope_buf_      = pad_scope_buf;
    pad_scope_capacity_ = pad_scope_capacity;
    granular_           = granular;
    granular_scope_buf_      = granular_scope_buf;
    granular_scope_capacity_ = granular_scope_capacity;
    granular_capture_buf_l_     = granular_capture_buf_l;
    granular_capture_buf_r_     = granular_capture_buf_r;
    granular_capture_capacity_  = granular_capture_capacity;
    granular_capturing_         = granular_capturing;
    granular_capture_write_pos_ = granular_capture_write_pos;

    // Same curve ApplyKnobs() uses for knob1 on Home, applied once here
    // so master_volume_ actually matches master_volume01_'s starting
    // value instead of carrying its own separate hardcoded default.
    master_volume_ = powf(master_volume01_, 2.5f) * 1.43f;
    if(master_volume_ < 0.f)
        master_volume_ = 0.f;

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

void Ui::Update(const UiControlEvents& events)
{
    HandleEncoder(events);
    HandleButton1(events);
    HandleButton2(events);
    ApplyKnobs();

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
            global_page_ = new_page;
        }
        else if(screen_ == Screen::Pad)
        {
            int n = (int)PadParamPage::kCount;
            int p = (((int)pad_param_page_ + inc) % n + n) % n;
            PadParamPage new_pad_page = (PadParamPage)p;
            if(new_pad_page == PadParamPage::Preset && pad_param_page_ != PadParamPage::Preset)
                pad_preset_slots_dirty_ = true; // re-scan the card on entry
            pad_param_page_ = new_pad_page;
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
                && global_page_ == GlobalPage::Pad)
        {
            // Global:Pad is an entry point into Screen::Pad, same
            // convention Screen::Granular's own Global page used --
            // takes priority over every other Global page's click
            // (TogglePauseAll()) since there's nothing else useful for
            // this specific page's click to do.
            screen_ = Screen::Pad;
        }
        else if(!encoder_long_fired_ && screen_ == Screen::Global
                && global_page_ == GlobalPage::Granular)
        {
            // Global:Granular is an entry point into Screen::Granular,
            // same convention Global:Pad's own click uses.
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
        else if(!encoder_long_fired_ && screen_ == Screen::Global)
        {
            TogglePauseAll();
        }
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
    else if(screen_ == Screen::Global && global_page_ == GlobalPage::File)
    {
        if(b.Pressed() && b.TimeHeldMs() > 800.f && !button2_long_fired_
           && file_slot_count_ > 0)
        {
            button2_long_fired_ = true;
            TriggerLoad();
        }
        if(events.btn2_released)
        {
            if(!button2_long_fired_ && events.btn2_held_ms > 800.f && file_slot_count_ > 0)
                TriggerLoad();
            else if(!button2_long_fired_)
            {
                // Short tap: explicit "save as new", alongside Button1's
                // own short-tap "smart save" -- see TriggerSave()'s
                // force_new doc comment for why this needs no
                // hold-to-confirm (never overwrites/destroys anything).
                if(!PerformanceStore::IsCardPresent())
                    PerformanceStore::Remount();
                else
                    TriggerSave(/*force_new=*/true);
            }
            button2_long_fired_ = false;
        }
    }
    else if(screen_ == Screen::Pad && pad_param_page_ == PadParamPage::Preset)
    {
        if(b.Pressed() && b.TimeHeldMs() > 800.f && !button2_long_fired_)
        {
            button2_long_fired_ = true;
            TriggerLoadPadPreset();
        }
        if(events.btn2_released)
        {
            if(!button2_long_fired_ && events.btn2_held_ms > 800.f)
                TriggerLoadPadPreset();
            else if(!button2_long_fired_)
            {
                if(!PerformanceStore::IsCardPresent())
                    PerformanceStore::Remount();
                else
                    TriggerSavePadPreset(/*force_new=*/true);
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
        if(b.Pressed() && b.TimeHeldMs() > 800.f && !button2_long_fired_)
        {
            button2_long_fired_ = true;
            TriggerLoadGranularPreset();
        }
        if(events.btn2_released)
        {
            if(!button2_long_fired_ && events.btn2_held_ms > 800.f)
                TriggerLoadGranularPreset();
            else if(!button2_long_fired_)
            {
                if(!PerformanceStore::IsCardPresent())
                    PerformanceStore::Remount();
                else
                    TriggerSaveGranularPreset(/*force_new=*/true);
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
                // No card: this hand-wired socket has no card-detect pin
                // (see PerformanceStore::Remount()'s doc comment), so a
                // card swapped out and back in needs an explicit re-mount
                // attempt -- repurpose Button1 for that instead of Save
                // while there's nothing to save to anyway.
                if(!PerformanceStore::IsCardPresent())
                {
                    PerformanceStore::Remount();
                    file_slots_dirty_ = true; // re-scan once actually mounted
                }
                else
                    TriggerSave();
            }
            else if(global_page_ == GlobalPage::Export)
            {
                if(!PerformanceStore::IsCardPresent())
                    PerformanceStore::Remount();
                else
                    TriggerExport();
            }
            else if(global_page_ == GlobalPage::Pad)
                pad_enabled_ = !pad_enabled_;
            else if(global_page_ == GlobalPage::Granular)
                granular_enabled_ = !granular_enabled_;
            else if(global_page_ == GlobalPage::Looper)
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
            else if(global_page_ == GlobalPage::Mixer)
                global_mixer_target_reverb_ = false; // knobs -> Output Level
            break;
        case Screen::Pad:
            if(pad_param_page_ == PadParamPage::ADSR)
            {
                pad_adsr_target_sr_ = false; // knobs -> Attack/Decay
            }
            else if(pad_param_page_ == PadParamPage::Filter && pad_synth_)
            {
                // Same "Button1 cycles" idiom as Layer:Filter/Global:Filter.
                int n = (int)FilterMode::kNumModes;
                int m = ((int)pad_synth_->GetFilterMode() + 1) % n;
                pad_synth_->SetFilterMode((FilterMode)m);
            }
            else if(pad_param_page_ == PadParamPage::ModAssign && pad_synth_)
            {
                int n = (int)PadSynth::ModDestination::ChorusDepth + 1;
                int m = ((int)pad_synth_->GetModDestination() + 1) % n;
                pad_synth_->SetModDestination((PadSynth::ModDestination)m);
            }
            else if(pad_param_page_ == PadParamPage::Preset)
            {
                if(!PerformanceStore::IsCardPresent())
                    PerformanceStore::Remount();
                else
                    TriggerSavePadPreset(false);
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
                // num_layers_ -> Direct Record.
                granular_capture_source_++;
                if(granular_capture_source_ >= num_layers_)
                    granular_capture_source_ = -1;
                granular_capture_status_[0] = '\0'; // stale result from the other source
            }
            else if(granular_param_page_ == GranularParamPage::Preset)
            {
                if(!PerformanceStore::IsCardPresent())
                    PerformanceStore::Remount();
                else
                    TriggerSaveGranularPreset();
            }
            break;
    }
}

void Ui::OnButton1Long()
{
    // Long-press is only meaningful on the transport (arm to record /
    // start overdub) and, on Global:File, as the hold-to-confirm gesture
    // for starting a new performance -- everywhere else a long hold does
    // nothing extra.
    if(screen_ == Screen::Home)
        layers_[cursor_layer_].OnRecordButtonLongPress(*tempo_);
    else if(screen_ == Screen::Layer && layer_page_ == LayerPage::Status)
        Cur().OnRecordButtonLongPress(*tempo_);
    else if(screen_ == Screen::Global && global_page_ == GlobalPage::File)
        TriggerNew();
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
    else if(screen_ == Screen::Pad && pad_param_page_ == PadParamPage::ADSR)
        pad_adsr_target_sr_ = true; // knobs -> Sustain/Release
    else if(screen_ == Screen::Granular && granular_param_page_ == GranularParamPage::Grain)
        granular_grain_target_gap_scan_ = true; // knobs -> Gap+Scan
    else if(screen_ == Screen::Granular && granular_param_page_ == GranularParamPage::ADSR)
        granular_adsr_target_sr_ = true; // knobs -> Sustain/Release
    else if(screen_ == Screen::Granular && granular_param_page_ == GranularParamPage::Mix)
        granular_mix_target_reverb_ = true; // knobs -> Reverb Send
    else if(screen_ == Screen::Global && global_page_ == GlobalPage::Mixer)
        global_mixer_target_reverb_ = true; // knobs -> Reverb Send
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
                case GlobalPage::Pad: return KnobContext::GlobalPad;
                case GlobalPage::Granular: return KnobContext::GlobalGranular;
                case GlobalPage::Looper: return KnobContext::GlobalLooper;
                case GlobalPage::Mixer:
                    return global_mixer_target_reverb_ ? KnobContext::GlobalMixerReverb
                                                          : KnobContext::GlobalMixer;
                default: return KnobContext::GlobalTempo;
            }
        case Screen::Pad:
            switch(pad_param_page_)
            {
                case PadParamPage::Tone: return KnobContext::PadTone;
                case PadParamPage::ADSR:
                    return pad_adsr_target_sr_ ? KnobContext::PadEnvSR : KnobContext::PadEnvAD;
                case PadParamPage::Chorus: return KnobContext::PadChorus;
                case PadParamPage::Vibrato: return KnobContext::PadVibrato;
                case PadParamPage::Filter: return KnobContext::PadFilter;
                case PadParamPage::Mix: return KnobContext::PadMix;
                case PadParamPage::ModAssign: return KnobContext::PadModAssign;
                case PadParamPage::Preset: return KnobContext::PadPreset;
                default: return KnobContext::PadTone;
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
        case KnobContext::GlobalPad: break; // entry point only -- see Screen::Pad instead
        case KnobContext::GlobalGranular: break; // entry point only -- see Screen::Granular instead
        case KnobContext::GlobalLooper: break; // no continuous knobs, Button1 toggle only
        case KnobContext::GlobalMixer:
            k1_pickup_raw_[i] = pad_synth_ ? pad_synth_->GetOutputLevel01() : 0.f;
            k2_pickup_raw_[i] = granular_ ? granular_->GetOutputLevel01() : 0.f;
            break;
        case KnobContext::GlobalMixerReverb:
            k1_pickup_raw_[i] = pad_synth_ ? pad_synth_->GetReverbSend01() : 0.f;
            k2_pickup_raw_[i] = granular_ ? granular_->GetReverbSend01() : 0.f;
            break;
        case KnobContext::PadTone:
            if(pad_synth_)
            {
                k1_pickup_raw_[i] = pad_synth_->GetRegistration01();
                k2_pickup_raw_[i] = pad_synth_->GetOscGain01();
            }
            break;
        case KnobContext::PadEnvAD:
            if(pad_synth_)
            {
                k1_pickup_raw_[i] = pad_synth_->GetAttack01();
                k2_pickup_raw_[i] = pad_synth_->GetDecay01();
            }
            break;
        case KnobContext::PadEnvSR:
            if(pad_synth_)
            {
                k1_pickup_raw_[i] = pad_synth_->GetSustain01();
                k2_pickup_raw_[i] = pad_synth_->GetRelease01();
            }
            break;
        case KnobContext::PadChorus:
            if(pad_synth_)
            {
                k1_pickup_raw_[i] = pad_synth_->GetChorusDepth01();
                k2_pickup_raw_[i] = pad_synth_->GetChorusRate01();
            }
            break;
        case KnobContext::PadVibrato:
            if(pad_synth_)
            {
                k1_pickup_raw_[i] = pad_synth_->GetVibratoDepth01();
                k2_pickup_raw_[i] = pad_synth_->GetVibratoRate01();
            }
            break;
        case KnobContext::PadFilter:
            if(pad_synth_)
            {
                k1_pickup_raw_[i] = pad_synth_->GetFilterCutoff01();
                k2_pickup_raw_[i] = pad_synth_->GetFilterResonance01();
            }
            break;
        case KnobContext::PadMix:
            if(pad_synth_)
            {
                k1_pickup_raw_[i] = pad_synth_->GetReverbSend01();
                k2_pickup_raw_[i] = pad_synth_->GetOutputLevel01();
            }
            break;
        case KnobContext::PadModAssign: break; // no continuous knob values, Button1 cycles it
        case KnobContext::PadPreset: break; // browses a list directly, no pickup used
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
                k1_pickup_raw_[i] = granular_->GetPosition01();
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
        default: break;
    }
}

bool Ui::KnobPickUp(float raw, float& stored_raw, bool& engaged)
{
    constexpr float kKnobPickupEpsilon = 0.02f;
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
                // Knob1 browses the list of existing saved slots (the
                // Load target) -- discretized, not a pickup-tracked
                // continuous value, since it's selecting one of a small
                // number of list items rather than dialing a parameter.
                if(file_slot_count_ > 0)
                {
                    int idx = (int)(Clampf(k1, 0.f, 1.f) * file_slot_count_);
                    if(idx >= file_slot_count_)
                        idx = file_slot_count_ - 1;
                    file_cursor_ = idx;
                }
            }
            else if(global_page_ == GlobalPage::Mixer)
            {
                if(!global_mixer_target_reverb_)
                {
                    if(pad_synth_ && KnobPickUp(k1, k1_pickup_raw_[ci], k1_pickup_engaged_[ci]))
                        pad_synth_->SetOutputLevel01(k1);
                    if(granular_ && KnobPickUp(k2, k2_pickup_raw_[ci], k2_pickup_engaged_[ci]))
                        granular_->SetOutputLevel01(k2);
                }
                else
                {
                    if(pad_synth_ && KnobPickUp(k1, k1_pickup_raw_[ci], k1_pickup_engaged_[ci]))
                        pad_synth_->SetReverbSend01(k1);
                    if(granular_ && KnobPickUp(k2, k2_pickup_raw_[ci], k2_pickup_engaged_[ci]))
                        granular_->SetReverbSend01(k2);
                }
            }
            break;

        case Screen::Pad:
            if(!pad_synth_)
                break;
            switch(pad_param_page_)
            {
                case PadParamPage::Tone:
                    if(KnobPickUp(k1, k1_pickup_raw_[ci], k1_pickup_engaged_[ci]))
                        pad_synth_->SetRegistration01(k1);
                    if(KnobPickUp(k2, k2_pickup_raw_[ci], k2_pickup_engaged_[ci]))
                        pad_synth_->SetOscGain01(k2);
                    break;
                case PadParamPage::ADSR:
                    if(!pad_adsr_target_sr_)
                    {
                        if(KnobPickUp(k1, k1_pickup_raw_[ci], k1_pickup_engaged_[ci]))
                            pad_synth_->SetAttack01(k1);
                        if(KnobPickUp(k2, k2_pickup_raw_[ci], k2_pickup_engaged_[ci]))
                            pad_synth_->SetDecay01(k2);
                    }
                    else
                    {
                        if(KnobPickUp(k1, k1_pickup_raw_[ci], k1_pickup_engaged_[ci]))
                            pad_synth_->SetSustain01(k1);
                        if(KnobPickUp(k2, k2_pickup_raw_[ci], k2_pickup_engaged_[ci]))
                            pad_synth_->SetRelease01(k2);
                    }
                    break;
                case PadParamPage::Chorus:
                    if(KnobPickUp(k1, k1_pickup_raw_[ci], k1_pickup_engaged_[ci]))
                        pad_synth_->SetChorusDepth01(k1);
                    if(KnobPickUp(k2, k2_pickup_raw_[ci], k2_pickup_engaged_[ci]))
                        pad_synth_->SetChorusRate01(k2);
                    break;
                case PadParamPage::Vibrato:
                    if(KnobPickUp(k1, k1_pickup_raw_[ci], k1_pickup_engaged_[ci]))
                        pad_synth_->SetVibratoDepth01(k1);
                    if(KnobPickUp(k2, k2_pickup_raw_[ci], k2_pickup_engaged_[ci]))
                        pad_synth_->SetVibratoRate01(k2);
                    break;
                case PadParamPage::Filter:
                    if(KnobPickUp(k1, k1_pickup_raw_[ci], k1_pickup_engaged_[ci]))
                        pad_synth_->SetFilterCutoff01(k1);
                    if(KnobPickUp(k2, k2_pickup_raw_[ci], k2_pickup_engaged_[ci]))
                        pad_synth_->SetFilterResonance01(k2);
                    break;
                case PadParamPage::Mix:
                    if(KnobPickUp(k1, k1_pickup_raw_[ci], k1_pickup_engaged_[ci]))
                        pad_synth_->SetReverbSend01(k1);
                    if(KnobPickUp(k2, k2_pickup_raw_[ci], k2_pickup_engaged_[ci]))
                        pad_synth_->SetOutputLevel01(k2);
                    break;
                case PadParamPage::ModAssign: break; // no knobs, Button1 cycles it
                case PadParamPage::Preset:
                {
                    // Knob1 browses the whole list directly (factory
                    // presets first, then user slots) -- discretized, not
                    // pickup-tracked, same idiom as Global:File's
                    // file_cursor_.
                    int total = PadSynth::kNumFactoryPresets + pad_preset_user_slot_count_;
                    if(total > 0)
                    {
                        int idx = (int)(Clampf(k1, 0.f, 1.f) * total);
                        if(idx >= total)
                            idx = total - 1;
                        // Browsing again -- clear the last save/load
                        // result so the "Load:" line (which shows what
                        // the knob is actually pointing at right now)
                        // comes back instead of staying stuck on a
                        // status message that never otherwise clears.
                        if(idx != pad_preset_cursor_)
                            pad_preset_status_[0] = '\0';
                        pad_preset_cursor_ = idx;
                    }
                    break;
                }
                default: break;
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
                    // Knob1 browses existing saves directly -- discretized,
                    // not pickup-tracked, same idiom as Global:File's own
                    // file_cursor_ (no factory range here to fold in,
                    // unlike Pad Preset's PadPresetCursorToSlot()).
                    int total = granular_preset_user_slot_count_;
                    if(total > 0)
                    {
                        int idx = (int)(Clampf(k1, 0.f, 1.f) * total);
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
        case Screen::Pad: DrawPadScreen(); break;
        case Screen::Granular: DrawGranularScreen(); break;
    }
    // TEMPORARY -- CPU diagnostic overlay (see SetDiagCpuPercent()'s doc
    // comment). Drawn last, on top of whatever the screen above just
    // drew, with an opaque background so it's always legible regardless
    // of what's underneath -- top-right corner, may cosmetically cover
    // the last beat-indicator dot on screens that use one there. Part of
    // the same Draw()/Update() pass as everything else, so it persists
    // exactly as long as any other on-screen content instead of getting
    // wiped by the next regular redraw a few ms later.
    if(diag_cpu_percent_ >= 0)
    {
        char diag[8];
        snprintf(diag, sizeof(diag), "%d%%", diag_cpu_percent_);
        int w = (int)strlen(diag) * 6; // Font_6x8
        int x = disp_->Width() - w;
        disp_->DrawRect(x, 0, disp_->Width() - 1, 7, false, true);
        disp_->SetCursor(x, 0);
        disp_->WriteString(diag, Font_6x8, true);
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
    if(global_page_ == GlobalPage::Pad)
    {
        // Entry point into Screen::Pad, plus the on/off toggle (see
        // IsPadEnabled()'s doc comment for why this was pulled forward
        // from the original plan's Stage 4).
        disp_->SetCursor(0, 0);
        WriteUpper("Global:Plaits");
        DrawBeatIndicator(disp_->Width() - 41, 0, 3);
        disp_->DrawLine(0, 9, disp_->Width() - 1, 9, true);
        disp_->SetCursor(0, 20);
        WriteUpper(pad_enabled_ ? "Enabled" : "Disabled");
        disp_->SetCursor(0, 30);
        WriteUpper("Click to open Plaits");
        DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "", "", "", "");
        DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, "Toggle On/Off", "", "", "");
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
        // All four values always shown (same "graph shows everything,
        // buttons only change what the knobs reach" idiom as Pad's own
        // ADSR page and Grains' Mix page) -- Output Level and Reverb
        // Send for both engines, grouped by parameter so the active pair
        // (Button1=Level, Button2=Reverb) reads as two adjacent bars.
        // Same underlying pad_synth_/granular_ values as each engine's
        // own Mix page, so changing it here updates there too.
        disp_->SetCursor(0, 0);
        WriteUpper("Global:Mixer");
        DrawBeatIndicator(disp_->Width() - 41, 0, 3);
        disp_->DrawLine(0, 9, disp_->Width() - 1, 9, true);

        const char* mix_labels[4] = {"P.Level", "G.Level", "P.Reverb", "G.Reverb"};
        float       mix_vals01[4] = {pad_synth_ ? pad_synth_->GetOutputLevel01() : 0.f,
                                      granular_ ? granular_->GetOutputLevel01() : 0.f,
                                      pad_synth_ ? pad_synth_->GetReverbSend01() : 0.f,
                                      granular_ ? granular_->GetReverbSend01() : 0.f};
        char        mix_values[4][8];
        for(int i = 0; i < 4; i++)
            snprintf(mix_values[i], sizeof(mix_values[i]), "%d%%",
                      (int)(mix_vals01[i] * 100.f + 0.5f));

        const int kMixLabelBaseline = 15;
        const int kMixValueBaseline = 21;
        const int kMixColWidth      = disp_->Width() / 4;
        const int kBarWidth = 26, kBarHeight = 6, kBarTop = 25;
        for(int i = 0; i < 4; i++)
        {
            int center_x = kMixColWidth * i + kMixColWidth / 2;
            int lw       = TomThumbAdvanceWidth(mix_labels[i]);
            TomThumbDrawText(disp_, center_x - lw / 2, kMixLabelBaseline, mix_labels[i], true);
            int vw = TomThumbAdvanceWidth(mix_values[i]);
            TomThumbDrawText(disp_, center_x - vw / 2, kMixValueBaseline, mix_values[i], true);

            int bar_x0 = center_x - kBarWidth / 2;
            int bar_x1 = bar_x0 + kBarWidth - 1;
            disp_->DrawRect(bar_x0, kBarTop, bar_x1, kBarTop + kBarHeight - 1, true, false);
            int fill_w = (int)(Clampf(mix_vals01[i], 0.f, 1.f) * (float)(kBarWidth - 2) + 0.5f);
            if(fill_w > 0)
                disp_->DrawRect(bar_x0 + 1, kBarTop + 1, bar_x0 + fill_w, kBarTop + kBarHeight - 2,
                                  true, true);
        }

        if(!global_mixer_target_reverb_)
            DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "Plaits", mix_values[0],
                             mix_values[1], "Grains");
        else
            DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "Plaits", mix_values[2],
                             mix_values[3], "Grains");
        DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY,
                         global_mixer_target_reverb_ ? "Level" : "Level*", "", "",
                         global_mixer_target_reverb_ ? "Reverb*" : "Reverb");
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
        disp_->SetCursor(0, 20);
        WriteUpper("No SD card");
        DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "", "", "", "");
        DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, "Retry", "", "", "");
        return;
    }

    if(file_slots_dirty_)
        RefreshFileSlots();

    // Whichever button is actually held wins the space (hold-to-confirm
    // progress, same idea as the Status page's Clear); otherwise this
    // area shows the current/browse-target slots and the last result.
    if(pod_->button1.Pressed())
    {
        float held = pod_->button1.TimeHeldMs();
        int   w    = (int)(Clampf(held / 400.f, 0.f, 1.f) * (disp_->Width() - 2));
        disp_->SetCursor(0, 20);
        WriteUpper("Hold: New...");
        disp_->DrawRect(0, 30, disp_->Width() - 1, 34, true, false);
        if(w > 0)
            disp_->DrawRect(1, 31, w, 33, true, true);
    }
    else if(pod_->button2.Pressed() && file_slot_count_ > 0)
    {
        float held = pod_->button2.TimeHeldMs();
        int   w    = (int)(Clampf(held / 800.f, 0.f, 1.f) * (disp_->Width() - 2));
        disp_->SetCursor(0, 20);
        WriteUpper("Hold: Load...");
        disp_->DrawRect(0, 30, disp_->Width() - 1, 34, true, false);
        if(w > 0)
            disp_->DrawRect(1, 31, w, 33, true, true);
    }
    else
    {
        char line1[32], line2[32];
        if(loaded_slot_ >= 0)
            snprintf(line1, sizeof(line1), "Now: %d - Perf", loaded_slot_);
        else
            snprintf(line1, sizeof(line1), "Now: (unsaved)");
        disp_->SetCursor(0, 14);
        WriteUpper(line1);

        if(file_slot_count_ > 0)
            snprintf(line2, sizeof(line2), "Load: %d - Perf",
                      file_slots_[file_cursor_]);
        else
            snprintf(line2, sizeof(line2), "Load: (no saves)");
        disp_->SetCursor(0, 26);
        WriteUpper(line2);

        if(file_status_[0] != '\0')
        {
            disp_->SetCursor(0, 36);
            WriteUpper(file_status_);
        }
    }

    // Knob1 browses a discrete list of slots (not a live 0..1 parameter)
    // -- show the slot number it's currently on, same index shown in the
    // "Load: N - Performance" line above. Empty when there's nothing to
    // browse (no saves yet), same "nothing if it does nothing" rule as an
    // idle knob elsewhere. Button1's label spells out its hold behaviour
    // explicitly; Button2's doesn't repeat "Load" as a tap meaning since
    // it doesn't have one here (see Global:Tempo's Button2 for where
    // that idle-tap slot went instead) -- the "Hold: New.../Hold:
    // Load..." progress-bar overlay (shown once you actually hold either
    // one) is still the fallback discovery path either way.
    char load_val[8] = "";
    if(file_slot_count_ > 0)
        snprintf(load_val, sizeof(load_val), "%d", file_slots_[file_cursor_]);
    DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "Load", load_val, "", "");
    // "Copy", not "New" -- Button1's own Hold=New already means something
    // different (wipes the performance); this just saves an additional,
    // separate slot without touching what's currently loaded.
    DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, "Save/Hold=New", "", "",
                     "Copy/Hold=Load");
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

void Ui::DrawPadScreen()
{
    if(!pad_synth_)
        return;

    const char* page_name = "Tone";
    switch(pad_param_page_)
    {
        case PadParamPage::Tone: page_name = "Tone"; break;
        case PadParamPage::ADSR: page_name = "ADSR"; break;
        case PadParamPage::Chorus: page_name = "Chorus"; break;
        case PadParamPage::Vibrato: page_name = "Vibrato"; break;
        case PadParamPage::Filter: page_name = "Filter"; break;
        case PadParamPage::Mix: page_name = "Mix"; break;
        case PadParamPage::ModAssign: page_name = "Mod"; break;
        case PadParamPage::Preset: page_name = "Preset"; break;
        default: break;
    }
    char title[24];
    snprintf(title, sizeof(title), "Plaits:%s", page_name);
    disp_->SetCursor(0, 0);
    WriteUpper(title);
    DrawBeatIndicator(disp_->Width() - 41, 0, 3);
    disp_->DrawLine(0, 9, disp_->Width() - 1, 9, true);

    // Persistent status row, shown on every Pad page regardless of which
    // one is selected -- 8-voice polyphony dots, a pitch-bend position
    // tick (center = no bend), and the mod wheel's live value + whichever
    // destination it's currently assigned to. Always visible here so
    // it's never ambiguous what the wheel is doing, not just when you
    // happen to be on the ModAssign page.
    const int kStatusY = 11;
    for(int i = 0; i < PadSynth::kMaxVoices; i++)
    {
        int x = 1 + i * 3;
        disp_->DrawRect(x, kStatusY, x + 1, kStatusY + 1, true, pad_synth_->IsVoiceActive(i));
    }

    const int kBendX0 = 30, kBendX1 = 62, kBendMidY = kStatusY + 1;
    disp_->DrawLine(kBendX0, kBendMidY, kBendX1, kBendMidY, true);
    {
        // +-2 semitones (see main.cpp's kPitchBendRangeSemis) maps to
        // the full width of this little bar, centered = no bend.
        float bend01 = Clampf(pad_synth_->GetPitchBendSemis() / 2.f, -1.f, 1.f);
        int   tick_x = kBendX0 + (kBendX1 - kBendX0) / 2
                       + (int)(bend01 * (float)(kBendX1 - kBendX0) / 2.f);
        disp_->DrawLine(tick_x, kStatusY, tick_x, kStatusY + 2, true);
    }

    // Font_6x8 text is a full 8px tall -- drawn on its own row below the
    // dots/tick (which are only 1-3px), with kBandTop pushed down enough
    // to give it real clearance, not sharing a cramped few-px band with
    // whatever the current page draws next (that's what caused pages to
    // visibly draw over this earlier).
    const int kModRowY = 13;
    char      mod_line[16];
    snprintf(mod_line, sizeof(mod_line), "%.4s%d%%", ModDestName(pad_synth_->GetModDestination()),
              (int)(pad_synth_->GetModWheel01() * 100.f + 0.5f));
    disp_->SetCursor(66, kModRowY);
    WriteUpper(mod_line);

    const int kBandTop = 22, kBandBottom = 44;
    switch(pad_param_page_)
    {
        case PadParamPage::Tone:
        {
            DrawOscilloscope(kBandTop, kBandBottom, pad_scope_buf_, pad_scope_capacity_);
            char reg_val[8], gain_val[8];
            snprintf(reg_val, sizeof(reg_val), "%d%%",
                      (int)(pad_synth_->GetRegistration01() * 100.f + 0.5f));
            snprintf(gain_val, sizeof(gain_val), "%d%%",
                      (int)(pad_synth_->GetOscGain01() * 100.f + 0.5f));
            DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "Tone", reg_val, gain_val,
                             "Gain");
            DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, "", "", "", "");
            break;
        }
        case PadParamPage::ADSR:
        {
            // All four stages always shown together, regardless of which
            // pair the knobs currently target -- Button1/Button2 only
            // change what the KNOBS reach, never what the graph shows.
            DrawAdsrShape(kBandTop, kBandBottom, pad_synth_->GetAttackSeconds(),
                          pad_synth_->GetDecaySeconds(), pad_synth_->GetSustain01(),
                          pad_synth_->GetReleaseSeconds());
            if(!pad_adsr_target_sr_)
            {
                char a_val[8], d_val[8];
                snprintf(a_val, sizeof(a_val), "%d%%",
                          (int)(pad_synth_->GetAttack01() * 100.f + 0.5f));
                snprintf(d_val, sizeof(d_val), "%d%%",
                          (int)(pad_synth_->GetDecay01() * 100.f + 0.5f));
                DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "Attack", a_val, d_val,
                                 "Decay");
            }
            else
            {
                char s_val[8], r_val[8];
                snprintf(s_val, sizeof(s_val), "%d%%",
                          (int)(pad_synth_->GetSustain01() * 100.f + 0.5f));
                snprintf(r_val, sizeof(r_val), "%d%%",
                          (int)(pad_synth_->GetRelease01() * 100.f + 0.5f));
                DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "Sustain", s_val, r_val,
                                 "Release");
            }
            // Marks which pair Button1/Button2 currently map the knobs
            // to -- "*" on whichever is active, since both buttons are
            // simple taps here (no hold behavior to also describe).
            DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY,
                             pad_adsr_target_sr_ ? "AD" : "AD*", "", "",
                             pad_adsr_target_sr_ ? "SR*" : "SR");
            break;
        }
        case PadParamPage::Chorus:
        {
            DrawOscilloscope(kBandTop, kBandBottom, pad_scope_buf_, pad_scope_capacity_);
            char depth_val[8], rate_val[8];
            snprintf(depth_val, sizeof(depth_val), "%d%%",
                      (int)(pad_synth_->GetChorusDepth01() * 100.f + 0.5f));
            snprintf(rate_val, sizeof(rate_val), "%d%%",
                      (int)(pad_synth_->GetChorusRate01() * 100.f + 0.5f));
            DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "Depth", depth_val, rate_val,
                             "Rate");
            DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, "", "", "", "");
            break;
        }
        case PadParamPage::Vibrato:
        {
            // No live oscilloscope here -- the LFO is far too slow
            // (0.5-8Hz) to show anything meaningful in the scope's short
            // capture window, unlike Tone/Chorus/Filter which all shape
            // the waveform itself on audio-rate timescales.
            char mod_line[24];
            snprintf(mod_line, sizeof(mod_line), "Mod wheel -> %s",
                      ModDestName(pad_synth_->GetModDestination()));
            disp_->SetCursor(0, kBandTop);
            WriteUpper(mod_line);
            if(pad_synth_->GetModDestination() != PadSynth::ModDestination::Vibrato)
            {
                disp_->SetCursor(0, kBandTop + 12);
                WriteUpper("(wheel not on Vibrato)");
            }
            char depth_val[8], rate_val[8];
            snprintf(depth_val, sizeof(depth_val), "%d%%",
                      (int)(pad_synth_->GetVibratoDepth01() * 100.f + 0.5f));
            snprintf(rate_val, sizeof(rate_val), "%d%%",
                      (int)(pad_synth_->GetVibratoRate01() * 100.f + 0.5f));
            DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "Depth", depth_val, rate_val,
                             "Rate");
            DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, "", "", "", "");
            break;
        }
        case PadParamPage::Filter:
        {
            // Mode name gets its own line (the only way to see which
            // mode is active), oscilloscope given the remaining band
            // beneath it rather than the full kBandTop..kBandBottom span
            // Tone/Chorus get.
            char mode_line[20];
            snprintf(mode_line, sizeof(mode_line), "Mode: %s",
                      FilterModeName(pad_synth_->GetFilterMode()));
            disp_->SetCursor(0, kBandTop);
            WriteUpper(mode_line);
            DrawOscilloscope(kBandTop + 9, kBandBottom, pad_scope_buf_, pad_scope_capacity_);

            char cutoff_val[8], res_val[8];
            snprintf(cutoff_val, sizeof(cutoff_val), "%d%%",
                      (int)(pad_synth_->GetFilterCutoff01() * 100.f + 0.5f));
            snprintf(res_val, sizeof(res_val), "%d%%",
                      (int)(pad_synth_->GetFilterResonance01() * 100.f + 0.5f));
            DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "Cutoff", cutoff_val, res_val,
                             "Res");
            DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, "Cycle mode", "", "", "");
            break;
        }
        case PadParamPage::Mix:
        {
            char send_val[8], out_val[8];
            snprintf(send_val, sizeof(send_val), "%d%%",
                      (int)(pad_synth_->GetReverbSend01() * 100.f + 0.5f));
            snprintf(out_val, sizeof(out_val), "%d%%",
                      (int)(pad_synth_->GetOutputLevel01() * 100.f + 0.5f));
            DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "Send", send_val, out_val,
                             "Output");
            DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, "", "", "", "");
            break;
        }
        case PadParamPage::ModAssign:
        {
            char dest_line[24];
            snprintf(dest_line, sizeof(dest_line), "Mod Dest: %s",
                      ModDestName(pad_synth_->GetModDestination()));
            disp_->SetCursor(0, kBandTop);
            WriteUpper(dest_line);
            DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "", "", "", "");
            DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, "Cycle dest", "", "", "");
            break;
        }
        case PadParamPage::Preset:
        {
            if(!PerformanceStore::IsCardPresent())
            {
                disp_->SetCursor(0, kBandTop);
                WriteUpper("No card (factory OK)");
                DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "", "", "", "");
                DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, "Retry", "", "", "");
                break;
            }
            if(pad_preset_slots_dirty_)
                RefreshPadPresetSlots();

            // Held Button2 takes over this space with a hold-to-confirm
            // progress bar, same pattern as Global:File's Hold=Load.
            if(pod_->button2.Pressed())
            {
                float held = pod_->button2.TimeHeldMs();
                int   w    = (int)(Clampf(held / 800.f, 0.f, 1.f) * (disp_->Width() - 2));
                disp_->SetCursor(0, kBandTop);
                WriteUpper("Hold: Load...");
                disp_->DrawRect(0, kBandTop + 10, disp_->Width() - 1, kBandTop + 14, true, false);
                if(w > 0)
                    disp_->DrawRect(1, kBandTop + 11, w, kBandTop + 13, true, true);
                DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "", "", "", "");
                DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, "", "", "", "");
                break;
            }

            char line1[24], line2[24];
            if(pad_loaded_preset_slot_ <= 0)
                snprintf(line1, sizeof(line1), "Now: (custom)");
            else if(pad_loaded_preset_slot_ <= PadSynth::kNumFactoryPresets)
                snprintf(line1, sizeof(line1), "Now: %s",
                          PadSynth::GetFactoryPresetName(pad_loaded_preset_slot_ - 1));
            else
                snprintf(line1, sizeof(line1), "Now: %d", pad_loaded_preset_slot_);
            disp_->SetCursor(0, kBandTop);
            WriteUpper(line1);

            // Status (if any) takes this row's place instead of stacking
            // below it -- same "fresh result takes priority over the
            // steady-state line" convention Global:Tempo's own
            // tempo_status_/"Clear layers first" already uses. There
            // isn't room for 3 full text lines in this band any more
            // (see kBandTop's own comment above), and the status is a
            // direct response to what you just did, so it's more useful
            // than "Load:" for that brief moment.
            if(pad_preset_status_[0] != '\0')
            {
                disp_->SetCursor(0, kBandTop + 12);
                WriteUpper(pad_preset_status_);
            }
            else
            {
                int browsed_slot = PadPresetCursorToSlot(pad_preset_cursor_,
                                                            pad_preset_user_slots_,
                                                            pad_preset_user_slot_count_);
                if(browsed_slot <= 0)
                    snprintf(line2, sizeof(line2), "Load: (none)");
                else if(browsed_slot <= PadSynth::kNumFactoryPresets)
                    snprintf(line2, sizeof(line2), "Load: %s",
                              PadSynth::GetFactoryPresetName(browsed_slot - 1));
                else
                    snprintf(line2, sizeof(line2), "Load: %d", browsed_slot);
                disp_->SetCursor(0, kBandTop + 12);
                WriteUpper(line2);
            }

            // "Save" alone, not "Save/Hold=X" -- Button1 has no hold
            // action on this page (unlike Global:File's own Button1),
            // so implying one here would be misleading.
            DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "", "", "", "");
            DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, "Save", "", "",
                             "Copy/Hold=Load");
            break;
        }
        default: break;
    }
}

void Ui::DrawGranularScreen()
{
    if(!granular_)
        return;

    const char* page_name = "Grain";
    switch(granular_param_page_)
    {
        case GranularParamPage::Grain: page_name = "Grain"; break;
        case GranularParamPage::Position: page_name = "Position"; break;
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
            // so turning the Position knob visibly moves the Grain
            // marker across the sample instead of only being a number.
            char line1[24];
            snprintf(line1, sizeof(line1), "Position: %d%%",
                      (int)(granular_->GetPosition01() * 100.f + 0.5f));
            TomThumbDrawText(disp_, 0, 15, line1, true);

            DrawGranularWaveform(granular_->GetWaveformPeaks(), granular_->GetGrainAnchor01(),
                                  granular_->GetScanAnchor01(), granular_->HasSource());

            char pos_val[8];
            snprintf(pos_val, sizeof(pos_val), "%d%%",
                      (int)(granular_->GetPosition01() * 100.f + 0.5f));
            DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "Position", pos_val, "", "");
            DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, "", "", "", "");
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
            char src_line[24];
            if(granular_capture_source_ >= 0)
                snprintf(src_line, sizeof(src_line), "Source: Layer %d",
                          granular_capture_source_ + 1);
            else
                snprintf(src_line, sizeof(src_line), "Source: Direct Record");
            TomThumbDrawText(disp_, 0, 15, src_line, true);

            bool recording = granular_capture_source_ < 0 && granular_capturing_
                              && *granular_capturing_;
            if(recording)
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

            DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "", "", "", "");
            DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, "Cycle Source", "", "",
                             granular_capture_source_ >= 0 ? "Hold=Capture" : "Hold=Record");
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
                TomThumbDrawText(disp_, 0, 15, "No card", true);
                DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "", "", "", "");
                DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, "Retry", "", "", "");
                break;
            }
            if(granular_preset_slots_dirty_)
                RefreshGranularPresetSlots();

            // Held Button2 takes over this space with a hold-to-confirm
            // progress bar, same pattern as Pad Preset's own Hold=Load --
            // the actual SD transfer (once the hold fires) takes over the
            // WHOLE display via Ui::OnSaveLoadProgress() instead, since
            // it can run long enough (up to ~1.9MB of audio) to need its
            // own feedback, unlike Pad's instant tiny-struct load.
            if(pod_->button2.Pressed())
            {
                float held = pod_->button2.TimeHeldMs();
                int   w    = (int)(Clampf(held / 800.f, 0.f, 1.f) * (disp_->Width() - 2));
                TomThumbDrawText(disp_, 0, 15, "Hold: Load...", true);
                disp_->DrawRect(0, 20, disp_->Width() - 1, 24, true, false);
                if(w > 0)
                    disp_->DrawRect(1, 21, w, 23, true, true);
                DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "", "", "", "");
                DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, "", "", "", "");
                break;
            }

            char line1[24];
            if(granular_loaded_preset_slot_ <= 0)
                snprintf(line1, sizeof(line1), "Now: (custom)");
            else
                snprintf(line1, sizeof(line1), "Now: %d", granular_loaded_preset_slot_);
            TomThumbDrawText(disp_, 0, 15, line1, true);

            // Status (if any) takes this row's place instead of stacking
            // below it -- same "fresh result takes priority" convention
            // Pad Preset's own page uses.
            if(granular_preset_status_[0] != '\0')
            {
                TomThumbDrawText(disp_, 0, 22, granular_preset_status_, true);
            }
            else
            {
                char line2[24];
                if(granular_preset_cursor_ < granular_preset_user_slot_count_)
                    snprintf(line2, sizeof(line2), "Load: %d",
                              granular_preset_user_slots_[granular_preset_cursor_]);
                else
                    snprintf(line2, sizeof(line2), "Load: (none)");
                TomThumbDrawText(disp_, 0, 22, line2, true);
            }

            DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "", "", "", "");
            DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, "Save", "", "",
                             "Copy/Hold=Load");
            break;
        }
        default: break;
    }
}

void Ui::DrawAdsrShape(int top, int bottom, float attack_s, float decay_s, float sustain01,
                        float release_s)
{
    // Duplicated from PadSynth's own private range (same "duplicate
    // rather than expose internals" convention this project already uses
    // elsewhere) -- must match PadSynth::kMinAdsrSeconds/kMaxAdsrSeconds
    // exactly or this graph and the footer's own seconds readout would
    // silently disagree about where a given knob position lands.
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

    PadSynth::PadPresetData pad_preset = pad_synth_ ? pad_synth_->CapturePreset()
                                                      : PadSynth::PadPresetData{};

    g_progress_disp      = disp_;
    file_op_in_progress_ = true;
    bool ok = PerformanceStore::Save(slot, *tempo_, layers_, num_layers_, master_volume01_,
                                       bypass_, master_filter_mode_, master_filter_cutoff01_,
                                       master_filter_res01_, reverb_size01_,
                                       bypass_reverb_send01_, pad_preset, &Ui::OnSaveLoadProgress);
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
    if(file_slot_count_ == 0)
        return;
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
    PadSynth::PadPresetData pad_preset;
    bool ok = PerformanceStore::Load(slot, *tempo_, layers_, num_layers_, &master_volume01_,
                                       &bypass_, &master_filter_mode_, &master_filter_cutoff01_,
                                       &master_filter_res01_, &reverb_size01_,
                                       &bypass_reverb_send01_, &pad_preset, &Ui::OnSaveLoadProgress);
    file_op_in_progress_ = false;
    g_progress_disp       = nullptr;
    g_audio_suspended     = false; // every layer + tempo phase is consistent now

    if(ok && pad_synth_)
    {
        pad_synth_->ApplyPreset(pad_preset);
        pad_loaded_preset_slot_ = -1; // this performance's pad sound, not a named preset
    }

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

void Ui::RefreshPadPresetSlots()
{
    pad_preset_user_slot_count_
        = PerformanceStore::ListPadPresets(pad_preset_user_slots_, kMaxPadPresetSlots);
    int total = PadSynth::kNumFactoryPresets + pad_preset_user_slot_count_;
    if(pad_preset_cursor_ >= total)
        pad_preset_cursor_ = total > 0 ? total - 1 : 0;
    pad_preset_slots_dirty_ = false;
}

void Ui::TriggerSavePadPreset(bool force_new)
{
    if(!pad_synth_)
        return;
    // Smart save (force_new=false): overwrite pad_loaded_preset_slot_ if
    // it's a real, writable user slot; a factory preset (1..kNumFactory-
    // Presets) or nothing loaded (-1) has no valid slot to overwrite, so
    // fall back to a new one automatically -- same reasoning TriggerSave()
    // uses for loaded_slot_.
    int slot = (!force_new && pad_loaded_preset_slot_ > PadSynth::kNumFactoryPresets)
                   ? pad_loaded_preset_slot_
                   : PerformanceStore::NextFreePadPresetSlot();
    if(slot < 0)
    {
        snprintf(pad_preset_status_, sizeof(pad_preset_status_), "Card full/missing");
        return;
    }

    bool ok = PerformanceStore::SavePadPreset(slot, pad_synth_->CapturePreset());
    if(ok)
    {
        pad_loaded_preset_slot_ = slot;
        snprintf(pad_preset_status_, sizeof(pad_preset_status_), "Saved %d", slot);
        pad_preset_slots_dirty_ = true; // a new slot may now exist
    }
    else
    {
        snprintf(pad_preset_status_, sizeof(pad_preset_status_), "Fail:%s",
                  PerformanceStore::GetLastError());
    }
}

void Ui::TriggerLoadPadPreset()
{
    if(!pad_synth_)
        return;
    int slot = PadPresetCursorToSlot(pad_preset_cursor_, pad_preset_user_slots_,
                                       pad_preset_user_slot_count_);
    if(slot < 0)
        return;

    PadSynth::PadPresetData preset;
    bool ok = PerformanceStore::LoadPadPreset(slot, &preset);
    if(ok)
    {
        pad_synth_->ApplyPreset(preset);
        pad_loaded_preset_slot_ = slot;
        if(slot <= PadSynth::kNumFactoryPresets)
            snprintf(pad_preset_status_, sizeof(pad_preset_status_), "Loaded %s",
                      PadSynth::GetFactoryPresetName(slot - 1));
        else
            snprintf(pad_preset_status_, sizeof(pad_preset_status_), "Loaded %d", slot);
    }
    else
    {
        snprintf(pad_preset_status_, sizeof(pad_preset_status_), "Fail:%s",
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
    // overwrite, so fall back to a new one -- same reasoning
    // TriggerSave()/TriggerSavePadPreset() use for their own loaded-slot
    // tracking.
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

void Ui::TriggerLoadGranularPreset()
{
    if(!granular_ || granular_preset_cursor_ < 0
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
