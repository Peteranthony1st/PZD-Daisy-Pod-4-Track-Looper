#include "looper_layer.h"
#include <math.h>
#include <cstring>

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
                       daisysp::PitchShifter* fx_pitchshift)
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

    // fx_phaser/fx_pitchshift live in SDRAM (see the class comment) --
    // libDaisy's own sdram.h documents that .sdram_bss is NOT zero-
    // initialized by startup code (confirmed against the actual linker
    // script/startup .s file: only ordinary .bss gets zero-filled), so
    // these objects start out holding raw leftover SDRAM contents, not
    // zero. That's harmless for most fields (Init()/SetX() below
    // overwrite what they use), but DaisySP's PitchShifter::Init()
    // itself doesn't touch several internal fields it reads on the very
    // first Process() call (prev_phs_a_/b_, mod_a_amt_/b_, mod_coeff_,
    // slewed_mod_, mod_, pitch_shift_) -- garbage there has a real chance
    // of decoding as NaN/Inf, which is self-sustaining once it appears
    // and (since every layer sends into ONE shared reverb bus) silently
    // poisons the entire mix, permanently, until a full power cycle.
    // Zeroing the whole object first is cheap insurance against this
    // exact class of bug for any current or future SDRAM-placed effect,
    // not just this one field set.
    memset(fx_phaser, 0, sizeof(*fx_phaser));
    memset(fx_pitchshift, 0, sizeof(*fx_pitchshift));
    fx_phaser_ = fx_phaser;
    fx_phaser_->Init(sample_rate_);
    fx_pitchshift_ = fx_pitchshift;
    fx_pitchshift_->Init(sample_rate_);
    // Init() leaves it at DaisySP's own default (16384 samples, ~341ms
    // latency) -- apply this layer's actual starting preset (defaults to
    // Fast, the lowest-latency option) instead of leaving that in place
    // until the user happens to cycle it.
    SetPitchDelayPreset(pitch_delay_preset_);

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
    record_write_phase_  = 0.f;
    prev_input_sample_l_ = 0.f;
    prev_input_sample_r_ = 0.f;
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
    record_write_phase_  = 0.f;
    prev_input_sample_l_ = 0.f;
    prev_input_sample_r_ = 0.f;
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
            // Fresh interpolation state -- don't blend the first sample
            // of this overdub against whatever was captured the last
            // time this layer recorded or overdubbed.
            prev_input_sample_l_ = 0.f;
            prev_input_sample_r_ = 0.f;
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
    speed_   = SpeedCurve01(speed01_);
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
                          AudioHandle::OutputBuffer reverb_send_out,
                          size_t                    size,
                          const TempoClock::TempoTick* ticks,
                          TempoClock&                  tempo,
                          float                        project_speed)
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
                // Seeded at -project_speed, not 0 -- this makes the very
                // first native slot (idx==0) always land at interpolation
                // weight t==1 (i.e. exactly the first real sample
                // captured) regardless of project_speed's value, so this
                // scheme degenerates to the original plain write_idx_++
                // behavior at project_speed==1.0. See the write loop below.
                record_write_phase_  = -project_speed;
                prev_input_sample_l_ = 0.f;
                prev_input_sample_r_ = 0.f;
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
            meter_ = fabsf(sample_l) * 0.3f + meter_ * 0.7f;

            // Vari-speed-aware write: real input arrives 1-per-real-sample
            // regardless of project_speed, but target_len_ (native-space)
            // never changes, so the write cursor must advance through
            // native space by project_speed per real sample -- exactly
            // mirroring play_pos_'s own advance below. At project_speed>1
            // one real sample can fill more than one native slot (upsample
            // via linear interpolation between this and the previous real
            // sample); at project_speed<1 several real samples pass before
            // crossing into a new native slot (decimation -- same "no
            // anti-alias filter" simplification the playback read path
            // below already accepts, just mirrored to the write side).
            // Degenerates to the original plain write_idx_++ at
            // project_speed==1.0 (first_idx==last_idx every step, t always
            // 1, one slot per real sample).
            float old_phase = record_write_phase_;
            float new_phase = old_phase + project_speed;
            int   first_idx = (int)floorf(old_phase) + 1;
            int   last_idx  = (int)floorf(new_phase);
            if(last_idx >= (int)target_len_)
                last_idx = (int)target_len_ - 1;

            for(int idx = first_idx; idx <= last_idx; idx++)
            {
                if(idx < 0)
                    continue; // pre-roll from the -project_speed seed
                float t = (idx - old_phase) / project_speed; // 0..1 across [prev, this]
                float l = prev_input_sample_l_ + t * (sample_l - prev_input_sample_l_);
                float r = prev_input_sample_r_ + t * (sample_r - prev_input_sample_r_);
                buffer_l_[idx] = l;
                buffer_r_[idx] = r;
                if(target_len_ > 0)
                {
                    int bucket = (int)((size_t)idx * (size_t)kWaveformCols / target_len_);
                    if(bucket >= kWaveformCols)
                        bucket = kWaveformCols - 1;
                    float a = fabsf(l);
                    if(a > waveform_peaks_[bucket])
                        waveform_peaks_[bucket] = a;
                }
                write_idx_ = (size_t)idx + 1;
            }
            record_write_phase_  = new_phase;
            prev_input_sample_l_ = sample_l;
            prev_input_sample_r_ = sample_r;

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

            // Same vari-speed-aware interpolated write as Recording above,
            // keyed off play_pos_ (already shared with the playback read
            // a few lines below) instead of a separate cursor -- wraps
            // instead of stopping at a target, and adds instead of
            // replacing. old_pos/new_pos are local previews only; the one
            // real play_pos_ mutation+wraparound still happens exactly
            // once, unchanged, in the shared advance block below.
            float effective_speed = speed_ * project_speed;
            float old_pos = play_pos_;
            float new_pos = old_pos + effective_speed;
            int   reclen    = (int)record_len_;
            int   first_idx = (int)floorf(old_pos) + 1;
            int   last_idx  = (int)floorf(new_pos);

            for(int idx = first_idx; idx <= last_idx; idx++)
            {
                int wrapped = idx % reclen; // a step can span the loop
                                             // boundary at high combined
                                             // speed on a short layer
                float t = (idx - old_pos) / effective_speed;
                float add_l = prev_input_sample_l_ + t * (sample_l - prev_input_sample_l_);
                float add_r = prev_input_sample_r_ + t * (sample_r - prev_input_sample_r_);
                buffer_l_[wrapped] = Clampf(buffer_l_[wrapped] + add_l, -1.5f, 1.5f);
                buffer_r_[wrapped] = Clampf(buffer_r_[wrapped] + add_r, -1.5f, 1.5f);

                // Waveform cache: same peak-hold simplification as before,
                // just now potentially touching more than one bucket per
                // real sample.
                int bucket = (int)((size_t)wrapped * (size_t)kWaveformCols / record_len_);
                if(bucket >= kWaveformCols)
                    bucket = kWaveformCols - 1;
                float a = fabsf(buffer_l_[wrapped]);
                if(a > waveform_peaks_[bucket])
                    waveform_peaks_[bucket] = a;
            }
            prev_input_sample_l_ = sample_l;
            prev_input_sample_r_ = sample_r;
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
                    // Safety net: a NaN/Inf here (e.g. from a DaisySP edge
                    // case, or any future SDRAM-placed effect that isn't
                    // fully self-initializing) would otherwise sail
                    // straight through Clampf() unchanged -- NaN compares
                    // false against both bounds in IEEE-754, so a plain
                    // min/max clamp doesn't catch it -- and flow into the
                    // shared reverb bus below, permanently poisoning its
                    // internal feedback state and silencing the entire
                    // mix until a power cycle. One branch, only on this
                    // path, to make that failure mode structurally
                    // impossible regardless of cause.
                    if(!isfinite(wet))
                        wet = 0.f;
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

                // Add this layer's Send-scaled contribution to the ONE
                // shared reverb bus (main.cpp runs the actual ReverbSc
                // once per sample, after every layer's Process() call --
                // see AudioCallback()) -- same pan so the eventual return
                // stays spatially consistent with this layer's dry mix.
                // Only accumulate when there's an actual send, same
                // "don't pay for it if it's not used" rule as before.
                if(reverb_send_ > 0.f)
                {
                    reverb_send_out[0][i] += fx_l * volume_ * panL * reverb_send_;
                    reverb_send_out[1][i] += fx_r * volume_ * panR * reverb_send_;
                }
            }
            else
            {
                meter_ *= 0.9f;
            }

            play_pos_ += speed_ * project_speed;
            while(play_pos_ >= (float)record_len_)
                play_pos_ -= (float)record_len_;
            while(play_pos_ < 0.f)
                play_pos_ += (float)record_len_;
        }
    }
}
