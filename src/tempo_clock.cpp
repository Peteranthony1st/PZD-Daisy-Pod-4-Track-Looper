#include "tempo_clock.h"
#include <cmath>

namespace
{
constexpr float kPi = 3.14159265358979323846f;

inline float Clamp(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}
} // namespace

void TempoClock::Init(float sample_rate)
{
    sample_rate_          = sample_rate;
    bpm_                  = 100.f;
    bars_                 = 4;
    locked_               = false;
    metronome_enabled_    = true;
    metronome_vol_        = 0.5f;
    phase_samples_        = 0.0;
    beat_in_bar_          = 0;
    bar_in_loop_          = 0;
    count_state_          = CountState::Idle;
    count_in_beats_remaining_      = 0;
    record_pending_after_count_in_ = false;
    click_env_            = 0.f;
    click_phase_          = 0.f;
    click_freq_           = 1200.f;
    click_audible_        = false;
    // ~12 ms decay to a short, punchy click regardless of sample rate.
    click_decay_coeff_ = expf(-1.f / (0.012f * sample_rate_));
}

float TempoClock::SamplesPerBeat() const
{
    return (60.f / bpm_) * sample_rate_;
}

void TempoClock::SetBpm(float bpm)
{
    if(locked_)
        return;
    bpm_ = Clamp(bpm, 40.f, 240.f);
}

void TempoClock::SetBars(int bars)
{
    if(locked_)
        return;
    bars_ = bars < 1 ? 1 : (bars > 16 ? 16 : bars);
    if(bar_in_loop_ >= bars_)
        bar_in_loop_ = 0; // avoid a stale "Bar 5 of 3"-style display if Bars shrinks
}

size_t TempoClock::GetLoopLengthSamples() const
{
    float samples = SamplesPerBeat() * (float)kBeatsPerBar * (float)bars_;
    return (size_t)(samples + 0.5f);
}

void TempoClock::SetMetronomeVolume01(float v)
{
    metronome_vol_ = Clamp(v, 0.f, 1.f);
}

void TempoClock::RequestRecordStart()
{
    count_state_ = CountState::WaitingForBar;
}

void TempoClock::ResetPhase()
{
    phase_samples_ = 0.0;
    // NOT 0 -- see beat_in_bar_/bar_in_loop_'s comment in the header for
    // why raw values run one step ahead of what's audible. During a live
    // recording, a new layer's sample 0 always lands on the SAME
    // Process() call that a downbeat tick fires on, and beat_in_bar_/
    // bar_in_loop_ have already been incremented to 1 (mod their range)
    // by the time that call returns -- that's what GetBeatInBar()'s "-1"
    // correction is undoing to report the downbeat as beat 0. A caller
    // of ResetPhase() is asserting "every layer's play_pos_ is 0 right
    // now, as if a downbeat just fired", so the raw counters need that
    // *post*-downbeat value, not Init()'s pre-anything-has-happened 0 --
    // leaving them at 0 here meant the next tick.downbeat (and its
    // click) didn't fire until a full beat after the loop's actual
    // downbeat had already played silently, i.e. the metronome landing
    // exactly one beat late relative to the audio.
    beat_in_bar_                    = 1 % kBeatsPerBar;
    bar_in_loop_                    = 1 % bars_;
    count_state_                    = CountState::Idle;
    count_in_beats_remaining_       = 0;
    record_pending_after_count_in_  = false;
    click_env_                      = 0.f;
    click_phase_                    = 0.f;
    click_audible_                  = false;
}

TempoClock::TempoTick TempoClock::Process()
{
    TempoTick tick{false, false, false, false, false};

    phase_samples_ += 1.0;
    const float spb = SamplesPerBeat();
    if(phase_samples_ >= spb)
    {
        phase_samples_ -= spb;
        tick.beat     = true;
        tick.downbeat = (beat_in_bar_ == 0);
        // Same relationship to tick.downbeat that beat_in_bar_ has to
        // tick.beat (increment right as the new bar's first beat fires,
        // not one beat early on the previous bar's last beat) -- see
        // GetBarInLoop()'s comment for why that timing matters.
        if(tick.downbeat)
            bar_in_loop_ = (bar_in_loop_ + 1) % bars_;
        beat_in_bar_  = (beat_in_bar_ + 1) % kBeatsPerBar;
        tick.bar      = (beat_in_bar_ == 0);

        // Count-in only ever begins right on a downbeat, so the count
        // clicks land on a clean bar boundary instead of mid-bar. These
        // are deliberately sequential ifs, not an exclusive switch: the
        // beat that transitions WaitingForBar -> CountingIn must also be
        // handled as the first count-in beat in this same tick, or the
        // count-in comes out one beat short.
        if(count_state_ == CountState::WaitingForBar && tick.downbeat)
        {
            count_state_              = CountState::CountingIn;
            count_in_beats_remaining_ = kCountInBeats;
        }

        if(count_state_ == CountState::CountingIn)
        {
            tick.count_in_beat = true;
            count_in_beats_remaining_--;
            if(count_in_beats_remaining_ <= 0)
            {
                // The next beat tick (a downbeat, since the count-in is
                // exactly one bar long) is where recording begins.
                count_state_                   = CountState::Idle;
                record_pending_after_count_in_ = true;
            }
        }
        else if(count_state_ == CountState::Idle && record_pending_after_count_in_
                && tick.downbeat)
        {
            tick.record_start               = true;
            record_pending_after_count_in_  = false;
        }
    }

    return tick;
}

float TempoClock::RenderClick(const TempoTick& tick)
{
    if(tick.beat || tick.count_in_beat)
    {
        click_env_   = 1.f;
        click_phase_ = 0.f;
        // Count-in clicks and the downbeat get a higher, more insistent
        // pitch than regular beats so they're easy to tell apart by ear.
        click_freq_ = tick.count_in_beat ? 1800.f
                      : tick.downbeat     ? 1600.f
                                          : 1000.f;
        // Count-in clicks always sound (so a quantized record-start is
        // actually audible) even with the regular per-beat metronome
        // toggled off; regular beat clicks still respect that toggle.
        click_audible_ = tick.count_in_beat || metronome_enabled_;
    }

    float out = 0.f;
    if(click_env_ > 0.0005f)
    {
        out = sinf(click_phase_) * click_env_;
        click_phase_ += 2.f * kPi * click_freq_ / sample_rate_;
        if(click_phase_ > 2.f * kPi)
            click_phase_ -= 2.f * kPi;
        click_env_ *= click_decay_coeff_;
    }
    else
    {
        click_env_ = 0.f;
    }

    return click_audible_ ? out * metronome_vol_ : 0.f;
}
