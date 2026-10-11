#include "dattorro_reverb.h"
#include <cstring>

// Real delay lengths and tap offsets below are el-visio/dattorro-verb's own
// tuned magic numbers (verb.c's initialize()), kept byte-for-byte -- only
// the storage (double->float, malloc->static array) changed. See
// dattorro_reverb.h's own top-of-file comment for the full story.

namespace
{
constexpr float kPreDelayLen       = 4800.f;
constexpr float kInDiffusion0Len   = 142.f;
constexpr float kInDiffusion1Len   = 107.f;
constexpr float kInDiffusion2Len   = 379.f;
constexpr float kInDiffusion3Len   = 277.f;
constexpr float kDecayDiffusion1Len = 672.f; // half 0; half 1 uses 908 (see Init)
constexpr float kDecayDiffusion1Len1 = 908.f;
constexpr float kPreDamping0Len = 4453.f;
constexpr float kPreDamping1Len = 4217.f;
constexpr float kDecayDiffusion2Len0 = 1800.f;
constexpr float kDecayDiffusion2Len1 = 2656.f;
constexpr float kPostDamping0Len = 3720.f;
constexpr float kPostDamping1Len = 3163.f;

enum Tap
{
    kTapMain = 0,
    kTapOut1 = 1,
    kTapOut2 = 2,
    kTapOut3 = 3,
};
} // namespace

void DattorroReverb::SetTapDelay(DelayBuffer& db, int tap, uint16_t delay_samples)
{
    db.read_offset_[tap] = (uint16_t)(db.mask_ + 1 - delay_samples);
}

void DattorroReverb::InitDelayBuffer(DelayBuffer& db, float* storage, uint16_t capacity,
                                      uint16_t initial_delay_samples)
{
    db.buf_  = storage;
    db.mask_ = (uint16_t)(capacity - 1);
    for(int tap = 0; tap < kMaxTaps; tap++)
        db.read_offset_[tap] = 0;
    SetTapDelay(db, kTapMain, initial_delay_samples);
}

float DattorroReverb::ProcessDelay(DelayBuffer& db, uint16_t t, float in)
{
    db.buf_[t & db.mask_] = in;
    return db.buf_[(uint16_t)(t + db.read_offset_[kTapMain]) & db.mask_];
}

void DattorroReverb::WriteDelay(DelayBuffer& db, uint16_t t, float in)
{
    db.buf_[t & db.mask_] = in;
}

float DattorroReverb::ReadDelay(const DelayBuffer& db, int tap, uint16_t t)
{
    return db.buf_[(uint16_t)(t + db.read_offset_[tap]) & db.mask_];
}

float DattorroReverb::ProcessAllPass(DelayBuffer& db, uint16_t t, float gain, float in)
{
    float delayed = ReadDelay(db, kTapMain, t);
    float fed     = in + delayed * -gain;
    WriteDelay(db, t, fed);
    return delayed + fed * gain;
}

float DattorroReverb::ProcessLowPass(float& state, float freq, float in)
{
    state += (in - state) * freq;
    return state;
}

void DattorroReverb::Init(float /*sample_rate*/)
{
    // Hardcoded for 48kHz, see this class's own header comment.
    memset(pre_delay_storage_, 0, sizeof(pre_delay_storage_));
    memset(in_diffusion0_storage_, 0, sizeof(in_diffusion0_storage_));
    memset(in_diffusion1_storage_, 0, sizeof(in_diffusion1_storage_));
    memset(in_diffusion2_storage_, 0, sizeof(in_diffusion2_storage_));
    memset(in_diffusion3_storage_, 0, sizeof(in_diffusion3_storage_));
    memset(decay_diffusion1_storage0_, 0, sizeof(decay_diffusion1_storage0_));
    memset(decay_diffusion1_storage1_, 0, sizeof(decay_diffusion1_storage1_));
    memset(pre_damping_storage0_, 0, sizeof(pre_damping_storage0_));
    memset(pre_damping_storage1_, 0, sizeof(pre_damping_storage1_));
    memset(decay_diffusion2_storage0_, 0, sizeof(decay_diffusion2_storage0_));
    memset(decay_diffusion2_storage1_, 0, sizeof(decay_diffusion2_storage1_));
    memset(post_damping_storage0_, 0, sizeof(post_damping_storage0_));
    memset(post_damping_storage1_, 0, sizeof(post_damping_storage1_));

    InitDelayBuffer(pre_delay_, pre_delay_storage_, kPreDelayCapacity,
                     (uint16_t)kPreDelayLen);

    InitDelayBuffer(in_diffusion_[0], in_diffusion0_storage_, kInDiffusion0Capacity,
                     (uint16_t)kInDiffusion0Len);
    InitDelayBuffer(in_diffusion_[1], in_diffusion1_storage_, kInDiffusion1Capacity,
                     (uint16_t)kInDiffusion1Len);
    InitDelayBuffer(in_diffusion_[2], in_diffusion2_storage_, kInDiffusion2Capacity,
                     (uint16_t)kInDiffusion2Len);
    InitDelayBuffer(in_diffusion_[3], in_diffusion3_storage_, kInDiffusion3Capacity,
                     (uint16_t)kInDiffusion3Len);

    InitDelayBuffer(decay_diffusion1_[0], decay_diffusion1_storage0_,
                     kDecayDiffusion1Capacity, (uint16_t)kDecayDiffusion1Len);
    InitDelayBuffer(decay_diffusion1_[1], decay_diffusion1_storage1_,
                     kDecayDiffusion1Capacity, (uint16_t)kDecayDiffusion1Len1);

    InitDelayBuffer(pre_damping_[0], pre_damping_storage0_, kPreDampingCapacity,
                     (uint16_t)kPreDamping0Len);
    SetTapDelay(pre_damping_[0], kTapOut1, 353);
    SetTapDelay(pre_damping_[0], kTapOut2, 3627);
    SetTapDelay(pre_damping_[0], kTapOut3, 1990);

    InitDelayBuffer(pre_damping_[1], pre_damping_storage1_, kPreDampingCapacity,
                     (uint16_t)kPreDamping1Len);
    SetTapDelay(pre_damping_[1], kTapOut1, 266);
    SetTapDelay(pre_damping_[1], kTapOut2, 2974);
    SetTapDelay(pre_damping_[1], kTapOut3, 2111);

    InitDelayBuffer(decay_diffusion2_[0], decay_diffusion2_storage0_,
                     kDecayDiffusion2Capacity0, (uint16_t)kDecayDiffusion2Len0);
    SetTapDelay(decay_diffusion2_[0], kTapOut1, 187);
    SetTapDelay(decay_diffusion2_[0], kTapOut2, 1228);

    InitDelayBuffer(decay_diffusion2_[1], decay_diffusion2_storage1_,
                     kDecayDiffusion2Capacity1, (uint16_t)kDecayDiffusion2Len1);
    SetTapDelay(decay_diffusion2_[1], kTapOut1, 335);
    SetTapDelay(decay_diffusion2_[1], kTapOut2, 1913);

    InitDelayBuffer(post_damping_[0], post_damping_storage0_, kPostDampingCapacity,
                     (uint16_t)kPostDamping0Len);
    SetTapDelay(post_damping_[0], kTapOut1, 1066);
    SetTapDelay(post_damping_[0], kTapOut2, 2673);

    InitDelayBuffer(post_damping_[1], post_damping_storage1_, kPostDampingCapacity,
                     (uint16_t)kPostDamping1Len);
    SetTapDelay(post_damping_[1], kTapOut1, 121);
    SetTapDelay(post_damping_[1], kTapOut2, 1996);

    pre_filter_state_ = 0.f;
    damping_state_[0] = 0.f;
    damping_state_[1] = 0.f;
    t_                = 0;

    // Same default values as the original library's own initialize()
    // (verb.c lines 226-232) -- set via the real setters, not just member
    // initializers, so derived fields like decay_diffusion2_amount_ come
    // out consistent with SetDecay01()'s own clamp logic.
    SetPreDelay01(0.1f);
    SetPreFilter01(0.85f);
    SetInputDiffusion1_01(0.75f);
    SetInputDiffusion2_01(0.625f);
    SetDecay01(0.75f);
    SetDecayDiffusion01(0.70f);
    SetDamping01(0.05f); // -> damping_amount_ = 0.95, matching the original's default
}

void DattorroReverb::SetDecay01(float v01)
{
    decay_amount_ = v01;
    float dd2     = v01 + 0.15f;
    if(dd2 < 0.25f)
        dd2 = 0.25f;
    if(dd2 > 0.50f)
        dd2 = 0.50f;
    decay_diffusion2_amount_ = dd2;
}

void DattorroReverb::SetDamping01(float v01)
{
    // Inverted -- see this setter's own doc comment in dattorro_reverb.h.
    damping_amount_ = 1.f - v01;
}

void DattorroReverb::SetPreDelay01(float v01)
{
    uint16_t delay = (uint16_t)(v01 * (float)kMaxPreDelaySamples);
    SetTapDelay(pre_delay_, kTapMain, delay);
}

void DattorroReverb::SetPreFilter01(float v01)
{
    pre_filter_amount_ = v01;
}

void DattorroReverb::SetInputDiffusion1_01(float v01)
{
    input_diffusion1_amount_ = v01;
}

void DattorroReverb::SetInputDiffusion2_01(float v01)
{
    input_diffusion2_amount_ = v01;
}

void DattorroReverb::SetDecayDiffusion01(float v01)
{
    decay_diffusion1_amount_ = v01;
}

void DattorroReverb::Process(float in1, float in2, float* out1, float* out2)
{
    // Mono-sum into the tank -- see this class's own header comment on why.
    float in = (in1 + in2) * 0.5f;

    // Decay diffusion 1's ±1-sample modulation, same cadence/shape as the
    // original (verb.c's own process() function, the only place either
    // library ever moves a read offset per-sample instead of per-Set call).
    if((t_ & 0x07ffu) == 0)
    {
        if((t_ & 0x8000u) == 0)
        {
            decay_diffusion1_[0].read_offset_[kTapMain]--;
            decay_diffusion1_[1].read_offset_[kTapMain]--;
        }
        else
        {
            decay_diffusion1_[0].read_offset_[kTapMain]++;
            decay_diffusion1_[1].read_offset_[kTapMain]++;
        }
    }

    float x = ProcessDelay(pre_delay_, t_, in);
    x       = ProcessLowPass(pre_filter_state_, pre_filter_amount_, x);

    x = ProcessAllPass(in_diffusion_[0], t_, input_diffusion1_amount_, x);
    x = ProcessAllPass(in_diffusion_[1], t_, input_diffusion1_amount_, x);
    x = ProcessAllPass(in_diffusion_[2], t_, input_diffusion2_amount_, x);
    x = ProcessAllPass(in_diffusion_[3], t_, input_diffusion2_amount_, x);

    for(int i = 0; i < 2; i++)
    {
        int   other = 1 - i;
        float x1    = x + ReadDelay(post_damping_[other], kTapMain, t_) * decay_amount_;
        x1          = ProcessAllPass(decay_diffusion1_[i], t_, -decay_diffusion1_amount_, x1);
        x1          = ProcessDelay(pre_damping_[i], t_, x1);
        x1          = ProcessLowPass(damping_state_[i], damping_amount_, x1);
        x1 *= decay_amount_;
        x1 = ProcessAllPass(decay_diffusion2_[i], t_, decay_diffusion2_amount_, x1);
        WriteDelay(post_damping_[i], t_, x1);
    }

    // The original library increments its own `t` before the caller reads
    // getLeft()/getRight() (they're separate public calls, made only after
    // DattorroVerb_process() has already done `v->t++`) -- so its output
    // taps are always read one position ahead of the t used for this same
    // cycle's tank update. Matched here exactly: increment first, then
    // read, rather than reading with the pre-increment t_.
    t_++;

    *out1 = ReadDelay(pre_damping_[1], kTapOut1, t_)
            + ReadDelay(pre_damping_[1], kTapOut2, t_)
            - ReadDelay(decay_diffusion2_[1], kTapOut2, t_)
            + ReadDelay(post_damping_[1], kTapOut2, t_)
            - ReadDelay(pre_damping_[0], kTapOut3, t_)
            - ReadDelay(decay_diffusion2_[0], kTapOut1, t_)
            + ReadDelay(post_damping_[0], kTapOut1, t_);

    *out2 = ReadDelay(pre_damping_[0], kTapOut1, t_)
            + ReadDelay(pre_damping_[0], kTapOut2, t_)
            - ReadDelay(decay_diffusion2_[0], kTapOut2, t_)
            + ReadDelay(post_damping_[0], kTapOut2, t_)
            - ReadDelay(pre_damping_[1], kTapOut3, t_)
            - ReadDelay(decay_diffusion2_[1], kTapOut1, t_)
            + ReadDelay(post_damping_[1], kTapOut1, t_);
}
