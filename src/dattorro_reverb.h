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
// The 13 real delay-line buffers (~166KB total -- see this class's own
// doc comment for the ReverbSc comparison), split out into their own
// plain POD struct so they can be placed in SDRAM independently of
// everything else in DattorroReverb below. Those arrays are too big
// for internal SRAM and have to live in external SDRAM; nothing else
// in this reverb does, and keeping tiny, constantly-touched bookkeeping
// fields (DelayBuffer's own mask_/read_offset_, the "amount" scalars)
// bundled into the same SDRAM-placed object as these forced them to
// pay SDRAM's own real per-access latency on every single sample for
// no reason -- see DattorroReverb's own class doc comment for the full
// story. Caller declares one of these DSY_SDRAM_BSS and passes it into
// Init(); DattorroReverb itself no longer needs any SDRAM placement at
// all.
struct DattorroReverbStorage
{
    float pre_delay_storage_[8192];
    float in_diffusion0_storage_[256];
    float in_diffusion1_storage_[128];
    float in_diffusion2_storage_[512];
    float in_diffusion3_storage_[512];
    float decay_diffusion1_storage0_[1024];
    float decay_diffusion1_storage1_[1024];
    float pre_damping_storage0_[8192];
    float pre_damping_storage1_[8192];
    float decay_diffusion2_storage0_[2048];
    float decay_diffusion2_storage1_[4096];
    float post_damping_storage0_[4096];
    float post_damping_storage1_[4096];
};

// Split from DattorroReverbStorage above (see its own doc comment for
// why): this class now holds only small, frequently-touched bookkeeping
// state (DelayBuffer structs, filter/damping state, the "amount"
// coefficients) -- nothing here is too big for ordinary SRAM, so unlike
// daisysp::ReverbSc or this class's own previous single-object shape,
// the caller declares this as a PLAIN object (no DSY_SDRAM_BSS), and
// separately declares one DattorroReverbStorage (which DOES still need
// DSY_SDRAM_BSS) to pass into Init().
class DattorroReverb
{
  public:
    // sample_rate is accepted for API-shape parity with
    // daisysp::ReverbSc::Init() but not otherwise used -- see this
    // class's own doc comment above. storage is the caller-owned,
    // separately-SDRAM-placed backing store for the 13 big delay
    // buffers -- see DattorroReverbStorage's own doc comment.
    void Init(float sample_rate, DattorroReverbStorage& storage);

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

    // The 13 big storage arrays themselves now live in the caller-owned
    // DattorroReverbStorage (see its own doc comment above) -- this
    // class keeps only the DelayBuffer structs below, whose own buf_
    // pointer gets wired to point into that external storage in
    // Init(). Also plain, uninitialized members (no `= {}`) -- see
    // DelayBuffer's own comment above for why; Init() memsets the
    // storage object's arrays and its own setters fill in every
    // "amount"/state field here.
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
