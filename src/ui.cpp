#include "ui.h"
#include "performance_store.h"
#include "audio_engine.h"
#include <cstdio>
#include <cstring>
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

// See LooperLayer::SetPitchDelayPreset() -- Fast/Med/Smooth trade sync
// latency against pitch-shift smoothness.
const char* PitchDelayPresetName(int preset)
{
    switch(preset)
    {
        case 0: return "Fast";
        case 1: return "Med";
        case 2: return "Smooth";
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
              int                           num_layers)
{
    pod_        = pod;
    disp_       = display;
    tempo_      = tempo;
    layers_     = layers;
    num_layers_ = num_layers;

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

void Ui::Update(uint32_t beat_count, uint32_t downbeat_count, const UiControlEvents& events)
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

    UpdateLeds(beat_count, downbeat_count);

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
        if(screen_ == Screen::Home)
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
    }

    if(pod_->encoder.Pressed() && pod_->encoder.TimeHeldMs() > 600.f && !encoder_long_fired_)
    {
        encoder_long_fired_ = true;
        if(screen_ == Screen::Home)
        {
            screen_      = Screen::Global;
            global_page_ = GlobalPage::Tempo;
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
        encoder_long_fired_ = false;
    }
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
            button2_long_fired_ = false;
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
            button2_long_fired_ = false;
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
                case LayerPage::Pitch:
                    Cur().SetPitchEnabled(!Cur().GetPitchEnabled());
                    break;
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
            else if(global_page_ == GlobalPage::File)
                TriggerSave();
            else if(global_page_ == GlobalPage::Export)
                TriggerExport();
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
    else if(screen_ == Screen::Layer && layer_page_ == LayerPage::Pitch)
    {
        int n = LooperLayer::kNumPitchDelayPresets;
        int p = (Cur().GetPitchDelayPreset() + 1) % n;
        Cur().SetPitchDelayPreset(p);
    }
    // All other screens: Button2 is unused (Status page's Button2 is
    // handled separately in HandleButton2 as a hold-to-confirm clear).
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
                case LayerPage::Pitch: return KnobContext::LayerPitch;
                default: return KnobContext::LayerStatus;
            }
        case Screen::Global:
            switch(global_page_)
            {
                case GlobalPage::Tempo: return KnobContext::GlobalTempo;
                case GlobalPage::Filter: return KnobContext::GlobalFilter;
                case GlobalPage::File: return KnobContext::GlobalFile;
                case GlobalPage::Export: return KnobContext::GlobalExport;
                default: return KnobContext::GlobalTempo;
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
            k1_pickup_raw_[i] = Cur().GetReverbSend01();
            k2_pickup_raw_[i] = Cur().GetReverbSize01();
            break;
        case KnobContext::LayerGain:
            k1_pickup_raw_[i] = Cur().GetInputGain01();
            break;
        case KnobContext::LayerPitch:
            k1_pickup_raw_[i] = Cur().GetPitchAmount01();
            k2_pickup_raw_[i] = Cur().GetPitchFun01();
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
        case KnobContext::GlobalFile: break; // browses a list directly, no pickup used
        case KnobContext::GlobalExport: break; // no continuous knob values, Button1 triggers it
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

void Ui::ApplyKnobs()
{
    float k1 = pod_->GetKnobValue(DaisyPod::KNOB_1);
    float k2 = pod_->GetKnobValue(DaisyPod::KNOB_2);

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
                    // Both knobs are this layer's own independent
                    // reverb now (send amount + size/decay) -- each
                    // layer owns its own ReverbSc instance.
                    if(KnobPickUp(k1, k1_pickup_raw_[ci], k1_pickup_engaged_[ci]))
                        Cur().SetReverbSend01(k1);
                    if(KnobPickUp(k2, k2_pickup_raw_[ci], k2_pickup_engaged_[ci]))
                        Cur().SetReverbSize01(k2);
                    break;
                case LayerPage::Gain:
                    if(KnobPickUp(k1, k1_pickup_raw_[ci], k1_pickup_engaged_[ci]))
                        Cur().SetInputGain01(k1);
                    break;
                case LayerPage::Pitch:
                    if(KnobPickUp(k1, k1_pickup_raw_[ci], k1_pickup_engaged_[ci]))
                        Cur().SetPitchAmount01(k1);
                    if(KnobPickUp(k2, k2_pickup_raw_[ci], k2_pickup_engaged_[ci]))
                        Cur().SetPitchFun01(k2);
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
            break;
    }
}

void Ui::UpdateLeds(uint32_t beat_count, uint32_t downbeat_count)
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

    // Led2: flashes with the metronome so you can see tempo even with
    // the click turned down (or off) on a loud stage. Brighter/white on
    // the downbeat, dimmer blue on the other beats.
    bool new_beat     = (beat_count != last_beat_count_);
    bool new_downbeat = (downbeat_count != last_downbeat_count_);
    last_beat_count_     = beat_count;
    last_downbeat_count_ = downbeat_count;
    if(new_downbeat)
        led2_flash_ = 1.f;
    else if(new_beat)
        led2_flash_ = 0.5f;
    if(tempo_->IsMetronomeEnabled())
        pod_->led2.Set(led2_flash_, led2_flash_, led2_flash_ * 0.6f);
    else
        pod_->led2.Set(0.f, 0.f, 0.f);
    led2_flash_ *= 0.8f; // quick decay so it reads as a flash, not a glow

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
    }
    disp_->Update();
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
        disp_->WriteString(label, Font_6x8, true);
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
        disp_->WriteString(label, Font_6x8, true);
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
        case LayerPage::Pitch: page_name = "Pitch"; break;
        default: break;
    }
    // Compact "N:Page" (not "Layer N - Page") specifically to leave room
    // for the beat indicator on the same row -- see DrawBeatIndicator().
    snprintf(title, sizeof(title), "%d:%s", cursor_layer_ + 1, page_name);
    disp_->SetCursor(0, 0);
    disp_->WriteString(title, Font_6x8, true);
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
                disp_->WriteString(msg, Font_6x8, true);
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
                // instead. Centered in the full gap between the title
                // divider (y9) and the footer divider (kFooterDividerY),
                // now that hold-to-clear has its own space above instead
                // of sharing this one. 1px bar + 1px gap.
                //
                // Auto-scaled to the loudest bucket currently in the
                // cache, so quiet input still uses the full height
                // instead of reading as "barely there" -- cheap to do
                // here (63 cached floats, ~30Hz redraw) versus the
                // buffer itself (up to 1.6M raw samples), which is
                // exactly why the cache exists in the first place.
                const float* peaks = Cur().GetWaveformPeaks();
                float        max_peak = 0.f;
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
                if(st == LayerState::Playing || st == LayerState::Paused
                   || st == LayerState::Overdubbing)
                {
                    int col = (int)(Cur().GetPlayPos01() * LooperLayer::kWaveformCols);
                    if(col >= LooperLayer::kWaveformCols)
                        col = LooperLayer::kWaveformCols - 1;
                    int px = 1 + col * 2;
                    disp_->DrawLine(px, 10, px, 11, true);
                }
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
            disp_->WriteString(line1, Font_6x8, true);

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
            disp_->WriteString(line1, Font_6x8, true);

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
            disp_->WriteString(line1, Font_6x8, true);

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
            // Independent per layer now (each layer owns its own
            // ReverbSc) -- both Send and Size are this layer's alone.
            snprintf(line1, sizeof(line1), "Send:%d%% Size:%d%%",
                      (int)(Cur().GetReverbSend01() * 100.f + 0.5f),
                      (int)(Cur().GetReverbSize01() * 100.f + 0.5f));
            disp_->SetCursor(0, 20);
            disp_->WriteString(line1, Font_6x8, true);

            char send_val[8], size_val[8];
            snprintf(send_val, sizeof(send_val), "%d%%",
                      (int)(Cur().GetReverbSend01() * 100.f + 0.5f));
            snprintf(size_val, sizeof(size_val), "%d%%",
                      (int)(Cur().GetReverbSize01() * 100.f + 0.5f));
            DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "Send", send_val, size_val, "Size");
            DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, "", "", "", "");
            break;
        }
        case LayerPage::Gain:
        {
            // See the Speed page's comment above on why not %f.
            int gain_x10 = (int)(Cur().GetInputGain() * 10.f + 0.5f);
            snprintf(line1, sizeof(line1), "Gain: %d.%dx", gain_x10 / 10, gain_x10 % 10);
            disp_->SetCursor(0, 20);
            disp_->WriteString(line1, Font_6x8, true);

            char gain_val[8];
            snprintf(gain_val, sizeof(gain_val), "%d.%dx", gain_x10 / 10, gain_x10 % 10);
            DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "Gain", gain_val, "", "");
            DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, "", "", "", "");
            break;
        }
        case LayerPage::Pitch:
        {
            // Independent of the character effect (see LooperLayer::
            // SetPitchEnabled()'s comment) -- real Off/On toggle via
            // button1, same idea as FilterMode::Off, so knob position
            // never implies "off" the way it would for a bipolar
            // -12..+12 range with no separate toggle.
            snprintf(line1, sizeof(line1), "Pitch: %s", Cur().GetPitchEnabled() ? "On" : "Off");
            disp_->SetCursor(0, 20);
            disp_->WriteString(line1, Font_6x8, true);

            float semi_f = Cur().GetPitchAmount01() * 24.f - 12.f;
            int   semis  = (int)(semi_f >= 0.f ? semi_f + 0.5f : semi_f - 0.5f);
            char  amount_val[8], fun_val[8];
            snprintf(amount_val, sizeof(amount_val), "%+dst", semis);
            snprintf(fun_val, sizeof(fun_val), "%d%%", (int)(Cur().GetPitchFun01() * 100.f + 0.5f));
            DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "Amount", amount_val, fun_val, "Fun");

            // Left label reflects what a tap does next, same convention
            // as every other dynamic button label in this file. Right
            // side shows the current delay preset (button2 cycles it,
            // see OnButton2Short()) -- same "action:state" shape as
            // Home's Bypass:On/Off label.
            const char* toggle_label = Cur().GetPitchEnabled() ? "Pitch Off" : "Pitch On";
            char        delay_label[16];
            snprintf(delay_label, sizeof(delay_label), "Delay:%s",
                      PitchDelayPresetName(Cur().GetPitchDelayPreset()));
            DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, toggle_label, "", "",
                             delay_label);
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

    // Compact form (not "Global Settings") for the same reason as the
    // Layer screen's title -- leaves room for the beat indicator on the
    // same row.
    disp_->SetCursor(0, 0);
    disp_->WriteString(global_page_ == GlobalPage::Tempo ? "Global:Tempo" : "Global:Filter",
                         Font_6x8, true);
    DrawBeatIndicator(disp_->Width() - 41, 0, 3);
    disp_->DrawLine(0, 9, disp_->Width() - 1, 9, true);

    char line1[32], line2[32];
    if(global_page_ == GlobalPage::Tempo)
    {
        snprintf(line1, sizeof(line1), "BPM: %d %s", (int)(tempo_->GetBpm() + 0.5f),
                  tempo_->IsLocked() ? "(locked)" : "");
        snprintf(line2, sizeof(line2), "Metro: %s",
                  tempo_->IsMetronomeEnabled() ? "On" : "Off");
        disp_->SetCursor(0, 12);
        disp_->WriteString(line1, Font_6x8, true);
        disp_->SetCursor(0, 24);
        disp_->WriteString(line2, Font_6x8, true);
        if(tempo_->IsLocked())
        {
            disp_->SetCursor(0, 36);
            disp_->WriteString("Clear layers first", Font_6x8, true);
        }

        char bpm_val[8], bars_val[8];
        snprintf(bpm_val, sizeof(bpm_val), "%d", (int)(tempo_->GetBpm() + 0.5f));
        snprintf(bars_val, sizeof(bars_val), "%d", tempo_->GetBars());
        DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "BPM", bpm_val, bars_val, "Bars");
        DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, "Metro on/off", "", "", "");
    }
    else // GlobalPage::Filter -- master-bus filter, applied once to the
         // full mix in main.cpp, not per-layer like the Layer Filter page.
    {
        snprintf(line1, sizeof(line1), "Mode: %s", FilterModeName(master_filter_mode_));
        disp_->SetCursor(0, 16);
        disp_->WriteString(line1, Font_6x8, true);

        // Actual values, not the pickup-tracking array -- see the
        // comment on DrawHome()'s equivalent Vol readout.
        char cutoff_val[8], res_val[8];
        snprintf(cutoff_val, sizeof(cutoff_val), "%d%%",
                  (int)(master_filter_cutoff01_ * 100.f + 0.5f));
        snprintf(res_val, sizeof(res_val), "%d%%", (int)(master_filter_res01_ * 100.f + 0.5f));
        DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "Cutoff", cutoff_val, res_val, "Res");
        DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, "Cycle mode", "", "", "");
    }
}

void Ui::DrawFileScreen()
{
    disp_->SetCursor(0, 0);
    disp_->WriteString("Global:File", Font_6x8, true);
    DrawBeatIndicator(disp_->Width() - 41, 0, 3);
    disp_->DrawLine(0, 9, disp_->Width() - 1, 9, true);

    if(!PerformanceStore::IsCardPresent())
    {
        disp_->SetCursor(0, 20);
        disp_->WriteString("No SD card", Font_6x8, true);
        DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "", "", "", "");
        DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, "", "", "", "");
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
        disp_->WriteString("Hold: New...", Font_6x8, true);
        disp_->DrawRect(0, 30, disp_->Width() - 1, 34, true, false);
        if(w > 0)
            disp_->DrawRect(1, 31, w, 33, true, true);
    }
    else if(pod_->button2.Pressed() && file_slot_count_ > 0)
    {
        float held = pod_->button2.TimeHeldMs();
        int   w    = (int)(Clampf(held / 800.f, 0.f, 1.f) * (disp_->Width() - 2));
        disp_->SetCursor(0, 20);
        disp_->WriteString("Hold: Load...", Font_6x8, true);
        disp_->DrawRect(0, 30, disp_->Width() - 1, 34, true, false);
        if(w > 0)
            disp_->DrawRect(1, 31, w, 33, true, true);
    }
    else
    {
        char line1[32], line2[32];
        if(loaded_slot_ >= 0)
            snprintf(line1, sizeof(line1), "Now: %d - Performance", loaded_slot_);
        else
            snprintf(line1, sizeof(line1), "Now: (unsaved)");
        disp_->SetCursor(0, 14);
        disp_->WriteString(line1, Font_6x8, true);

        if(file_slot_count_ > 0)
            snprintf(line2, sizeof(line2), "Load: %d - Performance",
                      file_slots_[file_cursor_]);
        else
            snprintf(line2, sizeof(line2), "Load: (no saves)");
        disp_->SetCursor(0, 26);
        disp_->WriteString(line2, Font_6x8, true);

        if(file_status_[0] != '\0')
        {
            disp_->SetCursor(0, 36);
            disp_->WriteString(file_status_, Font_6x8, true);
        }
    }

    // Knob1 browses a discrete list of slots (not a live 0..1 parameter)
    // -- show the slot number it's currently on, same index shown in the
    // "Load: N - Performance" line above. Empty when there's nothing to
    // browse (no saves yet), same "nothing if it does nothing" rule as an
    // idle knob elsewhere. "Hold=New" isn't mentioned in the button row,
    // same convention as the Status page's footer omitting its own
    // button1 long-press meaning -- the "Hold: New..." progress bar
    // (shown once you actually hold it) is the discoverability path for
    // that one.
    char load_val[8] = "";
    if(file_slot_count_ > 0)
        snprintf(load_val, sizeof(load_val), "%d", file_slots_[file_cursor_]);
    DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "Load", load_val, "", "");
    DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, "Save", "", "", "Hold=Load");
}

void Ui::DrawExportScreen()
{
    disp_->SetCursor(0, 0);
    disp_->WriteString("Global:Export", Font_6x8, true);
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
    disp_->WriteString(msg, Font_6x8, true);

    if(!PerformanceStore::IsCardPresent())
    {
        DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "", "", "", "");
        DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, "", "", "", "");
        return;
    }

    // No hold-to-confirm here (unlike File's New/Load) -- Export always
    // creates a new numbered file and never overwrites/destroys anything,
    // so a plain tap is safe.
    DrawControlRow(kFooterRow1Y, false, kFooterDividerY, "", "", "", "");
    DrawControlRow(kFooterRow2Y, true, kFooterInterRowDividerY, "Export", "", "", "");
}

void Ui::RefreshFileSlots()
{
    file_slot_count_ = PerformanceStore::ListSlots(file_slots_, kMaxFileSlots);
    if(file_cursor_ >= file_slot_count_)
        file_cursor_ = file_slot_count_ > 0 ? file_slot_count_ - 1 : 0;
    file_slots_dirty_ = false;
}

void Ui::TriggerSave()
{
    int slot = loaded_slot_ >= 0 ? loaded_slot_ : PerformanceStore::NextFreeSlot();
    if(slot < 0)
    {
        snprintf(file_status_, sizeof(file_status_), "Card full/missing");
        return;
    }

    g_progress_disp      = disp_;
    file_op_in_progress_ = true;
    bool ok = PerformanceStore::Save(slot, *tempo_, layers_, num_layers_, master_volume01_,
                                       bypass_, master_filter_mode_, master_filter_cutoff01_,
                                       master_filter_res01_, &Ui::OnSaveLoadProgress);
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
    snprintf(file_status_, sizeof(file_status_), "New performance");
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
    bool ok = PerformanceStore::Load(slot, *tempo_, layers_, num_layers_, &master_volume01_,
                                       &bypass_, &master_filter_mode_, &master_filter_cutoff01_,
                                       &master_filter_res01_, &Ui::OnSaveLoadProgress);
    file_op_in_progress_ = false;
    g_progress_disp       = nullptr;
    g_audio_suspended     = false; // every layer + tempo phase is consistent now

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
                                            &Ui::OnSaveLoadProgress);
    export_op_in_progress_  = false;
    g_progress_disp         = nullptr;
    g_audio_suspended       = false;

    if(ok)
        snprintf(export_status_, sizeof(export_status_), "Exported WAV");
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
    // Font_7x10, not the footer's usual Font_6x8 -- an 8-row-tall font
    // only has room for a 1-pixel descender, which reads as a clipped
    // "g" in "Working..." at this size no matter how it's positioned;
    // this font's extra 2 rows actually give it a real tail.
    d->WriteString("Working...", Font_7x10, true);
    int w = (int)(Clampf(progress01, 0.f, 1.f) * (d->Width() - 2));
    d->DrawRect(0, 32, d->Width() - 1, 40, true, false);
    if(w > 0)
        d->DrawRect(1, 33, w, 39, true, true);
    d->Update();
}
