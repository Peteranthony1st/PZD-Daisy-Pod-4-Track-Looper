#include <cstring>
#include "daisy_pod.h"
#include "daisysp.h"
#include "dev/oled_ssd130x.h"
#include "tempo_clock.h"
#include "looper_layer.h"
#include "ui.h"
#include "performance_store.h"
#include "audio_engine.h"
#include "itcm.h"
#include "pad_synth.h"
#include "granular_engine.h"

using namespace daisy;

// Same sizing as the original Ouroboros firmware: ~33 seconds per layer
// at 48kHz, stereo. 4 layers (dropped from the original 5) to keep the
// per-layer SDRAM budget (Phaser per layer, plus one shared ReverbSc bus
// -- see fx_reverb_shared below) comfortable. Most per-layer effect
// objects are a few hundred bytes and live in ordinary SRAM via the
// LooperLayer array below -- Phaser and ReverbSc are the exceptions
// (~75KB/~386KB per instance), and are placed in SDRAM alongside these
// loop buffers.
#define kBuffSize 1600000
#define kNumLayers 4

DaisyPod hw;

using MyDisplay = daisy::OledDisplay<daisy::SSD130xI2c128x64Driver>;
MyDisplay display;

float DSY_SDRAM_BSS buffer_l[kNumLayers][kBuffSize];
float DSY_SDRAM_BSS buffer_r[kNumLayers][kBuffSize];

// Grains' own dedicated capture buffer -- separate from the loop layers'
// buffers above (~5s @ 48kHz, chosen to fit comfortably in the SDRAM left
// over after the 4 loop layers). Two sources write into it: Ui's
// TriggerGranularCaptureFromLayer() (an instant main-loop-side copy from
// whichever loop layer is selected) or live input recorded sample-by-
// sample in AudioCallback() while g_granular_capturing is true (see
// below). Either way, granular.SetSource() is pointed at it once the
// capture finishes, same as every other GranularEngine source.
constexpr size_t kGranularCaptureSamples = 240000; // ~5s @ 48kHz
float DSY_SDRAM_BSS g_granular_capture_l[kGranularCaptureSamples];
float DSY_SDRAM_BSS g_granular_capture_r[kGranularCaptureSamples];

// Shared between Ui (which starts/stops recording on Button2 press/
// release -- see Ui::HandleButton2()) and AudioCallback() (which actually
// writes live input samples in while true). volatile for the same
// ISR/main-loop-shared reason as every other cross-context flag in this
// file (g_encoder_pos etc.) -- Ui also clears write_pos to 0 right before
// setting this true, arming a fresh recording.
volatile bool   g_granular_capturing        = false;
volatile size_t g_granular_capture_write_pos = 0;

TempoClock  tempo;
LooperLayer layers[kNumLayers];
Ui          ui;

// One Phaser per layer, in SDRAM because it's not small enough to live
// directly as a LooperLayer member in ordinary SRAM. Each layer's
// instance is fully independent (not shared) -- LooperLayer::Init()
// takes a pointer to it and owns calling Init() on it.
daisysp::Phaser DSY_SDRAM_BSS fx_phaser[kNumLayers];

// ONE shared reverb bus for every layer, not one per layer -- up to 4
// simultaneous ReverbSc instances (by far the heaviest thing in this
// signal chain, ~386KB each) was enough real-time DSP cost in the worst
// case to starve the main loop/TIM5 badly enough to cause actual audio
// glitches and OLED corruption, not just UI lag. Every layer's
// Reverb-Send-scaled signal is summed (see LooperLayer::Process()'s
// reverb_send_out parameter) and run through this single instance once
// per sample in AudioCallback() -- see fx_reverb_size01 below for how
// its shared Size control works. In SDRAM for the same reason as
// fx_phaser above.
daisysp::ReverbSc DSY_SDRAM_BSS fx_reverb_shared;

// Master-bus filter -- applied once to the final mix (layers + their
// reverb, summed) rather than per-layer, so it's a plain Svf pair
// living in ordinary SRAM (tiny, no big internal buffer like the
// per-layer effects above) instead of needing SDRAM placement.
daisysp::Svf fx_master_filter_l, fx_master_filter_r;

// The MIDI-played pad synth (see pad_synth.h for the CPU-budget story
// behind its OscillatorBank/Adsr/Chorus architecture). Small enough
// (Svf/Chorus/OscillatorBank/Adsr are all plain-SRAM-sized, no big
// internal buffer like fx_phaser/fx_reverb_shared) to live directly in
// ordinary SRAM, same reasoning fx_master_filter_l/r's own comment gives.
PadSynth pad_synth;

// The MIDI-played granular voice (see granular_engine.h -- monophonic,
// two overlapping-grain layers). Small enough (Grain/Adsr/Svf state, no
// big internal buffer of its own) to live in ordinary SRAM -- it doesn't
// own the captured audio, just points at whichever buffer holds one (see
// g_granular_source_l/r below).
GranularEngine granular;

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

// Each button's held duration as of the most recent tick it was still
// pressed -- see ui.h's UiControlEvents::btn1_held_ms/btn2_held_ms
// comment for why this can't just be Switch::TimeHeldMs() read from the
// main loop. Naturally "freezes" at the correct value the instant a
// button releases (the write below only happens while Pressed() is
// still true), and stays there until the next press starts updating it
// again.
volatile float    g_btn1_held_ms        = 0.f;
volatile float    g_btn2_held_ms        = 0.f;

// Small ring buffer the pad synth's dry output is copied into every
// block, purely for Ui::DrawOscilloscope()'s live trace on the Pad
// screen -- same "engine writes samples, main.cpp owns the buffer"
// convention used for the looper's own scope-style visuals elsewhere.
constexpr size_t kPadScopeSamples = 1024;
static float      g_pad_scope_l[kPadScopeSamples];
static size_t      g_pad_scope_write_pos = 0;

// Same convention, for GranularParamPage::Filter's own live oscilloscope.
constexpr size_t kGranularScopeSamples = 1024;
static float      g_granular_scope_l[kGranularScopeSamples];
static size_t      g_granular_scope_write_pos = 0;

// Same convention, for Screen::Mixer's own oscilloscope page -- captures
// the actual final post-fader mix (after reverb/bypass/master filter/
// click/master volume), not any one instrument's own signal.
constexpr size_t kMasterScopeSamples = 1024;
static float      g_master_scope_l[kMasterScopeSamples];
static size_t      g_master_scope_write_pos = 0;

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
    // Switch::TimeHeldMs()/Pressed()/FallingEdge() are only reliable when
    // checked at the same rate Debounce() runs (see Switch's own doc
    // comment) -- true here (this whole callback is that same guaranteed
    // 1kHz tick), unlike reading them from the main loop's own irregular
    // iteration rate. Capturing the held duration every tick it's still
    // pressed (not just at the FallingEdge tick itself, when Pressed()
    // has *already* gone false and TimeHeldMs() would already read 0) is
    // what makes g_btn1_held_ms/g_btn2_held_ms trustworthy after the
    // fact.
    if(hw.button1.Pressed())
        g_btn1_held_ms = hw.button1.TimeHeldMs();
    if(hw.button1.FallingEdge())
        g_btn1_releases++;
    if(hw.button2.Pressed())
        g_btn2_held_ms = hw.button2.TimeHeldMs();
    if(hw.button2.FallingEdge())
        g_btn2_releases++;

    // MIDI polled here rather than the main loop for the same reason
    // buttons are: this runs at a guaranteed, jitter-free 1kHz regardless
    // of whatever the main loop's OLED redraw (blocking I2C) is doing.
    // Polled from the main loop instead, a redraw stall could delay
    // draining the UART long enough that a very fast tap's NoteOn AND
    // NoteOff both end up sitting in the queue together, getting drained
    // in the same pass with no audio block ever seeing the note as held
    // in between -- silence instead of a short blip. Same reasoning
    // applies to pitch bend: smoother, evenly-spaced bend_ratio_ updates
    // read far more natural than whatever's left of them after being
    // bunched up behind an OLED write.
    hw.midi.Listen();
    while(hw.midi.HasEvents())
    {
        MidiEvent event = hw.midi.PopEvent();
        if(event.type == NoteOn)
        {
            NoteOnEvent noteon = event.AsNoteOn();
            pad_synth.NoteOn(noteon.note, noteon.velocity);
            // TEMPORARY (Stage 1) -- same MIDI stream drives both engines
            // for now, no channel/screen-based routing yet.
            granular.NoteOn(noteon.note, noteon.velocity);
        }
        else if(event.type == NoteOff)
        {
            NoteOffEvent noteoff = event.AsNoteOff();
            pad_synth.NoteOff(noteoff.note);
            granular.NoteOff(noteoff.note);
        }
        else if(event.type == PitchBend)
        {
            constexpr float kPitchBendRangeSemis = 2.f; // standard default
            PitchBendEvent  pb                   = event.AsPitchBend();
            pad_synth.SetPitchBendSemis((pb.value / 8192.f) * kPitchBendRangeSemis);
        }
        else if(event.type == ControlChange)
        {
            ControlChangeEvent cc = event.AsControlChange();
            if(cc.control_number == 1) // mod wheel
                pad_synth.SetModWheel01(cc.value / 127.f);
        }
    }
}

DSY_ITCM_TEXT
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
    static float pad_l[256];
    static float pad_r[256];
    static float gran_l[256];
    static float gran_r[256];

    if(g_audio_suspended)
    {
        for(size_t i = 0; i < size; i++)
        {
            out[0][i] = 0.f;
            out[1][i] = 0.f;
        }
        return;
    }

    // Grains' Direct Record capture -- see g_granular_capturing's own
    // comment above. Writes raw live input straight in, unmixed with
    // anything else (no pad/click/reverb), same "what you'd actually
    // plug in and pluck" intent as a sampler's own direct record.
    if(g_granular_capturing)
    {
        for(size_t i = 0; i < size; i++)
        {
            if(g_granular_capture_write_pos >= kGranularCaptureSamples)
            {
                g_granular_capturing = false;
                break;
            }
            g_granular_capture_l[g_granular_capture_write_pos] = in[0][i];
            g_granular_capture_r[g_granular_capture_write_pos] = in[1][i];
            g_granular_capture_write_pos++;
        }
    }

    // Block-rate: tape-style multiplier on top of every layer's own Speed
    // and the tempo clock's own tick rate -- see Ui::GetProjectSpeed(),
    // TempoClock::Process(), LooperLayer::Process(). Read here, before
    // the loops below, since both need it.
    const float project_speed = ui.GetProjectSpeed();

    // Skipped along with the loop layers below when Global:Looper is off
    // -- ResetPhase() (called once, right when it's switched off) already
    // leaves bar/beat position, count-in state, and the click envelope at
    // a clean bar-1-beat-1 rest state, and not calling Process() here is
    // what keeps it sitting there (frozen, not just paused) instead of
    // silently continuing to advance in the background while disabled.
    bool looper_enabled = ui.IsLooperEnabled();
    for(size_t i = 0; i < size; i++)
    {
        out[0][i] = 0.f;
        out[1][i] = 0.f;
        reverb_send_l[i] = 0.f;
        reverb_send_r[i] = 0.f;

        if(looper_enabled)
        {
            ticks[i] = tempo.Process(project_speed);
            click[i] = tempo.RenderClick(ticks[i]);
        }
        else
        {
            ticks[i] = TempoClock::TempoTick{};
            click[i] = 0.f;
        }
    }

    // Renders the pad synth's own dry stereo signal (post-chorus/filter/
    // output-level) and adds its Send-scaled contribution into
    // reverb_send_l/r -- see PadSynth::Process()'s own doc comment for
    // why this writes pad_l/r rather than adding directly into out[]
    // (main.cpp needs this exact signal for three separate consumers).
    //
    // Skipped entirely when disabled from Global:Pad -- see
    // Ui::IsPadEnabled()'s doc comment. Real hardware measurement (4
    // layers + full 8-voice Pad + Granular, all active) showed only
    // ~11% CPU headroom left in the worst case, so this needs to be a
    // genuine compute saving, not just muting pad_l/r after the fact.
    if(ui.IsPadEnabled())
    {
        pad_synth.Process(size, pad_l, pad_r, reverb_send_l, reverb_send_r);
    }
    else
    {
        for(size_t i = 0; i < size; i++)
        {
            pad_l[i] = 0.f;
            pad_r[i] = 0.f;
        }
    }

    // Skipped entirely when disabled from Global:Granular -- same real
    // CPU-saving reasoning as kDiagSkipPad's replacement above.
    if(ui.IsGranularEnabled())
    {
        granular.Process(size, gran_l, gran_r, reverb_send_l, reverb_send_r);
    }
    else
    {
        for(size_t i = 0; i < size; i++)
        {
            gran_l[i] = 0.f;
            gran_r[i] = 0.f;
        }
    }

    // Feed the pad's own oscilloscope ring buffer (Ui::DrawPadScreen()'s
    // live waveform trace) -- same "write every block, wrap" pattern as
    // every other capture buffer in this project.
    for(size_t i = 0; i < size; i++)
    {
        g_pad_scope_l[g_pad_scope_write_pos] = pad_l[i];
        g_pad_scope_write_pos                = (g_pad_scope_write_pos + 1) % kPadScopeSamples;
    }

    // Same for Granular's own oscilloscope (GranularParamPage::Filter).
    for(size_t i = 0; i < size; i++)
    {
        g_granular_scope_l[g_granular_scope_write_pos] = gran_l[i];
        g_granular_scope_write_pos
            = (g_granular_scope_write_pos + 1) % kGranularScopeSamples;
    }

    // Recording input is live input PLUS the pad, summed -- so playing
    // the pad synth while a layer is actively recording captures both
    // together, same "always summed, no toggle" decision this project
    // already made for its granular engine (since removed/shelved, but
    // the same reasoning applies here: a live+synth mix is what someone
    // pressing record while playing a MIDI keyboard actually wants).
    static float mixed_in_l[256];
    static float mixed_in_r[256];
    for(size_t i = 0; i < size; i++)
    {
        mixed_in_l[i] = in[0][i] + pad_l[i];
        mixed_in_r[i] = in[1][i] + pad_r[i];
    }
    const float* const       mixed_ptr_arr[2] = {mixed_in_l, mixed_in_r};
    AudioHandle::InputBuffer mixed_in         = mixed_ptr_arr;

    // Each layer mixes its own dry signal directly into out[], and adds
    // its Reverb Send contribution into reverb_send_l/r (see above --
    // the actual shared ReverbSc runs once per sample further down).
    //
    // Skipped entirely when disabled from Global:Looper -- a real stop,
    // not TogglePauseAll()'s own phase-locked pause: no layer's Process()
    // runs at all (no recording, no playback, no reverb-send
    // contribution), same "skip the call outright" CPU-saving pattern as
    // ui.IsPadEnabled()/IsGranularEnabled() above.
    if(looper_enabled)
    {
        float* reverb_send_ptrs[2] = {reverb_send_l, reverb_send_r};
        for(int L = 0; L < kNumLayers; L++)
        {
            LayerState st = layers[L].GetState();
            bool        is_recording_ish = st == LayerState::Recording
                                            || st == LayerState::Overdubbing
                                            || st == LayerState::ArmedCountIn;
            AudioHandle::InputBuffer layer_in = is_recording_ish ? mixed_in : in;
            layers[L].Process(layer_in, out, reverb_send_ptrs, size, ticks, tempo,
                               project_speed);
        }
    }

    // Pad's own dry signal into the master mix -- unconditional, same
    // position (before the shared reverb Process() call and the master
    // filter) so both apply to the pad equally alongside the loops.
    // (kDiagSkipPad above zeroes pad_l/r for this test, so this add is a
    // harmless no-op rather than needing its own separate mute.)
    for(size_t i = 0; i < size; i++)
    {
        out[0][i] += pad_l[i];
        out[1][i] += pad_r[i];
        out[0][i] += gran_l[i];
        out[1][i] += gran_r[i];
    }

    const float mv           = ui.GetMasterVolume();
    const bool  byp          = ui.IsBypassed();
    const float bypass_gain  = ui.GetBypassGain();
    const float bypass_reverb_send01 = ui.GetBypassReverbSend01();
    // Screen::Mixer's own Bypass channel -- multiplies on top of
    // bypass_gain above (see Ui::GetBypassMixVolume01()'s own comment for
    // why these are two separate controls), plus a genuine Pan, applied
    // to both the dry monitor mix and its reverb send below (same "pan
    // affects the send too" treatment LooperLayer::Process() already
    // uses for every loop layer).
    const float bypass_mix_volume = ui.GetBypassMixVolume();
    const float bypass_pan01      = ui.GetBypassPan01();
    const float bypass_pan_l      = 1.f - bypass_pan01;
    const float bypass_pan_r      = bypass_pan01;
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
        // consumes reverb_send_l/r for this sample. Same bypass_gain/
        // mix-volume-scaled, L+R-summed-to-mono treatment as the dry
        // bypass mix further down (see its own comment for why mono),
        // now also panned the same way -- same "pan affects the send
        // too" treatment LooperLayer::Process() uses for every layer.
        if(byp && bypass_reverb_send01 > 0.f)
        {
            float byp_mono_base = (in[0][i] + in[1][i]) * bypass_gain * bypass_mix_volume;
            reverb_send_l[i] += byp_mono_base * bypass_pan_l * bypass_reverb_send01;
            reverb_send_r[i] += byp_mono_base * bypass_pan_r * bypass_reverb_send01;
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
            //
            // Also scaled by Screen::Mixer's own Bypass Volume and panned
            // (bypass_mix_volume/bypass_pan_l/r above) -- panning this
            // mono signal spreads it across the stereo field same as any
            // other mixer channel, same linear law as everything else.
            float byp_mono = (in[0][i] + in[1][i]) * bypass_gain * bypass_mix_volume;
            out[0][i] += byp_mono * bypass_pan_l;
            out[1][i] += byp_mono * bypass_pan_r;
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

        // Screen::Mixer's own oscilloscope -- the actual final signal,
        // captured last, same "write every block, wrap" pattern as
        // g_pad_scope_l/g_granular_scope_l above.
        g_master_scope_l[g_master_scope_write_pos] = out[0][i];
        g_master_scope_write_pos = (g_master_scope_write_pos + 1) % kMasterScopeSamples;
    }
}

// Linker-provided symbols from STM32H750IB_qspi_custom.lds -- .itcm_text
// has a load address (LMA) in QSPIFLASH and a run address (VMA) in
// ITCMRAM, same relationship .data already has between QSPIFLASH and
// SRAM. The stock startup code's own copy-down loop only knows about
// .data's symbols, not this new section, so nothing copies these bytes
// into ITCM without this -- ITCM has no power-on contents of its own
// (same "not zero/not anything in particular at boot" issue this
// project already hit with .sdram_bss, except this is executable code,
// not data, so the CPU would execute whatever garbage was already
// sitting there instead of just misbehaving numerically).
extern uint8_t _sitcm_text[];
extern uint8_t _eitcm_text[];
extern uint8_t _siitcm_text[];

int main(void)
{
    // Must run before anything DSY_ITCM_TEXT-tagged is ever called --
    // in practice that's hw.StartAudio(AudioCallback) further down, but
    // this sits right at the top of main() for the widest possible
    // safety margin.
    memcpy(_sitcm_text, _siitcm_text, (size_t)(_eitcm_text - _sitcm_text));

    hw.Init();

    // hw.Init() -> InitMidi() claims D13 as MIDI UART TX via
    // MidiUartHandler::Config's default-constructed transport_config --
    // the exact same physical pin as the encoder's click button (see
    // daisy_pod.cpp's ENC_CLICK_PIN). The Pod's actual MIDI IN jack only
    // ever wires up D14/RX per Electrosmith's own pinout diagram, so that
    // TX claim is never usable here regardless -- re-init with TX
    // disabled (Pin(PORTX, 0), the standard "don't claim this pin"
    // sentinel) and re-init the encoder afterward to reclaim D13, rather
    // than relying on TX simply never being driven.
    {
        MidiUartHandler::Config midi_config;
        midi_config.transport_config.tx = Pin(PORTX, 0);
        hw.midi.Init(midi_config);
        hw.encoder.Init(seed::D26, seed::D25, seed::D13);
    }
    hw.midi.StartReceive();

    hw.SetAudioBlockSize(48);

    tempo.Init(hw.AudioSampleRate());

    for(int i = 0; i < kNumLayers; i++)
        layers[i].Init(buffer_l[i], buffer_r[i], kBuffSize, hw.AudioSampleRate(),
                       &fx_phaser[i]);

    fx_master_filter_l.Init(hw.AudioSampleRate());
    fx_master_filter_r.Init(hw.AudioSampleRate());

    pad_synth.Init(hw.AudioSampleRate()); // applies its own hardcoded defaults, see pad_synth.h
    granular.Init(hw.AudioSampleRate());
    // The Stage-1 diagnostic overrides that used to force max Fill/zero
    // Gap/an always-on Scan sweep (needed back when there were no real
    // knobs to test with) are gone now that the Grain page is fully
    // built -- the class's own defaults (Fill=2/3, Gap=0.1, Scan off)
    // apply at boot instead, same as every other engine's own hardcoded
    // defaults. Also meaningfully cheaper at rest: max Fill was forcing
    // the densest, most CPU-expensive grain scheduling as the permanent
    // starting state every boot, not just as something the Fill knob
    // could reach if asked for.

    // Zeroed before Init() for the same reason LooperLayer::Init() now
    // zeros fx_phaser: this project's .sdram_bss objects
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

    ui.Init(&hw, &display, &tempo, layers, kNumLayers, &pad_synth, g_pad_scope_l,
            kPadScopeSamples, &granular, g_granular_scope_l, kGranularScopeSamples,
            g_granular_capture_l, g_granular_capture_r, kGranularCaptureSamples,
            &g_granular_capturing, &g_granular_capture_write_pos, g_master_scope_l,
            kMasterScopeSamples);
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
        events.btn1_held_ms       = g_btn1_held_ms;
        events.btn2_held_ms       = g_btn2_held_ms;
        last_encoder_pos          = g_encoder_pos;
        last_encoder_click_falls  = g_encoder_click_falls;
        last_btn1_releases        = g_btn1_releases;
        last_btn2_releases        = g_btn2_releases;

        // MIDI is polled from ControlTimerCallback() (TIM5 ISR) now, not
        // here -- see its own comment for why.

        ui.Update(events);

        hw.DelayMs(1);
    }
}
