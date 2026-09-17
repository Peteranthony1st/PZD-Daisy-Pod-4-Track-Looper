#include "granular_engine.h"
#include "itcm.h"
#include <cmath>

using namespace daisysp;

namespace
{
inline float Clampf(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}
constexpr float kPi = 3.14159265358979323846f;

// Tiny xorshift32 PRNG for Direction::Random -- same shape the old
// engine used for its own randomization (spray jitter there; only
// Direction needs one here, Spray was dropped this redesign).
inline uint32_t NextRandom(uint32_t& state)
{
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state;
}
} // namespace

void GranularEngine::Init(float sample_rate)
{
    sample_rate_ = sample_rate;
    for(int i = 0; i < kHannTableSize; i++)
        hann_table_[i]
            = 0.5f * (1.f - cosf(2.f * kPi * (float)i / (float)(kHannTableSize - 1)));

    adsr_.Init(sample_rate);
    filter_l_.Init(sample_rate);
    filter_r_.Init(sample_rate);

    SetAttack01(attack01_);
    SetDecay01(decay01_);
    SetSustain01(sustain01_);
    SetRelease01(release01_);
    SetOutputLevel01(output_level01_);
}

void GranularEngine::SetOutputLevel01(float v01)
{
    output_level01_ = Clampf(v01, 0.f, 1.f);
    // Same shape (not the same ceiling) as LooperLayer::SetVolume01()'s
    // own curve -- deliberately diverges here. This engine's own signal
    // chain already loses real level nothing else in the project has to:
    // overlap-gain compensation (1/fill_count_, up to -6dB+ depending on
    // Fill) and the shared linear center-pan law (-6dB at center,
    // unavoidable without diverging that too) both eat into it before
    // this multiply even runs. Real hardware testing (user report: still
    // very quiet with Mixer's Grains channel at 100%, i.e. output_level01_
    // ==1) confirmed the old 1.4x ceiling didn't leave this knob enough
    // real headroom to compensate. Process()'s own tanhf() soft limiter
    // (added alongside this change) is what makes it safe to push this
    // much higher without harsh digital clipping once a user actually
    // turns this all the way up -- pushed again after a second real-
    // hardware report that 4x's ceiling was still too quiet: at the top
    // of the range this now drives tanhf() into real, deliberate
    // saturation (louder AND more compressed, not just louder) rather
    // than staying clean, which is the tradeoff needed to make "all the
    // way up" actually loud given how much this engine's own gain chain
    // (overlap-gain, center pan) eats before this multiply runs.
    output_level_ = powf(output_level01_, 2.5f) * 12.f;
}

void GranularEngine::SetPan01(float v01)
{
    pan01_      = Clampf(v01, 0.f, 1.f);
    pan_l_gain_ = 1.f - pan01_;
    pan_r_gain_ = pan01_;
}

void GranularEngine::SetSource(const float* buf_l, const float* buf_r, size_t len)
{
    src_l_   = buf_l;
    src_r_   = buf_r;
    src_len_ = len;

    // Silence both clusters outright rather than letting an in-flight
    // grain's position (computed against the OLD src_len_) carry over
    // into a shorter new buffer, where it could read out of bounds --
    // same reasoning the old engine's SetSource() used.
    grain_cluster_ = GrainCluster{};
    scan_cluster_  = GrainCluster{};
    held_note_     = -1;

    if(len == 0)
    {
        for(int i = 0; i < kWaveformCols; i++)
            waveform_peaks_[i] = 0.f;
        return;
    }

    size_t per_col = len / (size_t)kWaveformCols;
    if(per_col < 1)
        per_col = 1;
    for(int col = 0; col < kWaveformCols; col++)
    {
        size_t start = (size_t)col * per_col;
        size_t end   = start + per_col;
        if(end > len)
            end = len;
        float peak = 0.f;
        for(size_t i = start; i < end; i++)
        {
            float a = fabsf(src_l_[i]);
            if(a > peak)
                peak = a;
        }
        waveform_peaks_[col] = peak;
    }
}

void GranularEngine::NoteOn(uint8_t note, uint8_t velocity)
{
    // Monophonic in more than just pitch-tracking: a genuinely different
    // note interrupting one that's still sounding (e.g. playing a chord
    // fast enough that several NoteOns land before the first note's
    // grains finish) chokes whatever's currently ringing in both
    // clusters first, so the new note's grains don't just stack on top
    // of the old ones -- without this, fast chord-like playing sounded
    // like real polyphony even though held_note_ only ever tracks one
    // note at a time.
    if(held_note_ != -1 && held_note_ != (int)note)
    {
        ChokeCluster(grain_cluster_);
        ChokeCluster(scan_cluster_);
    }

    held_note_ = note;
    note_rate_ = powf(2.f, ((float)note - 60.f) / 12.f);
    // Square-root taper, not linear -- same reasoning the old engine's
    // NoteOn() used (hearing is roughly logarithmic).
    note_gain_ = sqrtf((float)velocity / 127.f);

    // Trigger one grain from each cluster immediately (not waiting for
    // the scheduler's own countdown), so there's no audible gap before
    // anything sounds -- same idiom the old engine's NoteOn() used.
    float dir_sign  = ResolveDirectionSign();
    float note_mult = grain_follows_note_ ? note_rate_ : 1.f;
    if(src_len_ > 0)
    {
        TriggerGrainInCluster(grain_cluster_, position01_ * (float)src_len_,
                              grain_tune_rate_ * note_mult * dir_sign, note_gain_);
        grain_cluster_.next_grain_countdown = ComputeHopSamples();

        float lo, hi;
        lo = Clampf(scan_start01_, 0.f, 1.f) * (float)src_len_;
        hi = Clampf(scan_end01_, 0.f, 1.f) * (float)src_len_;
        if(lo > hi)
        {
            float t = lo;
            lo      = hi;
            hi      = t;
        }
        scan_direction_sign_   = scan_initial_sign_;
        scan_position_samples_ = scan_initial_sign_ >= 0.f ? lo : hi;
        TriggerGrainInCluster(scan_cluster_, scan_position_samples_,
                              grain_tune_rate_ * note_mult * scan_direction_sign_, note_gain_);
        scan_cluster_.next_grain_countdown = ComputeHopSamples();
    }
}

void GranularEngine::NoteOff(uint8_t note)
{
    if(held_note_ == (int)note)
        held_note_ = -1; // Process()'s Adsr gate goes false next block, own Release begins
}

void GranularEngine::SetSize01(float v01)
{
    size01_ = Clampf(v01, 0.f, 1.f);
}

void GranularEngine::SetFill01(float v01)
{
    fill_count_ = (int)(Clampf(v01, 0.f, 1.f) * kGrainsPerVoice + 0.5f);
}

void GranularEngine::SetGap01(float v01)
{
    gap01_ = Clampf(v01, 0.f, 1.f);
}

void GranularEngine::SetPosition01(float v01)
{
    position01_ = Clampf(v01, 0.f, 1.f);
}

void GranularEngine::SetScan01(float v01)
{
    // Same dead-zone-centered bidirectional curve the old engine's
    // SetScan01() used -- center=0.5 is off, magnitude+initial-sign only,
    // the actual bounce/direction-flip state lives in Process().
    scan01_ = Clampf(v01, 0.f, 1.f);
    const float center    = 0.5f;
    const float dead_zone = 0.09f;
    if(scan01_ < center - dead_zone)
    {
        float tt           = (center - dead_zone - scan01_) / (center - dead_zone);
        scan_speed_        = tt * kMaxScanFractionPerSecond;
        scan_initial_sign_ = -1.f;
    }
    else if(scan01_ > center + dead_zone)
    {
        float tt           = (scan01_ - (center + dead_zone)) / (1.f - (center + dead_zone));
        scan_speed_        = tt * kMaxScanFractionPerSecond;
        scan_initial_sign_ = 1.f;
    }
    else
    {
        scan_speed_ = 0.f;
    }
}

void GranularEngine::SetScanStart01(float v01)
{
    scan_start01_ = Clampf(v01, 0.f, 1.f);
}

void GranularEngine::SetScanEnd01(float v01)
{
    scan_end01_ = Clampf(v01, 0.f, 1.f);
}

void GranularEngine::SetGrainTuneSemitones01(float v01)
{
    v01                    = Clampf(v01, 0.f, 1.f);
    grain_tune_semitones_  = (int)(v01 * 48.f + 0.5f) - 24; // -24..+24
    grain_tune_rate_       = powf(2.f, (float)grain_tune_semitones_ / 12.f);
}

float GranularEngine::GetGrainTuneSemitones01() const
{
    // SetGrainTuneSemitones01() rounds v01*48 to the nearest integer
    // before subtracting 24, so the 01 range that maps to a given
    // semitone S is [(S+23.5)/48, (S+24.5)/48) -- this must return a
    // value strictly inside that bucket, i.e. its center (S+24)/48, not
    // its exclusive upper edge. An earlier version added another +0.5
    // here (on top of the setter's own rounding), landing exactly on
    // that upper edge -- Capture/Save round-tripping this straight back
    // through the setter (see CapturePreset()/ApplyPreset()) then always
    // rounded up into the NEXT semitone, so a saved-and-reloaded Grains
    // preset always came back a semitone sharp.
    return (float)(grain_tune_semitones_ + 24) / 48.f;
}

void GranularEngine::SetDirection01(float v01)
{
    v01      = Clampf(v01, 0.f, 1.f);
    int mode = (int)(v01 * 3.f);
    if(mode > 2)
        mode = 2;
    direction_ = (Direction)mode;
}

float GranularEngine::GetDirection01() const
{
    return ((float)(int)direction_ + 0.5f) / 3.f;
}

float GranularEngine::ResolveDirectionSign()
{
    switch(direction_)
    {
        case Direction::Reverse: return -1.f;
        case Direction::Random:
        {
            float r01 = (float)(NextRandom(rng_state_) & 0x00FFFFFFu) / (float)0x00FFFFFFu;
            return r01 < 0.5f ? -1.f : 1.f;
        }
        case Direction::Forward:
        default: return 1.f;
    }
}

void GranularEngine::SetAttack01(float v01)
{
    attack01_ = Clampf(v01, 0.f, 1.f);
    adsr_.SetAttackTime(GetAttackSeconds());
}
void GranularEngine::SetDecay01(float v01)
{
    decay01_ = Clampf(v01, 0.f, 1.f);
    adsr_.SetDecayTime(GetDecaySeconds());
}
void GranularEngine::SetSustain01(float v01)
{
    sustain01_ = Clampf(v01, 0.f, 1.f);
    adsr_.SetSustainLevel(sustain01_);
}
void GranularEngine::SetRelease01(float v01)
{
    release01_ = Clampf(v01, 0.f, 1.f);
    adsr_.SetReleaseTime(GetReleaseSeconds());
}
float GranularEngine::GetAttackSeconds() const
{
    return kMinAdsrSeconds * powf(kMaxAdsrSeconds / kMinAdsrSeconds, attack01_);
}
float GranularEngine::GetDecaySeconds() const
{
    return kMinAdsrSeconds * powf(kMaxAdsrSeconds / kMinAdsrSeconds, decay01_);
}
float GranularEngine::GetReleaseSeconds() const
{
    return kMinAdsrSeconds * powf(kMaxAdsrSeconds / kMinAdsrSeconds, release01_);
}

float GranularEngine::GetGrainAnchor01() const
{
    return src_len_ > 0 ? position01_ : 0.f;
}
float GranularEngine::GetScanAnchor01() const
{
    return src_len_ > 0 ? Clampf(scan_position_samples_ / (float)src_len_, 0.f, 1.f) : 0.f;
}

float GranularEngine::ComputeHopSamples() const
{
    float grain_len_samples = Clampf(size01_, 0.f, 1.f) * (kMaxGrainMs - kMinGrainMs)
                               + kMinGrainMs;
    grain_len_samples       = grain_len_samples * 0.001f * sample_rate_;
    float base_hop    = fill_count_ > 0 ? grain_len_samples / (float)fill_count_
                                          : grain_len_samples;
    float sparse_mult = 1.f + gap01_ * kMaxSparseFactor;
    return base_hop * sparse_mult;
}

float GranularEngine::ReadHann(float phase01) const
{
    float pos  = Clampf(phase01, 0.f, 1.f) * (float)(kHannTableSize - 1);
    int   idx0 = (int)pos;
    int   idx1 = idx0 + 1 < kHannTableSize ? idx0 + 1 : idx0;
    float frac = pos - (float)idx0;
    return hann_table_[idx0] * (1.f - frac) + hann_table_[idx1] * frac;
}

void GranularEngine::ChokeCluster(GrainCluster& c)
{
    // Moves every currently-active grain into its own release-fade slot
    // (keyed by its own index, same as TriggerGrainInCluster()'s own
    // steal-safe hand-off) so it fades out over ~2ms instead of either
    // clicking (a hard cut) or continuing to ring out alongside a brand
    // new note's grains. Covers all kGrainsPerVoice slots, not just
    // however many fill_count_ currently uses, in case Fill was lowered
    // after a grain was triggered at a higher setting.
    for(int i = 0; i < kGrainsPerVoice; i++)
    {
        if(!c.grains[i].active)
            continue;
        c.release_grains[i] = c.grains[i];
        c.release_fades[i]  = 1.f;
        c.grains[i].active  = false;
    }
}

void GranularEngine::TriggerGrainInCluster(GrainCluster& c, float read_pos, float read_inc,
                                            float gain)
{
    // Steal whichever slot (among the first fill_count_ of them) is
    // furthest along, or an inactive one if any -- same idiom as the old
    // engine's TriggerGrainInVoice().
    int   victim      = 0;
    float worst_phase = -1.f;
    int   n           = fill_count_ > 0 ? fill_count_ : 1;
    if(n > kGrainsPerVoice)
        n = kGrainsPerVoice;
    for(int i = 0; i < n; i++)
    {
        if(!c.grains[i].active)
        {
            victim      = i;
            worst_phase = 2.f;
            break;
        }
        if(c.grains[i].phase > worst_phase)
        {
            worst_phase = c.grains[i].phase;
            victim      = i;
        }
    }

    const float kStealSafePhase = 0.85f;
    if(c.grains[victim].active && c.grains[victim].phase < kStealSafePhase)
    {
        c.release_grains[victim] = c.grains[victim];
        c.release_fades[victim]  = 1.f;
    }

    float grain_len_samples = Clampf(size01_, 0.f, 1.f) * (kMaxGrainMs - kMinGrainMs)
                               + kMinGrainMs;
    grain_len_samples       = grain_len_samples * 0.001f * sample_rate_;

    Grain& g    = c.grains[victim];
    g.active    = true;
    g.phase     = 0.f;
    g.phase_inc = grain_len_samples > 0.f ? 1.f / grain_len_samples : 1.f;
    g.read_pos  = src_len_ > 0 ? fmodf(read_pos, (float)src_len_) : 0.f;
    if(g.read_pos < 0.f && src_len_ > 0)
        g.read_pos += (float)src_len_;
    g.read_inc = read_inc;
    g.gain     = gain;
}

void GranularEngine::RenderGrain(Grain& g, float extra_gain, float& out_l, float& out_r) const
{
    int   idx0 = (int)g.read_pos;
    float frac = g.read_pos - (float)idx0;
    if(idx0 < 0)
    {
        idx0 = 0;
        frac = 0.f;
    }
    else if(idx0 >= (int)src_len_)
    {
        idx0 = (int)src_len_ - 1;
        frac = 0.f;
    }
    // Clamp, don't wrap, at the buffer's last sample -- src_l_/src_r_
    // isn't guaranteed to be a seamless loop (a captured loop layer
    // usually is, by construction, but an imported one-shot sample
    // almost never is), so interpolating straight from the last sample
    // back to sample 0 here would fabricate a real discontinuity right
    // in the middle of a grain's read, past the Hann window's own
    // fade-out at the grain's actual start/end -- audible as a click,
    // worse the more of the buffer a grain (or Scan, sweeping its
    // anchor close to both ends) reads through.
    int   idx1 = idx0 + 1 < (int)src_len_ ? idx0 + 1 : idx0;
    float l    = src_l_[idx0] * (1.f - frac) + src_l_[idx1] * frac;
    float r    = src_r_[idx0] * (1.f - frac) + src_r_[idx1] * frac;
    float env  = ReadHann(g.phase);

    out_l = l * env * g.gain * extra_gain;
    out_r = r * env * g.gain * extra_gain;

    g.read_pos += g.read_inc;
    // Same clamp-not-wrap reasoning as idx1 above, for the grain's
    // ongoing read position -- a grain that runs past either end just
    // holds at that edge (silent-ish anyway, this close to the buffer
    // boundary and to the grain's own natural end) instead of jumping to
    // the opposite end of a buffer that was never meant to loop.
    if(g.read_pos >= (float)src_len_)
        g.read_pos = (float)src_len_ - 1.f;
    else if(g.read_pos < 0.f)
        g.read_pos = 0.f;
    g.phase += g.phase_inc;
    if(g.phase >= 1.f)
        g.active = false;
}

void GranularEngine::RenderCluster(GrainCluster& c, float overlap_gain, float& out_l,
                                     float& out_r)
{
    out_l = 0.f;
    out_r = 0.f;
    int n = fill_count_ > 0 ? fill_count_ : 1;
    if(n > kGrainsPerVoice)
        n = kGrainsPerVoice;
    for(int i = 0; i < n; i++)
    {
        if(!c.grains[i].active)
            continue;
        float gl, gr;
        RenderGrain(c.grains[i], 1.f, gl, gr);
        out_l += gl;
        out_r += gr;
    }
    const float kFadeInc = 1.f / (0.002f * sample_rate_);
    for(int i = 0; i < kGrainsPerVoice; i++)
    {
        if(c.release_fades[i] <= 0.f)
            continue;
        float rl, rr;
        RenderGrain(c.release_grains[i], c.release_fades[i], rl, rr);
        out_l += rl;
        out_r += rr;
        c.release_fades[i] -= kFadeInc;
        if(c.release_fades[i] < 0.f)
            c.release_fades[i] = 0.f;
    }
    // Overlap-add gain compensation -- without this, more simultaneously
    // active grains (higher Fill) linearly increases amplitude (up to
    // kGrainsPerVoice+1 grains summing at once, near their Hann peak),
    // which is exactly what made cranking Fill and the Mix knobs together
    // clip. sqrt-based (constant-power-ish), not a full 1/n divide, so
    // Fill still sounds a bit fuller/denser, not just louder.
    out_l *= overlap_gain;
    out_r *= overlap_gain;
}

DSY_ITCM_TEXT
void GranularEngine::Process(size_t size, float* out_l, float* out_r, float* reverb_send_l,
                              float* reverb_send_r)
{
    bool has_source = src_len_ > 0;

    float cutoff_hz = kFilterMinHz * powf(kFilterMaxHz / kFilterMinHz, filter_cutoff01_);
    float nyquist_guard = sample_rate_ / 3.f - 1.f;
    cutoff_hz = cutoff_hz < kFilterMinHz ? kFilterMinHz
                : cutoff_hz > nyquist_guard ? nyquist_guard
                                            : cutoff_hz;
    filter_l_.SetFreq(cutoff_hz);
    filter_l_.SetRes(filter_res01_ * 0.9f);
    filter_r_.SetFreq(cutoff_hz);
    filter_r_.SetRes(filter_res01_ * 0.9f);

    // See RenderCluster()'s own comment -- computed once per block since
    // fill_count_ only changes when the Fill knob moves, not per sample.
    // Full 1/n (not sqrt) -- sqrt-based compensation still let loudness
    // creep up audibly as Fill increased, since it only partially offsets
    // the linear gain from more simultaneously active grains.
    int   fill_for_gain = fill_count_ > 0 ? fill_count_ : 1;
    float overlap_gain  = 1.f / (float)fill_for_gain;

    float lo = 0.f, hi = 0.f;
    if(has_source)
    {
        lo = Clampf(scan_start01_, 0.f, 1.f) * (float)src_len_;
        hi = Clampf(scan_end01_, 0.f, 1.f) * (float)src_len_;
        if(lo > hi)
        {
            float t = lo;
            lo      = hi;
            hi      = t;
        }
    }

    for(size_t i = 0; i < size; i++)
    {
        bool gate = held_note_ >= 0;
        float grain_l = 0.f, grain_r = 0.f, scan_l = 0.f, scan_r = 0.f;
        // env stays 0 (silent) when there's nothing captured -- no point
        // running the ADSR's own Process() at all in that case, since
        // grain_l/scan_l are already 0 without a source regardless of
        // what env would be.
        float env = 0.f;

        if(has_source)
        {
            env = adsr_.Process(gate);

            // Keep triggering new grains through the WHOLE release tail,
            // not just while the note is actually held -- gating this on
            // plain `gate` meant a long Release time never actually
            // produced a longer tail than whatever grain(s) happened to
            // already be in flight at the moment of NoteOff (at most one
            // grain's own Size, up to 500ms), since nothing kept feeding
            // new material for the envelope to shape. IsRunning() stays
            // true until the envelope actually reaches idle (release
            // genuinely finished), so this naturally stops triggering
            // exactly when there'd be nothing left to hear anyway --
            // confirmed via a real user report ("long release, let go of
            // the note -- should sound on, but it doesn't").
            bool still_sounding = gate || adsr_.IsRunning();

            // Grain layer: fixed anchor, just needs re-triggering on its
            // own schedule.
            grain_cluster_.next_grain_countdown -= 1.f;
            if(grain_cluster_.next_grain_countdown <= 0.f && still_sounding)
            {
                float dir_sign  = ResolveDirectionSign();
                float note_mult = grain_follows_note_ ? note_rate_ : 1.f;
                TriggerGrainInCluster(grain_cluster_, position01_ * (float)src_len_,
                                      grain_tune_rate_ * note_mult * dir_sign, note_gain_);
                grain_cluster_.next_grain_countdown += ComputeHopSamples();
            }
            RenderCluster(grain_cluster_, overlap_gain, grain_l, grain_r);

            // Scan layer -- skipped entirely (no position advance, no
            // triggering, no rendering) when its Mix volume is at/near
            // zero: a whole second grain cluster running at identical
            // cost to the main Grain layer for zero audible contribution
            // was pure waste, and was a real chunk of the measured
            // worst-case CPU. scan_position_samples_ simply holds still
            // while inactive (fine -- there's nothing to show moving on
            // the waveform display either, since it isn't audible).
            if(scan_volume01_ > 0.001f)
            {
                // Advance + bounce the anchor every sample (even when not
                // gated, so a re-triggered note picks up wherever the
                // sweep already was -- matches the old engine's own
                // per-block bounce, just now driving a whole cluster
                // instead of one grain).
                if(scan_speed_ != 0.f)
                {
                    scan_position_samples_ += scan_direction_sign_ * scan_speed_
                                               * (1.f / sample_rate_) * (float)src_len_;
                }
                if(scan_position_samples_ > hi)
                {
                    scan_position_samples_ = hi;
                    scan_direction_sign_   = -1.f;
                }
                else if(scan_position_samples_ < lo)
                {
                    scan_position_samples_ = lo;
                    scan_direction_sign_   = 1.f;
                }

                scan_cluster_.next_grain_countdown -= 1.f;
                if(scan_cluster_.next_grain_countdown <= 0.f && still_sounding)
                {
                    float note_mult = grain_follows_note_ ? note_rate_ : 1.f;
                    TriggerGrainInCluster(scan_cluster_, scan_position_samples_,
                                          grain_tune_rate_ * note_mult * scan_direction_sign_,
                                          note_gain_);
                    scan_cluster_.next_grain_countdown += ComputeHopSamples();
                }
                RenderCluster(scan_cluster_, overlap_gain, scan_l, scan_r);
            }
        }

        float mixed_l = (grain_l * grain_volume01_ + scan_l * scan_volume01_) * env;
        float mixed_r = (grain_r * grain_volume01_ + scan_r * scan_volume01_) * env;

        // Safety backstop -- the overlap-gain compensation above handles
        // the common case, but the release-fade grain, Grain+Scan summing
        // together, and both Mix knobs pushed high at once can still add
        // up past full scale. A cheap hard clamp (not a soft/tanh
        // saturator) since this runs every sample and CPU headroom is
        // already tight -- see the DWT-measured worst case.
        mixed_l = Clampf(mixed_l, -1.f, 1.f);
        mixed_r = Clampf(mixed_r, -1.f, 1.f);

        float fl = mixed_l, fr = mixed_r;
        if(filter_mode_ != FilterMode::Off)
        {
            filter_l_.Process(mixed_l);
            filter_r_.Process(mixed_r);
            switch(filter_mode_)
            {
                case FilterMode::LowPass: fl = filter_l_.Low(); fr = filter_r_.Low(); break;
                case FilterMode::HighPass: fl = filter_l_.High(); fr = filter_r_.High(); break;
                case FilterMode::BandPass: fl = filter_l_.Band(); fr = filter_r_.Band(); break;
                default: break;
            }
        }

        fl *= output_level_ * pan_l_gain_;
        fr *= output_level_ * pan_r_gain_;

        // Soft limiter -- output_level_'s own ceiling was raised well past
        // unity (see SetOutputLevel01()'s comment) specifically so the
        // Output Level knob has enough real headroom to compensate for
        // this engine's own overlap-gain and center-pan attenuation, which
        // Dexed/the loop layers don't have -- this is what makes pushing
        // that knob all the way up loud instead of just clipping. tanhf()
        // is near-transparent at normal levels and only compresses once a
        // peak actually approaches/exceeds unity, same convention as
        // DexedSynth::Process()'s own limiter.
        fl = tanhf(fl);
        fr = tanhf(fr);

        out_l[i] = fl;
        out_r[i] = fr;
        reverb_send_l[i] += fl * reverb_send01_;
        reverb_send_r[i] += fr * reverb_send01_;
    }
}

void GranularEngine::ApplyPreset(const GranularPresetData& p)
{
    SetSize01(p.size01);
    SetFill01(p.fill01);
    SetGap01(p.gap01);
    SetPosition01(p.position01);
    SetScanStart01(p.scan_start01);
    SetScanEnd01(p.scan_end01);
    SetScan01(p.scan01);
    SetGrainTuneSemitones01(p.grain_tune01);
    SetGrainFollowsNote(p.grain_follows_note);
    SetDirection01(p.direction01);
    SetAttack01(p.attack01);
    SetDecay01(p.decay01);
    SetSustain01(p.sustain01);
    SetRelease01(p.release01);
    SetFilterMode((FilterMode)p.filter_mode);
    SetFilterCutoff01(p.filter_cutoff01);
    SetFilterResonance01(p.filter_res01);
    SetGrainVolume01(p.grain_volume01);
    SetScanVolume01(p.scan_volume01);
}

GranularEngine::GranularPresetData GranularEngine::CapturePreset() const
{
    GranularPresetData p;
    p.size01              = size01_;
    p.fill01              = GetFill01();
    p.gap01               = gap01_;
    p.position01          = position01_;
    p.scan01              = scan01_;
    p.scan_start01        = scan_start01_;
    p.scan_end01          = scan_end01_;
    p.grain_tune01        = GetGrainTuneSemitones01();
    p.grain_follows_note  = grain_follows_note_;
    p.direction01         = GetDirection01();
    p.attack01            = attack01_;
    p.decay01             = decay01_;
    p.sustain01           = sustain01_;
    p.release01           = release01_;
    p.filter_mode         = (int32_t)filter_mode_;
    p.filter_cutoff01     = filter_cutoff01_;
    p.filter_res01        = filter_res01_;
    p.grain_volume01      = grain_volume01_;
    p.scan_volume01       = scan_volume01_;
    return p;
}
