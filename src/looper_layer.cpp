#include "looper_layer.h"
#include <math.h>

using namespace daisysp;

namespace
{
inline float Clampf(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}
// kFilterMinHz/kFilterMaxHz now live in looper_layer.h (public), shared
// with main.cpp's master-bus filter.
} // namespace

void LooperLayer::Init(float*             buf_l,
                       float*             buf_r,
                       size_t             buffer_size,
                       float              sample_rate,
                       daisysp::Phaser*       fx_phaser,
                       daisysp::PitchShifter* fx_pitchshift,
                       daisysp::ReverbSc*     fx_reverb)
{
    buffer_l_    = buf_l;
    buffer_r_    = buf_r;
    buffer_size_ = buffer_size;
    sample_rate_ = sample_rate;

    filter_l_.Init(sample_rate_);
    filter_r_.Init(sample_rate_);

    for(int ch = 0; ch < 2; ch++)
    {
        fx_drive_[ch].Init();
        fx_bitcrush_[ch].Init();
        fx_tremolo_[ch].Init(sample_rate_);
        fx_flanger_[ch].Init(sample_rate_);
        fx_autowah_[ch].Init(sample_rate_);
    }
    fx_chorus_.Init(sample_rate_);

    fx_phaser_ = fx_phaser;
    fx_phaser_->Init(sample_rate_);
    fx_pitchshift_ = fx_pitchshift;
    fx_pitchshift_->Init(sample_rate_);
    // Init() leaves it at DaisySP's own default (16384 samples, ~341ms
    // latency) -- apply this layer's actual starting preset (defaults to
    // Fast, the lowest-latency option) instead of leaving that in place
    // until the user happens to cycle it.
    SetPitchDelayPreset(pitch_delay_preset_);
    fx_reverb_ = fx_reverb;
    fx_reverb_->Init(sample_rate_);
    fx_reverb_->SetLpFreq(9000.f); // fixed damping, matches the old shared reverb's default
    fx_reverb_->SetFeedback(reverb_size01_);

    Reset();
}

void LooperLayer::Reset()
{
    state_             = LayerState::Empty;
    write_idx_         = 0;
    record_len_        = 0;
    target_len_        = 0;
    play_pos_          = 0.f;
    meter_             = 0.f;
    for(int i = 0; i < kWaveformCols; i++)
        waveform_peaks_[i] = 0.f;
}

void LooperLayer::Clear()
{
    state_      = LayerState::Empty;
    write_idx_  = 0;
    record_len_ = 0;
    play_pos_   = 0.f;
    meter_      = 0.f;
    for(int i = 0; i < kWaveformCols; i++)
        waveform_peaks_[i] = 0.f;
}

// --- Transport ----------------------------------------------------------

void LooperLayer::OnRecordButtonPressed(TempoClock& tempo)
{
    (void)tempo;
    switch(state_)
    {
        case LayerState::Empty: break; // long-press to arm, short press does nothing
        case LayerState::ArmedCountIn:
            state_ = LayerState::Empty; // cancel the count-in
            break;
        case LayerState::Recording:
            // Manual early stop: use whatever was captured so far.
            record_len_ = write_idx_ > 0 ? write_idx_ : 1;
            state_      = LayerState::Playing;
            play_pos_   = 0.f;
            break;
        case LayerState::Playing: state_ = LayerState::Paused; break;
        case LayerState::Paused: state_ = LayerState::Playing; break;
        case LayerState::Overdubbing: break; // release ends it, not a tap
    }
}

void LooperLayer::OnRecordButtonLongPress(TempoClock& tempo)
{
    switch(state_)
    {
        case LayerState::Empty:
            state_      = LayerState::ArmedCountIn;
            write_idx_  = 0;
            speed_      = 1.f; // always capture at normal speed
            tempo.RequestRecordStart();
            break;
        case LayerState::Playing:
        case LayerState::Paused:
            state_ = LayerState::Overdubbing;
            break;
        default: break;
    }
}

void LooperLayer::OnRecordButtonReleased()
{
    if(state_ == LayerState::Overdubbing)
        state_ = LayerState::Playing;
}

// --- Continuous controls --------------------------------------------------

void LooperLayer::SetVolume01(float v)
{
    volume01_ = Clampf(v, 0.f, 1.f);
    volume_   = powf(volume01_, 2.5f) * 1.4f;
    if(volume_ < 0.f)
        volume_ = 0.f;
}

void LooperLayer::SetPan01(float v)
{
    // Locked during recording/count-in, same rationale as the original
    // firmware: what's captured shouldn't move while it's being captured.
    if(state_ == LayerState::Recording || state_ == LayerState::ArmedCountIn)
        return;
    pan_ = Clampf(v, 0.f, 1.f);
}

void LooperLayer::SetSpeed01(float v)
{
    if(state_ == LayerState::Recording || state_ == LayerState::ArmedCountIn)
        return;
    speed01_ = Clampf(v, 0.f, 1.f);
    const float center    = 0.5f;
    const float dead_zone = 0.09f;
    if(v < center - dead_zone)
    {
        float tt = v / (center - dead_zone);
        speed_   = 0.3f + tt * (1.f - 0.3f);
    }
    else if(v > center + dead_zone)
    {
        float tt = (v - (center + dead_zone)) / (1.f - (center + dead_zone));
        speed_   = 1.f + tt * (2.f - 1.f);
    }
    else
    {
        speed_ = 1.f;
    }
}

void LooperLayer::SetFilterCutoff01(float v)
{
    // Log-ish taper so most of the knob's travel sits in the useful
    // low/mid range, same feel as a real filter cutoff pot.
    v                 = Clampf(v, 0.f, 1.f);
    filter_cutoff01_  = v;
    filter_cutoff_hz_ = kFilterMinHz * powf(kFilterMaxHz / kFilterMinHz, v);
}

void LooperLayer::SetFilterResonance01(float v)
{
    filter_res01_ = Clampf(v, 0.f, 1.f);
    filter_res_   = filter_res01_ * 0.9f; // stay shy of self-oscillation
}

void LooperLayer::SetInputGain01(float v)
{
    input_gain01_ = Clampf(v, 0.f, 1.f);
    input_gain_   = 1.f + input_gain01_ * 3.f; // 1x..4x (0..+12dB)
}

void LooperLayer::SetEffect(LayerEffect e)
{
    if(e != effect_)
    {
        effect_fade_ = 0.f; // ramp back up in Process(), see member comment
        // Reset params to a genuine "off" (0) on every type change --
        // otherwise a newly-selected effect starts from whatever the
        // *previous* effect left the shared param pair at (or the 0.5
        // default), which for something like Drive can be surprisingly
        // loud before you've touched a knob. Verified against the actual
        // DaisySP source (not just guessed from the label names) that
        // param=0 is a real clean/off state for every remaining effect
        // here -- Pitch was the one exception, which is why it's no
        // longer in this enum at all (see SetPitchEnabled()).
        effect_param_a_ = 0.f;
        effect_param_b_ = 0.f;
    }
    effect_ = e;
}

void LooperLayer::SetEffectParamA01(float v)
{
    effect_param_a_ = Clampf(v, 0.f, 1.f);
}

void LooperLayer::SetEffectParamB01(float v)
{
    effect_param_b_ = Clampf(v, 0.f, 1.f);
}

void LooperLayer::SetPitchEnabled(bool on)
{
    if(on && !pitch_enabled_)
        pitch_fade_ = 0.f; // ramp up in Process(), same idea as effect_fade_
    pitch_enabled_ = on;
}

void LooperLayer::SetPitchAmount01(float v)
{
    pitch_amount01_ = Clampf(v, 0.f, 1.f);
}

void LooperLayer::SetPitchFun01(float v)
{
    pitch_fun01_ = Clampf(v, 0.f, 1.f);
}

void LooperLayer::SetPitchDelayPreset(int preset)
{
    if(preset < 0)
        preset = 0;
    if(preset >= kNumPitchDelayPresets)
        preset = kNumPitchDelayPresets - 1;
    pitch_delay_preset_ = preset;

    // Fast/Med computed in ms against this layer's real sample rate, not
    // a hardcoded 48kHz assumption. Smooth passes a large-but-in-range
    // sample count (100000, safely inside uint32_t) rather than deriving
    // one from an even larger ms value -- that overflowed the cast to
    // uint32_t (undefined behavior, not a safe clamp). DaisySP's own
    // SetDelSize() clamps anything past its actual buffer size (16384
    // samples) down to that max, so this reliably lands on the exact
    // original default without this file needing to know that constant.
    uint32_t samples;
    if(preset == 2)
        samples = 100000u;
    else
    {
        const float kPresetMs[2] = {50.f, 125.f};
        samples = (uint32_t)(kPresetMs[preset] * 0.001f * sample_rate_);
    }
    fx_pitchshift_->SetDelSize(samples);

    // Changing the delay line size while this is actively processing can
    // pop -- same declick treatment as enabling it in the first place.
    pitch_fade_ = 0.f;
}

void LooperLayer::SetReverbSend01(float v)
{
    reverb_send_ = Clampf(v, 0.f, 1.f);
}

void LooperLayer::SetReverbSize01(float v)
{
    reverb_size01_ = Clampf(v, 0.f, 1.f);
    fx_reverb_->SetFeedback(reverb_size01_);
}

// --- Save/load ------------------------------------------------------------

void LooperLayer::RestoreRecordedLength(size_t len)
{
    write_idx_  = len;
    record_len_ = len;
    play_pos_   = 0.f;
    state_      = len > 0 ? LayerState::Playing : LayerState::Empty;

    // The incremental per-sample cache update in Process() never ran for
    // this audio (it came from PerformanceStore::Load()'s f_read() straight
    // into buffer_l_, not the normal record path), so do a one-time full
    // rebuild here instead. Fine to be O(len) in this one spot -- Load()
    // is already a slow, blocking, progress-barred operation, not
    // something that happens on every UI redraw.
    for(int i = 0; i < kWaveformCols; i++)
        waveform_peaks_[i] = 0.f;
    if(len > 0)
    {
        for(size_t i = 0; i < len; i++)
        {
            int bucket = (int)(i * (size_t)kWaveformCols / len);
            if(bucket >= kWaveformCols)
                bucket = kWaveformCols - 1;
            float a = fabsf(buffer_l_[i]);
            if(a > waveform_peaks_[bucket])
                waveform_peaks_[bucket] = a;
        }
    }
}

// --- Effects chain --------------------------------------------------------

void LooperLayer::ProcessEffectsChain(float dry_l, float dry_r, float& out_l, float& out_r)
{
    switch(effect_)
    {
        case LayerEffect::Drive:
            fx_drive_[0].SetDrive(effect_param_a_);
            fx_drive_[1].SetDrive(effect_param_a_);
            out_l = fx_drive_[0].Process(dry_l);
            out_r = fx_drive_[1].Process(dry_r);
            break;

        case LayerEffect::Bitcrush:
            fx_bitcrush_[0].SetBitcrushFactor(effect_param_a_);
            fx_bitcrush_[0].SetDownsampleFactor(effect_param_b_);
            fx_bitcrush_[1].SetBitcrushFactor(effect_param_a_);
            fx_bitcrush_[1].SetDownsampleFactor(effect_param_b_);
            out_l = fx_bitcrush_[0].Process(dry_l);
            out_r = fx_bitcrush_[1].Process(dry_r);
            break;

        case LayerEffect::Chorus:
            // Chorus is a mono-in/stereo-out effect -- it's fed the left
            // channel and generates its own stereo spread. Fine for a
            // loop track that's usually close to mono-ish source
            // material (drums, a single guitar/vocal take); a fully
            // independent L/R chorus isn't worth the extra CPU/RAM here.
            fx_chorus_.SetLfoDepth(effect_param_a_);
            fx_chorus_.SetLfoFreq(0.1f + effect_param_b_ * 4.f);
            fx_chorus_.Process(dry_l);
            out_l = fx_chorus_.GetLeft();
            out_r = fx_chorus_.GetRight();
            break;

        case LayerEffect::Tremolo:
            fx_tremolo_[0].SetFreq(0.5f + effect_param_b_ * 9.5f);
            fx_tremolo_[0].SetDepth(effect_param_a_);
            fx_tremolo_[1].SetFreq(0.5f + effect_param_b_ * 9.5f);
            fx_tremolo_[1].SetDepth(effect_param_a_);
            out_l = fx_tremolo_[0].Process(dry_l);
            out_r = fx_tremolo_[1].Process(dry_r);
            break;

        case LayerEffect::Flanger:
            fx_flanger_[0].SetLfoDepth(effect_param_a_);
            fx_flanger_[0].SetLfoFreq(0.1f + effect_param_b_ * 4.f);
            fx_flanger_[1].SetLfoDepth(effect_param_a_);
            fx_flanger_[1].SetLfoFreq(0.1f + effect_param_b_ * 4.f);
            out_l = fx_flanger_[0].Process(dry_l);
            out_r = fx_flanger_[1].Process(dry_r);
            break;

        case LayerEffect::AutoWah:
            fx_autowah_[0].SetWah(effect_param_a_);
            fx_autowah_[0].SetLevel(effect_param_b_);
            fx_autowah_[1].SetWah(effect_param_a_);
            fx_autowah_[1].SetLevel(effect_param_b_);
            out_l = fx_autowah_[0].Process(dry_l);
            out_r = fx_autowah_[1].Process(dry_r);
            break;

        // Phaser is mono (see the member comment in looper_layer.h for
        // why): fed a summed L+R, same result to both output channels.
        case LayerEffect::Phaser:
        {
            float mono_in = (dry_l + dry_r) * 0.5f;
            fx_phaser_->SetLfoDepth(effect_param_a_);
            fx_phaser_->SetLfoFreq(0.1f + effect_param_b_ * 4.f);
            float wet = fx_phaser_->Process(mono_in);
            out_l = out_r = wet;
            break;
        }

        case LayerEffect::Off:
        default: out_l = dry_l; out_r = dry_r; break;
    }
}

// --- Audio ------------------------------------------------------------

void LooperLayer::Process(AudioHandle::InputBuffer  in,
                          AudioHandle::OutputBuffer out,
                          size_t                    size,
                          const TempoClock::TempoTick* ticks,
                          TempoClock&                  tempo)
{
    const float panL = 1.f - pan_;
    const float panR = pan_;

    for(size_t i = 0; i < size; i++)
    {
        const TempoClock::TempoTick& tick = ticks[i];
        const float mic_in    = in[0][i];
        const float guitar_in = in[1][i];

        if(state_ == LayerState::ArmedCountIn)
        {
            if(tick.record_start)
            {
                state_      = LayerState::Recording;
                write_idx_  = 0;
                target_len_ = tempo.GetLoopLengthSamples();
                if(target_len_ > buffer_size_)
                    target_len_ = buffer_size_; // don't record past SDRAM allocation
                for(int c = 0; c < kWaveformCols; c++)
                    waveform_peaks_[c] = 0.f;
            }
        }

        if(state_ == LayerState::Recording)
        {
            // True stereo capture, both jacks always -- see DESIGN.md's
            // input front-end notes for why a Mic/Guitar/Line routing
            // selector was dropped: on this hardware it never did
            // anything but pick which jack(s) fed the recording, no
            // actual gain difference, so it wasn't earning its menu
            // slot. input_gain_ (SetInputGain01) is the real fix for a
            // quiet source.
            float sample_l = Clampf(mic_in * input_gain_, -1.5f, 1.5f);
            float sample_r = Clampf(guitar_in * input_gain_, -1.5f, 1.5f);

            if(write_idx_ < buffer_size_)
            {
                buffer_l_[write_idx_] = sample_l;
                buffer_r_[write_idx_] = sample_r;
                // Waveform cache: which of kWaveformCols buckets this
                // sample falls into is known up front since target_len_
                // was fixed the moment recording started -- one division
                // per sample, negligible on this hardware (Cortex-M7 has
                // a single-digit-cycle hardware integer divider).
                if(target_len_ > 0)
                {
                    int bucket = (int)(write_idx_ * (size_t)kWaveformCols / target_len_);
                    if(bucket >= kWaveformCols)
                        bucket = kWaveformCols - 1;
                    float a = fabsf(sample_l);
                    if(a > waveform_peaks_[bucket])
                        waveform_peaks_[bucket] = a;
                }
                write_idx_++;
            }
            meter_ = fabsf(sample_l) * 0.3f + meter_ * 0.7f;

            if(write_idx_ >= target_len_)
            {
                record_len_ = target_len_;
                state_      = LayerState::Playing;
                play_pos_   = 0.f;
            }
            continue; // no monitoring while recording, matches original firmware
        }

        if(state_ == LayerState::Overdubbing && record_len_ > 0)
        {
            float sample_l = mic_in * input_gain_;
            float sample_r = guitar_in * input_gain_;

            int idx0 = (int)play_pos_;
            buffer_l_[idx0] = Clampf(buffer_l_[idx0] + sample_l, -1.5f, 1.5f);
            buffer_r_[idx0] = Clampf(buffer_r_[idx0] + sample_r, -1.5f, 1.5f);

            // Waveform cache: keyed off the buffer position actually
            // touched (idx0), against record_len_ (fixed, known) rather
            // than target_len_. Only ever raises a bucket's cached peak
            // -- known simplification, not a full rescan -- since this
            // reflects what got summed in, not what the sum's magnitude
            // ends up being (which could in principle be lower if phases
            // partly cancel); peak-hold is a normal look for this kind of
            // display and self-corrects on the next full re-record.
            {
                int bucket = (int)((size_t)idx0 * (size_t)kWaveformCols / record_len_);
                if(bucket >= kWaveformCols)
                    bucket = kWaveformCols - 1;
                float a = fabsf(buffer_l_[idx0]);
                if(a > waveform_peaks_[bucket])
                    waveform_peaks_[bucket] = a;
            }
        }

        if((state_ == LayerState::Playing || state_ == LayerState::Overdubbing
            || state_ == LayerState::Paused)
           && record_len_ > 0)
        {
            // Paused still advances play_pos_ (just produces no audio) so
            // it stays phase-locked to the shared beat grid the whole
            // time it's paused, instead of freezing and then resuming
            // from a stale position that's drifted out of sync with
            // every other (still-running) layer.
            bool audible = state_ == LayerState::Playing || state_ == LayerState::Overdubbing;

            if(audible)
            {
                int   idx0 = (int)play_pos_;
                int   idx1 = (idx0 + 1) % (int)record_len_;
                float frac = play_pos_ - idx0;

                float raw_l = buffer_l_[idx0] * (1.f - frac) + buffer_l_[idx1] * frac;
                float raw_r = buffer_r_[idx0] * (1.f - frac) + buffer_r_[idx1] * frac;

                meter_ = fabsf(raw_l) * 0.3f + meter_ * 0.7f;

                float cutoff = filter_cutoff_hz_;

                float filtered_l = raw_l;
                float filtered_r = raw_r;
                if(filter_mode_ != FilterMode::Off)
                {
                    cutoff = Clampf(cutoff, kFilterMinHz, sample_rate_ / 3.f - 1.f);
                    filter_l_.SetFreq(cutoff);
                    filter_l_.SetRes(filter_res_);
                    filter_l_.Process(raw_l);
                    filter_r_.SetFreq(cutoff);
                    filter_r_.SetRes(filter_res_);
                    filter_r_.Process(raw_r);
                    switch(filter_mode_)
                    {
                        case FilterMode::LowPass:
                            filtered_l = filter_l_.Low();
                            filtered_r = filter_r_.Low();
                            break;
                        case FilterMode::HighPass:
                            filtered_l = filter_l_.High();
                            filtered_r = filter_r_.High();
                            break;
                        case FilterMode::BandPass:
                            filtered_l = filter_l_.Band();
                            filtered_r = filter_r_.Band();
                            break;
                        default: break;
                    }
                }

                float fx_l, fx_r;
                ProcessEffectsChain(filtered_l, filtered_r, fx_l, fx_r);

                // Declick: ramp in over ~8ms after a SetEffect() switch
                // instead of jumping straight to full level. Masks both
                // the ordinary discontinuity of swapping processing
                // chains and (for Phaser specifically) its delay line
                // starting cold/silent.
                if(effect_fade_ < 1.f)
                {
                    effect_fade_ += 1.f / (0.008f * sample_rate_);
                    if(effect_fade_ > 1.f)
                        effect_fade_ = 1.f;
                }
                fx_l *= effect_fade_;
                fx_r *= effect_fade_;

                // Pitch shift: independent of (and applied after) the
                // character effect above -- see SetPitchEnabled()'s
                // comment for why this isn't in ProcessEffectsChain's
                // switch. Own declick fade, same technique as effect_fade_.
                if(pitch_enabled_)
                {
                    float mono = (fx_l + fx_r) * 0.5f;
                    fx_pitchshift_->SetTransposition(pitch_amount01_ * 24.f - 12.f);
                    fx_pitchshift_->SetFun(pitch_fun01_);
                    float wet = fx_pitchshift_->Process(mono);
                    // DaisySP's PitchShifter crossfades two overlapping
                    // delay-line reads internally and isn't guaranteed to
                    // stay at unity gain doing it -- its output can come
                    // out noticeably louder than the input. Tightened from
                    // an earlier 0.6x/+-1.2 safety margin (which still let
                    // clipping-range peaks through) to stay inside normal
                    // unity range instead of past it.
                    wet = Clampf(wet * 0.5f, -1.f, 1.f);
                    if(pitch_fade_ < 1.f)
                    {
                        pitch_fade_ += 1.f / (0.008f * sample_rate_);
                        if(pitch_fade_ > 1.f)
                            pitch_fade_ = 1.f;
                    }
                    fx_l = fx_r = wet * pitch_fade_;
                }

                out[0][i] += fx_l * volume_ * panL;
                out[1][i] += fx_r * volume_ * panR;

                // This layer's own reverb (see fx_reverb_), fed by the
                // same signal at this layer's send level -- same pan so
                // the reverb return stays spatially consistent with the
                // dry mix. Only run when there's an actual send, so a
                // muted layer doesn't pay for reverb processing.
                if(reverb_send_ > 0.f)
                {
                    float send_l = fx_l * volume_ * panL * reverb_send_;
                    float send_r = fx_r * volume_ * panR * reverb_send_;
                    float wet_l, wet_r;
                    fx_reverb_->Process(send_l, send_r, &wet_l, &wet_r);
                    out[0][i] += wet_l;
                    out[1][i] += wet_r;
                }
            }
            else
            {
                meter_ *= 0.9f;
            }

            play_pos_ += speed_;
            while(play_pos_ >= (float)record_len_)
                play_pos_ -= (float)record_len_;
            while(play_pos_ < 0.f)
                play_pos_ += (float)record_len_;
        }
    }
}
