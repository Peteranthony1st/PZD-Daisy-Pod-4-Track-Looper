#include "pad_synth.h"
#include "itcm.h"

using namespace daisysp;

namespace
{
inline float Clampf(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

// SetAmplitudes()'s 7 slots (oscillatorbank.h): Saw8'/Sq8'/Saw4'/Sq4'/
// Saw2'/Sq2'/Saw1' -- "must sum to 1". Dark leans on the low registers
// (warm, full-bodied); Bright leans on the high ones (airy, present).
// SetRegistration01() lerps between the two, which stays sum-to-1 for
// any blend since both endpoints already sum to 1.
constexpr float kDarkRegistration[7]
    = {0.30f, 0.25f, 0.20f, 0.15f, 0.06f, 0.03f, 0.01f};
constexpr float kBrightRegistration[7]
    = {0.05f, 0.05f, 0.15f, 0.15f, 0.25f, 0.20f, 0.15f};

const char* kFactoryPresetNames[PadSynth::kNumFactoryPresets] = {
    "New",
    "Warm Pad",
    "Bright Ensemble",
    "Slow Swell",
    "Glassy Strings",
    "Deep Drone",
    "Vintage Organ",
    "Airy Chorus Wash",
    "Soft Felt",
    "Lush Wide Pad",
    "Punchy Bass",
    "Sub Bass",
    "Synth Lead",
    "Screamer Lead",
};

// registration01, osc_gain01, attack01, decay01, sustain01, release01,
// chorus_depth01, chorus_rate01, filter_mode, filter_cutoff01,
// filter_res01, reverb_send01, output_level01, mod_destination,
// vibrato_depth01, vibrato_rate01.
// attack/decay/release01 run through the same curved-seconds mapping
// GetAttackSeconds() etc. use (kMinAdsrSeconds..kMaxAdsrSeconds,
// exponential) -- e.g. 0.2 ~= 18ms, 0.5 ~= 0.12s, 0.9 ~= 1.7s. First-pass
// values, meant to be ear-tuned on real hardware, not treated as final.
const PadSynth::PadPresetData kFactoryPresets[PadSynth::kNumFactoryPresets] = {
    // New -- neutral baseline, nothing added.
    {0.5f, 0.8f, 0.3f, 0.3f, 0.8f, 0.4f, 0.f, 0.3f, (int32_t)FilterMode::Off, 1.f, 0.f, 0.2f,
     0.7f, (int32_t)PadSynth::ModDestination::Vibrato, 0.3f, 0.4f},
    // Warm Pad -- dark registration, soft attack, gentle chorus, filter
    // rolled back for warmth.
    {0.15f, 0.8f, 0.75f, 0.5f, 0.85f, 0.7f, 0.3f, 0.25f, (int32_t)FilterMode::LowPass, 0.6f,
     0.15f, 0.3f, 0.7f, (int32_t)PadSynth::ModDestination::Vibrato, 0.3f, 0.4f},
    // Bright Ensemble -- bright registration, quicker attack, more
    // chorus movement (the "detune" character on this and every other
    // bright/ensemble-leaning preset comes entirely from Chorus's
    // modulated delay lines beating against the dry signal, plus
    // OscillatorBank's own multi-octave registration -- there's no
    // actual separate detuned oscillator; see the answer this comment's
    // change came with for the full explanation).
    {0.85f, 0.85f, 0.5f, 0.4f, 0.75f, 0.5f, 0.6f, 0.4f, (int32_t)FilterMode::Off, 1.f, 0.f, 0.25f,
     0.7f, (int32_t)PadSynth::ModDestination::Vibrato, 0.3f, 0.4f},
    // Slow Swell -- long attack/release, minimal chorus, warm filter.
    {0.35f, 0.75f, 0.92f, 0.6f, 0.9f, 0.9f, 0.15f, 0.2f, (int32_t)FilterMode::LowPass, 0.7f, 0.1f,
     0.35f, 0.65f, (int32_t)PadSynth::ModDestination::Vibrato, 0.3f, 0.4f},
    // Glassy Strings -- bright, high-pass-leaning, subtle chorus.
    {0.75f, 0.8f, 0.4f, 0.35f, 0.7f, 0.55f, 0.25f, 0.35f, (int32_t)FilterMode::HighPass, 0.3f,
     0.1f, 0.3f, 0.7f, (int32_t)PadSynth::ModDestination::ChorusDepth, 0.3f, 0.4f},
    // Deep Drone -- very slow attack/release, dark/sub-heavy, filter closed.
    // Output level pushed up to compensate for how much energy the dark
    // registration + closed filter both remove -- without this it reads
    // as barely audible even though the raw settings "look" moderate.
    {0.05f, 0.9f, 0.95f, 0.7f, 0.95f, 0.95f, 0.1f, 0.15f, (int32_t)FilterMode::LowPass, 0.45f,
     0.2f, 0.4f, 0.85f, (int32_t)PadSynth::ModDestination::Vibrato, 0.3f, 0.4f},
    // Vintage Organ -- fast attack, full registration, light chorus, no
    // big release tail -- the divide-down "string synth" character this
    // oscillator is modeled on.
    {0.5f, 0.9f, 0.15f, 0.15f, 1.f, 0.2f, 0.2f, 0.3f, (int32_t)FilterMode::Off, 1.f, 0.f, 0.15f,
     0.75f, (int32_t)PadSynth::ModDestination::Vibrato, 0.3f, 0.4f},
    // Airy Chorus Wash -- max chorus depth/rate, mid registration, long release.
    {0.6f, 0.75f, 0.7f, 0.5f, 0.8f, 0.85f, 0.9f, 0.6f, (int32_t)FilterMode::Off, 1.f, 0.f, 0.35f,
     0.65f, (int32_t)PadSynth::ModDestination::ChorusDepth, 0.3f, 0.4f},
    // Soft Felt -- very dark/filtered, slow attack. "Quiet" describes the
    // felt-muted attack character, not the actual output level -- same
    // headroom fix as Deep Drone above.
    {0.1f, 0.75f, 0.85f, 0.6f, 0.8f, 0.75f, 0.1f, 0.2f, (int32_t)FilterMode::LowPass, 0.35f, 0.05f,
     0.3f, 0.8f, (int32_t)PadSynth::ModDestination::Vibrato, 0.3f, 0.4f},
    // Lush Wide Pad -- balanced registration, generous chorus, moderate
    // everything -- the signature showcase pad.
    {0.45f, 0.8f, 0.65f, 0.45f, 0.82f, 0.65f, 0.55f, 0.35f, (int32_t)FilterMode::LowPass, 0.75f,
     0.1f, 0.35f, 0.7f, (int32_t)PadSynth::ModDestination::FilterCutoff, 0.3f, 0.4f},
    // Punchy Bass -- dark/fundamental-heavy registration, near-instant
    // attack with a quick decay/release for a plucked/percussive feel,
    // no chorus (keeps the low end tight/mono instead of smeared), a
    // resonant low-pass for character, dry (bass usually wants minimal
    // reverb so it doesn't turn to mud).
    {0.2f, 0.9f, 0.05f, 0.35f, 0.5f, 0.25f, 0.f, 0.2f, (int32_t)FilterMode::LowPass, 0.5f, 0.35f,
     0.1f, 0.9f, (int32_t)PadSynth::ModDestination::FilterCutoff, 0.1f, 0.4f},
    // Sub Bass -- maximally dark (pure low fundamental, minimal upper
    // harmonics), filter closed further still for a deep, clean sub tone.
    {0.f, 0.9f, 0.1f, 0.3f, 0.7f, 0.3f, 0.f, 0.2f, (int32_t)FilterMode::LowPass, 0.3f, 0.15f,
     0.05f, 0.95f, (int32_t)PadSynth::ModDestination::Vibrato, 0.05f, 0.3f},
    // Synth Lead -- bright registration to cut through a mix, fast
    // attack, mostly sustained (a lead is played melodically, not held
    // as a chord pad), some chorus for width, a resonant filter for
    // bite. Vibrato depth set high since mod-wheel vibrato is the
    // classic lead-playing gesture.
    {0.7f, 0.85f, 0.1f, 0.2f, 0.95f, 0.35f, 0.35f, 0.4f, (int32_t)FilterMode::LowPass, 0.85f,
     0.25f, 0.2f, 0.85f, (int32_t)PadSynth::ModDestination::Vibrato, 0.5f, 0.5f},
    // Screamer Lead -- maximally bright, fast/snappy envelope, band-pass
    // with heavier resonance for an aggressive, cutting lead tone.
    {0.95f, 0.9f, 0.05f, 0.15f, 1.f, 0.2f, 0.2f, 0.45f, (int32_t)FilterMode::BandPass, 0.7f, 0.5f,
     0.25f, 0.85f, (int32_t)PadSynth::ModDestination::Vibrato, 0.6f, 0.6f},
};
} // namespace

const char* PadSynth::GetFactoryPresetName(int index)
{
    if(index < 0 || index >= kNumFactoryPresets)
        return "?";
    return kFactoryPresetNames[index];
}

PadSynth::PadPresetData PadSynth::GetFactoryPreset(int index)
{
    if(index < 0 || index >= kNumFactoryPresets)
        return PadPresetData{};
    return kFactoryPresets[index];
}

void PadSynth::Init(float sample_rate)
{
    sample_rate_ = sample_rate;

    for(int i = 0; i < kMaxVoices; i++)
    {
        voices_[i].osc.Init(sample_rate);
        voices_[i].adsr.Init(sample_rate);
        voices_[i].held_note    = -1;
        voices_[i].base_hz      = 0.f;
        voices_[i].triggered_at = 0;
    }

    chorus_.Init(sample_rate);
    // Delay time/feedback aren't exposed as their own knobs (see
    // PadParamPage::Chorus -- just Depth + Rate); fixed at values that
    // sound like ensemble chorus rather than flanging/comb-filtering.
    chorus_.SetDelayMs(15.f);
    chorus_.SetFeedback(0.15f);

    filter_l_.Init(sample_rate);
    filter_r_.Init(sample_rate);

    // Applies every setter in one place, from the SAME data (factory
    // preset 0, "New") that the Preset page's list shows -- rather than
    // this Init() and that preset table separately hand-coding matching
    // "default" numbers that could quietly drift apart over time.
    ApplyPreset(GetFactoryPreset(0));
}

int PadSynth::FindVoiceForNote(uint8_t note)
{
    for(int i = 0; i < kMaxVoices; i++)
        if(voices_[i].held_note == (int)note)
            return i;

    // Prefer a free voice that's fully silent (ADSR_SEG_IDLE) over one
    // still fading out in Release -- least audible retrigger.
    int  best_free    = -1;
    bool best_is_idle = false;
    for(int i = 0; i < kMaxVoices; i++)
    {
        if(voices_[i].held_note != -1)
            continue;
        bool idle = voices_[i].adsr.GetCurrentSegment() == ADSR_SEG_IDLE;
        if(best_free < 0 || (idle && !best_is_idle))
        {
            best_free    = i;
            best_is_idle = idle;
        }
    }
    if(best_free >= 0)
        return best_free;

    // Every voice genuinely held -- steal the oldest (standard
    // oldest-note voice stealing).
    int oldest = 0;
    for(int i = 1; i < kMaxVoices; i++)
        if(voices_[i].triggered_at < voices_[oldest].triggered_at)
            oldest = i;
    return oldest;
}

void PadSynth::NoteOn(uint8_t note, uint8_t velocity)
{
    (void)velocity; // no velocity mapping yet -- SetOutputLevel01 covers overall level
    int    vi              = FindVoiceForNote(note);
    Voice& v                = voices_[vi];
    bool   retrigger_same   = (v.held_note == (int)note);

    v.held_note    = note;
    v.base_hz      = 440.f * powf(2.f, ((float)note - 69.f) / 12.f);
    v.triggered_at = ++trigger_seq_;
    if(mod_dest_ != ModDestination::Vibrato)
        v.osc.SetFreq(v.base_hz * bend_ratio_);

    // A genuinely new/stolen voice picks up Attack naturally next
    // Process() call (its Adsr's internal gate_ is already false, so
    // gate=true is a fresh rising edge) -- only an explicit re-strike of
    // the SAME still-held note needs a forced retrigger.
    if(retrigger_same)
        v.adsr.Retrigger(false);
}

void PadSynth::NoteOff(uint8_t note)
{
    for(int i = 0; i < kMaxVoices; i++)
        if(voices_[i].held_note == (int)note)
            voices_[i].held_note = -1;
}

void PadSynth::SetPitchBendSemis(float semis)
{
    pitch_bend_semis_ = semis;
    bend_ratio_       = powf(2.f, semis / 12.f);
}

void PadSynth::SetModWheel01(float v01)
{
    mod_wheel01_ = Clampf(v01, 0.f, 1.f);
}

void PadSynth::ApplyRegistrationToAllVoices()
{
    float reg[7];
    for(int i = 0; i < 7; i++)
        reg[i] = kDarkRegistration[i]
                 + (kBrightRegistration[i] - kDarkRegistration[i]) * registration01_;
    for(int v = 0; v < kMaxVoices; v++)
    {
        voices_[v].osc.SetAmplitudes(reg);
        voices_[v].osc.SetGain(osc_gain01_);
    }
}

void PadSynth::SetRegistration01(float v01)
{
    registration01_ = Clampf(v01, 0.f, 1.f);
    ApplyRegistrationToAllVoices();
}

void PadSynth::SetOscGain01(float v01)
{
    osc_gain01_ = Clampf(v01, 0.f, 1.f);
    ApplyRegistrationToAllVoices();
}

void PadSynth::ApplyEnvelopeTimesToAllVoices()
{
    float attack_s  = kMinAdsrSeconds * powf(kMaxAdsrSeconds / kMinAdsrSeconds, attack01_);
    float decay_s   = kMinAdsrSeconds * powf(kMaxAdsrSeconds / kMinAdsrSeconds, decay01_);
    float release_s = kMinAdsrSeconds * powf(kMaxAdsrSeconds / kMinAdsrSeconds, release01_);
    for(int v = 0; v < kMaxVoices; v++)
    {
        voices_[v].adsr.SetAttackTime(attack_s);
        voices_[v].adsr.SetDecayTime(decay_s);
        voices_[v].adsr.SetSustainLevel(sustain01_);
        voices_[v].adsr.SetReleaseTime(release_s);
    }
}

void PadSynth::SetAttack01(float v01)
{
    attack01_ = Clampf(v01, 0.f, 1.f);
    ApplyEnvelopeTimesToAllVoices();
}
void PadSynth::SetDecay01(float v01)
{
    decay01_ = Clampf(v01, 0.f, 1.f);
    ApplyEnvelopeTimesToAllVoices();
}
void PadSynth::SetSustain01(float v01)
{
    sustain01_ = Clampf(v01, 0.f, 1.f);
    ApplyEnvelopeTimesToAllVoices();
}
void PadSynth::SetRelease01(float v01)
{
    release01_ = Clampf(v01, 0.f, 1.f);
    ApplyEnvelopeTimesToAllVoices();
}

float PadSynth::GetAttackSeconds() const
{
    return kMinAdsrSeconds * powf(kMaxAdsrSeconds / kMinAdsrSeconds, attack01_);
}
float PadSynth::GetDecaySeconds() const
{
    return kMinAdsrSeconds * powf(kMaxAdsrSeconds / kMinAdsrSeconds, decay01_);
}
float PadSynth::GetReleaseSeconds() const
{
    return kMinAdsrSeconds * powf(kMaxAdsrSeconds / kMinAdsrSeconds, release01_);
}

void PadSynth::SetChorusDepth01(float v01)
{
    chorus_depth01_ = Clampf(v01, 0.f, 1.f);
}
void PadSynth::SetChorusRate01(float v01)
{
    chorus_rate01_ = Clampf(v01, 0.f, 1.f);
}

void PadSynth::SetOutputLevel01(float v01)
{
    output_level01_ = Clampf(v01, 0.f, 1.f);
    output_level_   = powf(output_level01_, 2.5f) * 1.4f; // same curve as LooperLayer::SetVolume01
    if(output_level_ < 0.f)
        output_level_ = 0.f;
}

bool PadSynth::IsVoiceActive(int i) const
{
    if(i < 0 || i >= kMaxVoices)
        return false;
    return voices_[i].held_note >= 0 || voices_[i].adsr.IsRunning();
}

void PadSynth::ApplyPreset(const PadPresetData& p)
{
    SetRegistration01(p.registration01);
    SetOscGain01(p.osc_gain01);
    SetAttack01(p.attack01);
    SetDecay01(p.decay01);
    SetSustain01(p.sustain01);
    SetRelease01(p.release01);
    SetChorusDepth01(p.chorus_depth01);
    SetChorusRate01(p.chorus_rate01);
    SetFilterMode((FilterMode)p.filter_mode);
    SetFilterCutoff01(p.filter_cutoff01);
    SetFilterResonance01(p.filter_res01);
    SetReverbSend01(p.reverb_send01);
    SetOutputLevel01(p.output_level01);
    SetModDestination((ModDestination)p.mod_destination);
    SetVibratoDepth01(p.vibrato_depth01);
    SetVibratoRate01(p.vibrato_rate01);
}

PadSynth::PadPresetData PadSynth::CapturePreset() const
{
    PadPresetData p;
    p.registration01 = registration01_;
    p.osc_gain01      = osc_gain01_;
    p.attack01        = attack01_;
    p.decay01         = decay01_;
    p.sustain01       = sustain01_;
    p.release01       = release01_;
    p.chorus_depth01  = chorus_depth01_;
    p.chorus_rate01   = chorus_rate01_;
    p.filter_mode     = (int32_t)filter_mode_;
    p.filter_cutoff01 = filter_cutoff01_;
    p.filter_res01    = filter_res01_;
    p.reverb_send01   = reverb_send01_;
    p.output_level01  = output_level01_;
    p.mod_destination = (int32_t)mod_dest_;
    p.vibrato_depth01 = vibrato_depth01_;
    p.vibrato_rate01  = vibrato_rate01_;
    return p;
}

DSY_ITCM_TEXT
void PadSynth::Process(size_t size,
                        float* out_l,
                        float* out_r,
                        float* reverb_send_l,
                        float* reverb_send_r)
{
    // Block-rate: bus filter cutoff/resonance and chorus depth both take
    // the mod wheel's ADDITIVE contribution here if that's the current
    // destination. Vibrato is handled per-sample below instead, since it
    // needs the LFO's continuously-changing value, not a single
    // per-block number.
    float filter_cutoff01_eff = filter_cutoff01_;
    float chorus_depth01_eff  = chorus_depth01_;
    if(mod_dest_ == ModDestination::FilterCutoff)
        filter_cutoff01_eff = Clampf(filter_cutoff01_ + mod_wheel01_ * 0.5f, 0.f, 1.f);
    else if(mod_dest_ == ModDestination::ChorusDepth)
        chorus_depth01_eff = Clampf(chorus_depth01_ + mod_wheel01_ * 0.5f, 0.f, 1.f);

    // Same nyquist-guarded cutoff curve as main.cpp's master filter, so
    // this bus filter feels consistent with every other filter in the
    // project.
    float cutoff_hz = kFilterMinHz * powf(kFilterMaxHz / kFilterMinHz, filter_cutoff01_eff);
    float nyquist_guard = sample_rate_ / 3.f - 1.f;
    cutoff_hz = cutoff_hz < kFilterMinHz ? kFilterMinHz
                : cutoff_hz > nyquist_guard ? nyquist_guard
                                            : cutoff_hz;
    filter_l_.SetFreq(cutoff_hz);
    filter_l_.SetRes(filter_res01_ * 0.9f);
    filter_r_.SetFreq(cutoff_hz);
    filter_r_.SetRes(filter_res01_ * 0.9f);

    chorus_.SetLfoDepth(chorus_depth01_eff);
    chorus_.SetLfoFreq(0.05f + chorus_rate01_ * 3.f);

    // Vibrato's own Depth/Rate knobs (PadParamPage::Vibrato) -- block-rate,
    // same as everything else above.
    float vibrato_lfo_hz = kVibratoMinRateHz + vibrato_rate01_ * (kVibratoMaxRateHz - kVibratoMinRateHz);
    float vibrato_depth_fraction = vibrato_depth01_ * kVibratoMaxDepthFraction;

    // Non-vibrato voices only need their frequency recomputed once per
    // block (pitch bend is control-rate); vibrato voices recompute every
    // sample below instead, since the LFO changes continuously.
    if(mod_dest_ != ModDestination::Vibrato)
    {
        for(int v = 0; v < kMaxVoices; v++)
            if(voices_[v].held_note >= 0)
                voices_[v].osc.SetFreq(voices_[v].base_hz * bend_ratio_);
    }

    for(size_t i = 0; i < size; i++)
    {
        // Cheap triangle LFO (no transcendentals) shared by every voice
        // -- computed once per SAMPLE, not once per voice, since it's
        // the same modulator applied to all of them.
        float lfo_val = 0.f;
        if(mod_dest_ == ModDestination::Vibrato && mod_wheel01_ > 0.f)
        {
            lfo_phase_ += vibrato_lfo_hz / sample_rate_;
            if(lfo_phase_ >= 1.f)
                lfo_phase_ -= 1.f;
            lfo_val = lfo_phase_ < 0.5f ? (lfo_phase_ * 4.f - 1.f) : (3.f - lfo_phase_ * 4.f);
        }

        float voice_sum = 0.f;
        for(int v = 0; v < kMaxVoices; v++)
        {
            Voice& voice = voices_[v];
            bool   gate  = voice.held_note >= 0;
            float  env   = voice.adsr.Process(gate);
            if(mod_dest_ == ModDestination::Vibrato)
            {
                // Linear FM, not exponential/powf-based -- cheap enough
                // to run every sample for every voice (confirmed: no
                // transcendentals in OscillatorBank::SetFreq/Process).
                voice.osc.SetFreq(voice.base_hz * bend_ratio_
                                    * (1.f + lfo_val * mod_wheel01_ * vibrato_depth_fraction));
            }
            voice_sum += voice.osc.Process() * env;
        }

        chorus_.Process(voice_sum);
        float cl = chorus_.GetLeft();
        float cr = chorus_.GetRight();

        float fl = cl, fr = cr;
        if(filter_mode_ != FilterMode::Off)
        {
            filter_l_.Process(cl);
            filter_r_.Process(cr);
            switch(filter_mode_)
            {
                case FilterMode::LowPass: fl = filter_l_.Low(); fr = filter_r_.Low(); break;
                case FilterMode::HighPass: fl = filter_l_.High(); fr = filter_r_.High(); break;
                case FilterMode::BandPass: fl = filter_l_.Band(); fr = filter_r_.Band(); break;
                default: break;
            }
        }

        out_l[i] = fl * output_level_;
        out_r[i] = fr * output_level_;
        reverb_send_l[i] += out_l[i] * reverb_send01_;
        reverb_send_r[i] += out_r[i] * reverb_send01_;
    }
}
