#pragma once
#include <cstdint>

// A real Dattorro figure-8 plate reverb (Jon Dattorro, "Effect Design
// Part 1: Reverberator and Other Filters", 1997), ported from
// el-visio/dattorro-verb (https://github.com/el-visio/dattorro-verb,
// MIT License, Copyright (c) 2022 Pauli Pölkki) -- the real algorithm
// and its real tuned magic numbers (delay lengths, tap offsets, default
// coefficients) are kept exactly as that implementation has them;
// what's changed here is purely mechanical, to match this project's
// own conventions:
//   - `double` -> `float` throughout (this project's native audio type)
//   - `malloc()`'d delay buffers -> fixed-size member arrays (nothing
//     in this project's audio path uses heap allocation; the whole
//     object is meant to be declared as one DSY_SDRAM_BSS static
//     instance by its caller, same as daisysp::ReverbSc already is)
//   - public API reshaped to match daisysp::ReverbSc's own call shape
//     (Init(sample_rate) + Process(in1, in2, &out1, &out2) + setters)
//     so callers need no restructuring to swap one for the other
// See DESIGN.md's own Reverb section for the full story on why this
// replaced ReverbSc as the shared bus engine.
//
// Mono-in, stereo-out by design (in1+in2 are summed before entering the
// tank) -- this is how the real algorithm (and a real physical plate,
// one exciter, stereo pickup) is meant to work, not a limitation of
// this port specifically.
//
// Hardcoded for 48kHz, same as every other engine in this project --
// the original library's own magic numbers (e.g. the 100ms pre-delay
// ceiling) are tuned for exactly this rate and were never actually
// parameterized by sample rate in the original either.
class DattorroReverb
{
  public:
    // sample_rate is accepted for API-shape parity with
    // daisysp::ReverbSc::Init() but not otherwise used -- see this
    // class's own doc comment above.
    void Init(float sample_rate);

    void Process(float in1, float in2, float* out1, float* out2);

    // Reverb length/feedback -- the direct continuation of this
    // project's existing "Size" knob (Ui::GetReverbSize01()). No
    // documented safe ceiling in the original library the way
    // ReverbSc::SetFeedback() has one ("becomes infinite at 1.0") --
    // verify the top of this range on real hardware before trusting it.
    void SetDecay01(float v01);

    // Tail brightness-over-time. Inverted from the original library's
    // own convention (there, a higher value means LESS damping/
    // brighter) so this knob reads the normal hardware-reverb way:
    // higher = more damping = darker.
    void SetDamping01(float v01);

    // Gap before the tail starts, 0..1 scaled against this port's own
    // ~100ms ceiling (kMaxPreDelaySamples), same as the original.
    void SetPreDelay01(float v01);

    // One-pole lowpass on the signal before it ever enters the tank --
    // an input tone/brightness control, independent of Damping above.
    void SetPreFilter01(float v01);

    // The 4 input all-pass diffusers' own coefficients, split the same
    // way the original library splits them (diffusers 0+1 share one
    // amount, diffusers 2+3 share the other) -- controls how smeared/
    // soft the attack transient is going into the reverb.
    void SetInputDiffusion1_01(float v01);
    void SetInputDiffusion2_01(float v01);

    // All-pass diffusion inside the feedback tank itself -- shapes the
    // density/smoothness of the tail specifically, separate from the
    // input diffusers above. Maps to the original's own
    // decayDiffusion1Amount; decayDiffusion2Amount stays internally
    // derived from Decay (see SetDecay01()), matching the original
    // library's own design -- not separately exposed.
    void SetDecayDiffusion01(float v01);

  private:
    // TAP_MAIN/OUT1/OUT2/OUT3, matching the original's own
    // verb_structs.h enum and tap count exactly.
    static constexpr int kMaxTaps = 4;

    // A circular buffer view over one of this class's own fixed member
    // arrays (set once, in Init()) -- mask for fast wraparound, up to
    // kMaxTaps independent read offsets per buffer, same shape the
    // original's own DelayBuffer struct uses (just without an owned
    // pointer -- buf_ here always points at one of this class's own
    // static arrays, never malloc'd).
    // Deliberately a plain aggregate (no default member initializers) --
    // Init() always sets every field explicitly before use, and keeping
    // this (and DattorroReverb itself) trivial avoids a spurious
    // -Wclass-memaccess on the memset-then-Init() pattern this project
    // already uses for every other DSY_SDRAM_BSS effect object.
    struct DelayBuffer
    {
        float*   buf_;
        uint16_t mask_;
        uint16_t read_offset_[kMaxTaps];
    };

    static void SetTapDelay(DelayBuffer& db, int tap, uint16_t delay_samples);
    static void InitDelayBuffer(DelayBuffer& db, float* storage, uint16_t capacity,
                                  uint16_t initial_delay_samples);
    static float ProcessDelay(DelayBuffer& db, uint16_t t, float in);
    static void  WriteDelay(DelayBuffer& db, uint16_t t, float in);
    static float ReadDelay(const DelayBuffer& db, int tap, uint16_t t);
    static float ProcessAllPass(DelayBuffer& db, uint16_t t, float gain, float in);
    static float ProcessLowPass(float& state, float freq, float in);

    // Real buffer capacities -- each the original's own real delay
    // length (see dattorro_reverb.cpp's own Init()) rounded up to the
    // next power of 2, matching the original's own DelayBuffer_init()
    // bit-counting exactly. ~42,368 floats total (~166KB) -- see this
    // project's own DESIGN.md for the comparison against ReverbSc's
    // ~386KB.
    static constexpr uint16_t kPreDelayCapacity        = 8192;
    static constexpr uint16_t kInDiffusion0Capacity     = 256;
    static constexpr uint16_t kInDiffusion1Capacity     = 128;
    static constexpr uint16_t kInDiffusion2Capacity     = 512;
    static constexpr uint16_t kInDiffusion3Capacity     = 512;
    static constexpr uint16_t kDecayDiffusion1Capacity  = 1024; // both halves
    static constexpr uint16_t kPreDampingCapacity       = 8192; // both halves
    static constexpr uint16_t kDecayDiffusion2Capacity0 = 2048;
    static constexpr uint16_t kDecayDiffusion2Capacity1 = 4096;
    static constexpr uint16_t kPostDampingCapacity      = 4096; // both halves

    // 100ms @ 48kHz, same ceiling the original library itself uses
    // (MAX_PREDELAY).
    static constexpr uint16_t kMaxPreDelaySamples = 4800;

    // Also plain, uninitialized members (no `= {}`) -- see DelayBuffer's
    // own comment above for why. Init() memsets every storage array and
    // the setters it calls fill in every "amount"/state field.
    float pre_delay_storage_[kPreDelayCapacity];
    float in_diffusion0_storage_[kInDiffusion0Capacity];
    float in_diffusion1_storage_[kInDiffusion1Capacity];
    float in_diffusion2_storage_[kInDiffusion2Capacity];
    float in_diffusion3_storage_[kInDiffusion3Capacity];
    float decay_diffusion1_storage0_[kDecayDiffusion1Capacity];
    float decay_diffusion1_storage1_[kDecayDiffusion1Capacity];
    float pre_damping_storage0_[kPreDampingCapacity];
    float pre_damping_storage1_[kPreDampingCapacity];
    float decay_diffusion2_storage0_[kDecayDiffusion2Capacity0];
    float decay_diffusion2_storage1_[kDecayDiffusion2Capacity1];
    float post_damping_storage0_[kPostDampingCapacity];
    float post_damping_storage1_[kPostDampingCapacity];

    DelayBuffer pre_delay_;
    DelayBuffer in_diffusion_[4];
    DelayBuffer decay_diffusion1_[2];
    DelayBuffer pre_damping_[2];
    DelayBuffer decay_diffusion2_[2];
    DelayBuffer post_damping_[2];

    float pre_filter_state_;
    float damping_state_[2];

    float pre_filter_amount_;
    float input_diffusion1_amount_;
    float input_diffusion2_amount_;
    float decay_diffusion1_amount_;
    float damping_amount_; // internal convention -- see SetDamping01()
    float decay_amount_;
    float decay_diffusion2_amount_; // auto-derived, see SetDecay01()

    uint16_t t_;
};
