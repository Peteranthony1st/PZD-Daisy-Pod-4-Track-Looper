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
// at 48kHz, stereo. 4 layers (dropped from the original 5) specifically
// to free enough SDRAM for each layer to own an independent ReverbSc
// instance (~386KB each) instead of sharing one -- see fx_reverb below.
// Most per-layer effect objects are a few hundred bytes and live in
// ordinary SRAM via the LooperLayer array below -- Phaser, PitchShifter,
// and ReverbSc are the exceptions (~75KB/~128KB/~386KB per instance),
// and are placed in SDRAM alongside these loop buffers.
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

// One Phaser + one PitchShifter + one ReverbSc per layer, in SDRAM
// because none of the three are small enough to live directly as
// LooperLayer members in ordinary SRAM. Each layer's instances are
// fully independent (not shared) -- LooperLayer::Init() takes pointers
// to its trio and owns calling Init() on them.
daisysp::Phaser       DSY_SDRAM_BSS fx_phaser[kNumLayers];
daisysp::PitchShifter DSY_SDRAM_BSS fx_pitchshift[kNumLayers];
daisysp::ReverbSc     DSY_SDRAM_BSS fx_reverb[kNumLayers];

// Master-bus filter -- applied once to the final mix (layers + their
// reverb, summed) rather than per-layer, so it's a plain Svf pair
// living in ordinary SRAM (tiny, no big internal buffer like the
// per-layer effects above) instead of needing SDRAM placement.
daisysp::Svf fx_master_filter_l, fx_master_filter_r;

// See audio_engine.h.
volatile bool g_audio_suspended = false;

// Written only by AudioCallback, read only by the main loop (see ui.h's
// header comment for why this split exists). Plain increment/read of a
// word-sized counter needs no locking for this "did a beat happen"
// purpose -- worst case the LED flash is one main-loop tick (~1ms) late.
volatile uint32_t g_beat_count     = 0;
volatile uint32_t g_downbeat_count = 0;

// Written only by ControlTimerCallback (a 1kHz TIM5 ISR, see below), read
// only by the main loop, which diffs each against its own "last seen"
// value to build a UiControlEvents delta every iteration (see ui.h's
// UiControlEvents comment for why encoder/button edges need this
// treatment and beat_count-style level state doesn't).
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

    if(g_audio_suspended)
    {
        for(size_t i = 0; i < size; i++)
        {
            out[0][i] = 0.f;
            out[1][i] = 0.f;
        }
        return;
    }

    for(size_t i = 0; i < size; i++)
    {
        out[0][i] = 0.f;
        out[1][i] = 0.f;

        ticks[i] = tempo.Process();
        if(ticks[i].beat)
            g_beat_count++;
        if(ticks[i].downbeat)
            g_downbeat_count++;
        click[i] = tempo.RenderClick(ticks[i]);
    }

    // Each layer mixes its own dry signal AND its own reverb (see
    // LooperLayer::Process()) directly into out[] -- no shared bus here.
    for(int L = 0; L < kNumLayers; L++)
        layers[L].Process(in, out, size, ticks, tempo);

    const float mv           = ui.GetMasterVolume();
    const bool  byp          = ui.IsBypassed();
    const float bypass_gain  = ui.GetBypassGain();
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

    for(size_t i = 0; i < size; i++)
    {
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
                       &fx_phaser[i], &fx_pitchshift[i], &fx_reverb[i]);

    fx_master_filter_l.Init(hw.AudioSampleRate());
    fx_master_filter_r.Init(hw.AudioSampleRate());

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

        ui.Update(g_beat_count, g_downbeat_count, events);

        hw.DelayMs(1);
    }
}
