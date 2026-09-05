#include <cstring>
#include "daisy_pod.h"
#include "daisysp.h"
#include "dev/oled_ssd130x.h"
#include "tempo_clock.h"
#include "looper_layer.h"
#include "ui.h"
#include "performance_store.h"
#include "audio_engine.h"

using namespace daisy;

// Same sizing as the original Ouroboros firmware: ~33 seconds per layer
// at 48kHz, stereo. 4 layers (dropped from the original 5) to keep the
// per-layer SDRAM budget (Phaser + PitchShifter per layer, plus one
// shared ReverbSc bus -- see fx_reverb_shared below) comfortable. Most
// per-layer effect objects are a few hundred bytes and live in ordinary
// SRAM via the LooperLayer array below -- Phaser, PitchShifter, and
// ReverbSc are the exceptions (~75KB/~128KB/~386KB per instance), and
// are placed in SDRAM alongside these loop buffers.
#define kBuffSize 1600000
#define kNumLayers 4

DaisyPod hw;

using MyDisplay = daisy::OledDisplay<daisy::SSD130xI2c128x64Driver>;
MyDisplay display;

float DSY_SDRAM_BSS buffer_l[kNumLayers][kBuffSize];
float DSY_SDRAM_BSS buffer_r[kNumLayers][kBuffSize];

TempoClock  tempo;
LooperLayer layers[kNumLayers];
Ui          ui;

// One Phaser + one PitchShifter per layer, in SDRAM because neither is
// small enough to live directly as a LooperLayer member in ordinary
// SRAM. Each layer's instances are fully independent (not shared) --
// LooperLayer::Init() takes pointers to its pair and owns calling
// Init() on them.
daisysp::Phaser       DSY_SDRAM_BSS fx_phaser[kNumLayers];
daisysp::PitchShifter DSY_SDRAM_BSS fx_pitchshift[kNumLayers];

// ONE shared reverb bus for every layer, not one per layer -- up to 4
// simultaneous ReverbSc instances (by far the heaviest thing in this
// signal chain, ~386KB each) was enough real-time DSP cost in the worst
// case to starve the main loop/TIM5 badly enough to cause actual audio
// glitches and OLED corruption, not just UI lag. Every layer's
// Reverb-Send-scaled signal is summed (see LooperLayer::Process()'s
// reverb_send_out parameter) and run through this single instance once
// per sample in AudioCallback() -- see fx_reverb_size01 below for how
// its shared Size control works. In SDRAM for the same reason as
// fx_phaser/fx_pitchshift above.
daisysp::ReverbSc DSY_SDRAM_BSS fx_reverb_shared;

// Master-bus filter -- applied once to the final mix (layers + their
// reverb, summed) rather than per-layer, so it's a plain Svf pair
// living in ordinary SRAM (tiny, no big internal buffer like the
// per-layer effects above) instead of needing SDRAM placement.
daisysp::Svf fx_master_filter_l, fx_master_filter_r;

// See audio_engine.h.
volatile bool g_audio_suspended = false;

// Written only by ControlTimerCallback (a 1kHz TIM5 ISR, see below), read
// only by the main loop, which diffs each against its own "last seen"
// value to build a UiControlEvents delta every iteration (see ui.h's
// UiControlEvents comment for why encoder/button edges need this
// treatment).
volatile int32_t  g_encoder_pos         = 0; // cumulative encoder position
volatile uint32_t g_encoder_click_falls = 0;
volatile uint32_t g_btn1_releases       = 0;
volatile uint32_t g_btn2_releases       = 0;

TimerHandle control_timer;

// Runs Debounce() on the encoder/buttons (and the knob ADC smoothing) at
// a guaranteed 1kHz, independent of whatever the main loop is doing --
// see ui.h's UiControlEvents comment. Cheap (a few GPIO/ADC reads), so
// safe to run from this low-priority timer ISR (see per/tim.cpp: TIM5
// is installed at NVIC priority 0x0f, i.e. below the audio DMA ISR).
void ControlTimerCallback(void*)
{
    hw.ProcessAllControls();
    g_encoder_pos += hw.encoder.Increment();
    if(hw.encoder.FallingEdge())
        g_encoder_click_falls++;
    if(hw.button1.FallingEdge())
        g_btn1_releases++;
    if(hw.button2.FallingEdge())
        g_btn2_releases++;
}

void AudioCallback(AudioHandle::InputBuffer  in,
                   AudioHandle::OutputBuffer out,
                   size_t                    size)
{
    static TempoClock::TempoTick ticks[256];
    static float                 click[256];
    // Every layer's Reverb-Send-scaled signal is summed here (see
    // LooperLayer::Process()'s reverb_send_out parameter), then run
    // through the ONE shared fx_reverb_shared once per sample below --
    // not once per layer any more, see fx_reverb_shared's comment.
    static float reverb_send_l[256];
    static float reverb_send_r[256];

    if(g_audio_suspended)
    {
        for(size_t i = 0; i < size; i++)
        {
            out[0][i] = 0.f;
            out[1][i] = 0.f;
        }
        return;
    }

    // Block-rate: tape-style multiplier on top of every layer's own Speed
    // and the tempo clock's own tick rate -- see Ui::GetProjectSpeed(),
    // TempoClock::Process(), LooperLayer::Process(). Read here, before
    // the loops below, since both need it.
    const float project_speed = ui.GetProjectSpeed();

    for(size_t i = 0; i < size; i++)
    {
        out[0][i] = 0.f;
        out[1][i] = 0.f;
        reverb_send_l[i] = 0.f;
        reverb_send_r[i] = 0.f;

        ticks[i] = tempo.Process(project_speed);
        click[i] = tempo.RenderClick(ticks[i]);
    }

    // Each layer mixes its own dry signal directly into out[], and adds
    // its Reverb Send contribution into reverb_send_l/r (see above --
    // the actual shared ReverbSc runs once per sample further down).
    float* reverb_send_ptrs[2] = {reverb_send_l, reverb_send_r};
    for(int L = 0; L < kNumLayers; L++)
        layers[L].Process(in, out, reverb_send_ptrs, size, ticks, tempo, project_speed);

    const float mv           = ui.GetMasterVolume();
    const bool  byp          = ui.IsBypassed();
    const float bypass_gain  = ui.GetBypassGain();
    const float bypass_reverb_send01 = ui.GetBypassReverbSend01();
    const FilterMode mfilt_mode = ui.GetMasterFilterMode();

    // Block-rate controls (matches LooperLayer's own per-layer filter
    // curve -- kFilterMinHz/kFilterMaxHz in looper_layer.h -- so the
    // master filter feels consistent with the per-layer ones).
    float mfilt_cutoff
        = kFilterMinHz * powf(kFilterMaxHz / kFilterMinHz, ui.GetMasterFilterCutoff01());
    float nyquist_guard = hw.AudioSampleRate() / 3.f - 1.f;
    mfilt_cutoff = mfilt_cutoff < kFilterMinHz ? kFilterMinHz
                   : mfilt_cutoff > nyquist_guard ? nyquist_guard
                                                   : mfilt_cutoff;
    float mfilt_res = ui.GetMasterFilterResonance01() * 0.9f;
    fx_master_filter_l.SetFreq(mfilt_cutoff);
    fx_master_filter_l.SetRes(mfilt_res);
    fx_master_filter_r.SetFreq(mfilt_cutoff);
    fx_master_filter_r.SetRes(mfilt_res);

    // Shared reverb's Size/decay -- one Global:Reverb setting (see
    // Ui::GetReverbSize01()) applied to the single shared instance,
    // replacing what used to be an independent SetFeedback() per layer.
    fx_reverb_shared.SetFeedback(ui.GetReverbSize01());

    for(size_t i = 0; i < size; i++)
    {
        // Bypass's own Send into the shared reverb bus -- independent of
        // every layer's own Send, same bus though. Must happen BEFORE
        // the Process() call right below, which is what actually
        // consumes reverb_send_l/r for this sample. Same bypass_gain-
        // scaled, L+R-summed-to-mono treatment as the dry bypass mix
        // further down (see its own comment for why mono).
        if(byp && bypass_reverb_send01 > 0.f)
        {
            float byp_send = (in[0][i] + in[1][i]) * bypass_gain * bypass_reverb_send01;
            reverb_send_l[i] += byp_send;
            reverb_send_r[i] += byp_send;
        }

        // Shared reverb: process the summed sends once per sample and
        // mix the wet result into out[] -- BEFORE bypass/master filter/
        // click, same position this used to be added in when it ran
        // per-layer inside LooperLayer::Process() itself.
        float rev_wet_l, rev_wet_r;
        fx_reverb_shared.Process(reverb_send_l[i], reverb_send_r[i], &rev_wet_l, &rev_wet_r);
        out[0][i] += rev_wet_l;
        out[1][i] += rev_wet_r;

        if(byp)
        {
            // Digital "wet monitor" mix -- the original's analog bypass
            // relay had no equivalent DSP path to hook into on the Pod,
            // so this mixes a dry copy of the live input straight into
            // the output bus, same idea (hear your loops AND your live
            // input) implemented in software. See DESIGN.md. Scaled by
            // the selected layer's input gain so a quiet source (e.g. a
            // guitar with no preamp) is actually audible here too, not
            // just once it's recorded.
            //
            // Summed to mono (both input channels added together, sent
            // to both outputs) rather than kept as independent L/R --
            // the Pod has one physical input jack (a stereo TRS), and a
            // plain mono guitar cable plugged into it only excites ONE
            // of the two ADC channels (the other gets shorted to ground
            // by the missing ring contact), so a straight per-channel
            // passthrough left that signal audible on only one output
            // side. This only affects the bypass *monitor* -- recording
            // still captures each ADC channel independently (see
            // LooperLayer::Process()'s mic_in/guitar_in), so a genuinely
            // stereo source still records in true stereo.
            float byp_mono = (in[0][i] + in[1][i]) * bypass_gain;
            out[0][i] += byp_mono;
            out[1][i] += byp_mono;
        }

        // Master filter -- applied to the full mix (all layers, their
        // reverb, and the bypass monitor) but deliberately AFTER this
        // point so the metronome click below stays unfiltered and
        // stays clearly audible regardless of the filter setting.
        if(mfilt_mode != FilterMode::Off)
        {
            fx_master_filter_l.Process(out[0][i]);
            fx_master_filter_r.Process(out[1][i]);
            switch(mfilt_mode)
            {
                case FilterMode::LowPass:
                    out[0][i] = fx_master_filter_l.Low();
                    out[1][i] = fx_master_filter_r.Low();
                    break;
                case FilterMode::HighPass:
                    out[0][i] = fx_master_filter_l.High();
                    out[1][i] = fx_master_filter_r.High();
                    break;
                case FilterMode::BandPass:
                    out[0][i] = fx_master_filter_l.Band();
                    out[1][i] = fx_master_filter_r.Band();
                    break;
                default: break;
            }
        }

        out[0][i] = (out[0][i] + click[i]) * mv;
        out[1][i] = (out[1][i] + click[i]) * mv;
    }
}

int main(void)
{
    hw.Init();
    hw.SetAudioBlockSize(48);

    tempo.Init(hw.AudioSampleRate());

    for(int i = 0; i < kNumLayers; i++)
        layers[i].Init(buffer_l[i], buffer_r[i], kBuffSize, hw.AudioSampleRate(),
                       &fx_phaser[i], &fx_pitchshift[i]);

    fx_master_filter_l.Init(hw.AudioSampleRate());
    fx_master_filter_r.Init(hw.AudioSampleRate());

    // Zeroed before Init() for the same reason LooperLayer::Init() now
    // zeros fx_phaser/fx_pitchshift: this project's .sdram_bss objects
    // start out holding raw leftover SDRAM contents, not zero (libDaisy's
    // own sdram.h says as much, and the linker script/startup code
    // confirm it -- only ordinary .bss gets zero-filled at boot).
    memset(&fx_reverb_shared, 0, sizeof(fx_reverb_shared));
    fx_reverb_shared.Init(hw.AudioSampleRate());
    fx_reverb_shared.SetLpFreq(9000.f); // fixed damping, matches the old per-layer default
    // SetFeedback() itself is applied every block in AudioCallback() from
    // Ui::GetReverbSize01() (see there), same live-update pattern as the
    // master filter's cutoff/res just above -- no need to set it here too.

    // I2C1 defaults already target the Pod's header pins (D11=SCL,
    // D12=SDA) and address 0x3C -- most cheap SSD1306/SSD1309 modules
    // need no further configuration. If your module replies on 0x3D
    // instead, uncomment the line below.
    MyDisplay::Config disp_cfg;
    // disp_cfg.driver_config.transport_config.i2c_address = 0x3D;

    // libDaisy's SSD130x transport defaults to 1MHz I2C (Fast Mode Plus).
    // That's aggressive for typical cheap OLED breakout pull-ups/header
    // wiring, and this driver has no bus-recovery: SendData() blocks
    // byte-by-byte with a 1s timeout each, so one glitched byte in a
    // 1024-byte frame can stall the *entire* main loop -- which is also
    // where the encoder/buttons/pot get read -- for up to ~17 minutes.
    // 400kHz is far more forgiving and still plenty fast for this display.
    disp_cfg.driver_config.transport_config.i2c_config.speed
        = daisy::I2CHandle::Config::Speed::I2C_400KHZ;
    display.Init(disp_cfg);

    PerformanceStore::Init(); // mounts the SD card if one is present

    ui.Init(&hw, &display, &tempo, layers, kNumLayers);
    ui.ApplyStartupDefaults(); // no-op if nothing's been saved yet (see PerformanceStore::LoadPrefs())

    hw.StartAdc();
    hw.StartAudio(AudioCallback);

    // Drive control polling from TIM5 at 1kHz -- matches Encoder/Switch's
    // own internal 1kHz debounce rate cap, so nothing is gained by going
    // faster. TIM2 is off-limits (used internally by System for
    // sub-millisecond timing); TIM5 is otherwise unused here. See
    // ControlTimerCallback() above and ui.h's UiControlEvents comment for
    // why this can't just be a call in the while(1) loop below.
    TimerHandle::Config tim_cfg;
    tim_cfg.periph     = TimerHandle::Config::Peripheral::TIM_5;
    tim_cfg.enable_irq = true;
    tim_cfg.period     = System::GetPClk2Freq() / 1000;
    control_timer.Init(tim_cfg);
    control_timer.SetCallback(ControlTimerCallback);
    control_timer.Start();

    // Main loop's own "last seen" checkpoints for the ISR's monotonic
    // counters, used to build each iteration's UiControlEvents delta.
    int32_t  last_encoder_pos         = 0;
    uint32_t last_encoder_click_falls = 0;
    uint32_t last_btn1_releases       = 0;
    uint32_t last_btn2_releases       = 0;

    for(;;)
    {
        UiControlEvents events;
        events.encoder_delta      = g_encoder_pos - last_encoder_pos;
        events.encoder_click_fell = g_encoder_click_falls != last_encoder_click_falls;
        events.btn1_released      = g_btn1_releases != last_btn1_releases;
        events.btn2_released      = g_btn2_releases != last_btn2_releases;
        last_encoder_pos          = g_encoder_pos;
        last_encoder_click_falls  = g_encoder_click_falls;
        last_btn1_releases        = g_btn1_releases;
        last_btn2_releases        = g_btn2_releases;

        ui.Update(events);

        hw.DelayMs(1);
    }
}
