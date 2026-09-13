#pragma once
#include <cmath>
#include "itcm.h"

// A lookup-table-sine FM voice, `kNumOperators` = 4 operators, selectable
// between a few operator-routing "algorithms" -- built to test whether
// daisysp::Fm2's real cost (measured at 32% for 6 voices, isolated, vs
// Pad Synth's own 25% for the same count -- see main.cpp's own comment)
// was actually coming from FM synthesis itself, or just from Fm2's own
// Oscillator calling sinf() twice per sample per voice with no hardware
// transcendental unit to back it on this chip. A 2-operator version of
// this (kSineTableSize + linear interpolation, same idiom as Grains' own
// Hann window -- hann_table_/ReadHann() in granular_engine.h/.cpp)
// measured 10% for 6 voices -- confirming sinf() was the expensive part,
// not FM. 8-voice/3-operator was the balance chosen from that first round
// of measurement (more voices AND still cheaper than Pad Synth's own
// 6-voice engine); a 4th operator was added afterwards, once the shared-
// sine-table fix (see SineTable()'s own comment) removed the cache-
// thrashing spike that made an earlier 4-operator attempt look far more
// expensive than it actually was -- re-measured on real hardware before
// committing to it, same discipline as the original 3-op decision.
//
// Every algorithm below runs the exact same `kNumOperators` operators'
// worth of table-lookup-and-phase-math per sample -- Process() always
// does exactly 4 ReadTable() calls and 4 phase advances regardless of
// `algorithm_`, just wired differently (whose output feeds whose phase),
// so switching algorithms is a real, essentially-free sound-design
// choice, not a CPU trade-off.
class FastFmVoice
{
  public:
    static constexpr int kNumOperators = 4;

    // Op 0 is always (one of) the carrier's own operator slot(s);
    // index_[op] always means "depth at which the operator that
    // modulates op phase-modulates it" (see SetIndex()'s own comment) --
    // the same field is simply read differently depending on which
    // algorithm is active, same as real FM hardware reusing the same
    // per-operator knobs across algorithms.
    enum class Algorithm
    {
        // Op4 -> Op3 -> Op2 -> Op1/carrier, one deep chain -- the
        // deepest, most complex/evolving single-path timbre.
        Stack,
        // Op2, Op3 and Op4 all modulate Op1/carrier directly and
        // independently (not through each other) -- three separate
        // colors summed into one phase, the widest/densest single-
        // carrier tone available.
        Parallel,
        // Two independent 2-operator chains, both carriers, summed:
        // Op2->Op1 and Op4->Op3. More additive/detuned-pair character
        // than TwinCarrier's old 3-op shape, since BOTH carriers now get
        // their own modulator instead of one staying pure.
        DualStack,
        // Op3 and Op4 both modulate Op2 in parallel, and Op2's own
        // (now doubly-modulated) output modulates Op1/carrier -- a fork
        // feeding into a chain, richer than Stack's single modulator
        // per stage without the extra carrier DualStack/Parallel add.
        YBranch,
    };
    void      SetAlgorithm(Algorithm a) { algorithm_ = a; }
    Algorithm GetAlgorithm() const { return algorithm_; }

    void Init(float sample_rate)
    {
        sample_rate_ = sample_rate;
        SineTable(); // force the shared table built before first Process()
        for(int i = 0; i < kNumOperators; i++)
        {
            ratio_[i] = 1.f;
            index_[i] = 0.f;
            phase_[i] = 0.f;
            inc_[i]   = 0.f;
        }
        SetFrequency(220.f);
    }

    // Force-inlined (same reasoning as Process()'s own comment) -- called
    // from FmSynth::Process() up to once per voice PER SAMPLE (not just
    // per note-on) whenever the active preset's mod destination is
    // Vibrato, which is nearly every factory preset's default. That
    // makes this a genuine always-on per-sample cost, not just a
    // per-note-trigger one -- a real, measured contributor to Fm's CPU
    // reading staying high even before any note is held.
    __attribute__((always_inline)) inline void SetFrequency(float hz)
    {
        freq_ = hz;
        for(int i = 0; i < kNumOperators; i++)
            inc_[i] = (freq_ * ratio_[i]) / sample_rate_;
    }

    // op 0 is a carrier (the operator whose output reaches this voice's
    // final Process() return, directly or summed with another carrier);
    // op 1..kNumOperators-1 are modulators, wired per-algorithm (see the
    // class's own doc comment).
    void SetRatio(int op, float ratio)
    {
        ratio_[op] = ratio;
        inc_[op]   = (freq_ * ratio) / sample_rate_;
    }
    // Depth at which operator `op` is phase-modulated by whichever
    // operator feeds it in the active algorithm -- meaningless for an
    // operator nothing modulates (e.g. the topmost operator in a chain).
    // Same kIndexScalar convention as Fm2::kIdxScalar (index=5 -> a full
    // 2*PI radians of phase excursion).
    void SetIndex(int op, float index) { index_[op] = index * kIndexScalar; }

    // Force-inlined into FmSynth::Process() (fm_synth.cpp), which is
    // itself tagged DSY_ITCM_TEXT -- NOT given its own DSY_ITCM_TEXT tag,
    // because a header-inline function and FmSynth::Process()'s own
    // regular out-of-line definition get different COMDAT/linkage
    // treatment, and GCC refuses to put differently-linked functions in
    // the same explicit section ("section type conflict"). always_inline
    // sidesteps that entirely: there's no separate FastFmVoice::Process()
    // symbol at all once inlined, just more instructions inside
    // FmSynth::Process()'s own single ITCM-resident body.
    //
    // This function grew enough (4 operators, 4 algorithm branches) that
    // the compiler may no longer have been inlining all 8 per-sample
    // voice calls on its own, and any call left out-of-line falls back
    // to QSPIFLASH under this project's boot setup, which is
    // dramatically slower per-instruction than ITCM (see itcm.h's own
    // comment -- this is the exact regression class that header exists
    // to prevent). A real, measured symptom of exactly this: Fm's own
    // CPU reading stayed high (~50%+) even with no notes held, which
    // only makes sense if the cost is in how slowly the code executes,
    // not how much DSP work it's doing (every voice always runs its full
    // operator math regardless of note state).
    __attribute__((always_inline)) inline float Process()
    {
        float out = 0.f;
        switch(algorithm_)
        {
            case Algorithm::Stack:
            {
                // Op4 (raw) -> modulates Op3 -> modulates Op2 -> modulates
                // Op1/carrier -> final output.
                float mod4 = ReadTable(phase_[3]);
                float mod3 = ReadTable(WrapPhase(phase_[2] + mod4 * index_[2]));
                float mod2 = ReadTable(WrapPhase(phase_[1] + mod3 * index_[1]));
                out        = ReadTable(WrapPhase(phase_[0] + mod2 * index_[0]));
                break;
            }
            case Algorithm::Parallel:
            {
                // Op2, Op3, Op4 (all raw, not modulating each other) each
                // phase-modulate the carrier independently, summed.
                float mod2 = ReadTable(phase_[1]);
                float mod3 = ReadTable(phase_[2]);
                float mod4 = ReadTable(phase_[3]);
                out        = ReadTable(WrapPhase(phase_[0] + mod2 * index_[0] + mod3 * index_[1]
                                                  + mod4 * index_[2]));
                break;
            }
            case Algorithm::DualStack:
            {
                // Op2 -> Op1 (carrier A) and Op4 -> Op3 (carrier B),
                // fully independent, summed -- halved so two summed
                // carriers don't come out twice as loud as one.
                float mod2  = ReadTable(phase_[1]);
                float op1_o = ReadTable(WrapPhase(phase_[0] + mod2 * index_[0]));
                float mod4  = ReadTable(phase_[3]);
                float op3_o = ReadTable(WrapPhase(phase_[2] + mod4 * index_[2]));
                out         = (op1_o + op3_o) * 0.5f;
                break;
            }
            case Algorithm::YBranch:
            {
                // Op3 and Op4 (both raw) both phase-modulate Op2 in
                // parallel; Op2's own (doubly-modulated) output then
                // phase-modulates Op1/carrier.
                float mod3 = ReadTable(phase_[2]);
                float mod4 = ReadTable(phase_[3]);
                float mod2 = ReadTable(
                    WrapPhase(phase_[1] + mod3 * index_[1] + mod4 * index_[2]));
                out = ReadTable(WrapPhase(phase_[0] + mod2 * index_[0]));
                break;
            }
        }
        for(int i = 0; i < kNumOperators; i++)
        {
            phase_[i] += inc_[i];
            if(phase_[i] >= 1.f)
                phase_[i] -= 1.f;
        }
        return out;
    }

  private:
    static constexpr int   kSineTableSize = 256;
    static constexpr float kIndexScalar   = 0.2f; // matches Fm2's own kIdxScalar

    // Called from Process() up to 3x per voice per sample. Force-inlined
    // (not given its own DSY_ITCM_TEXT tag -- mixing a `static` and a
    // non-`static` member function under the SAME explicit linker
    // section trips a real GCC "section type conflict" error, since they
    // get different COMDAT/linkage treatment) so its body physically
    // becomes part of Process()'s own ITCM-resident code instead of a
    // separate out-of-line call the compiler might otherwise leave in
    // QSPIFLASH (see Process()'s own comment for why that matters).
    __attribute__((always_inline)) static inline float WrapPhase(float p)
    {
        while(p >= 1.f)
            p -= 1.f;
        while(p < 0.f)
            p += 1.f;
        return p;
    }

    // ONE table shared by every FastFmVoice instance (a function-local
    // static, lazily built on the very first call from any instance's
    // Init()) rather than each voice owning its own private 1KB copy --
    // 6+ separate per-instance tables scattered through memory meant the
    // FM loop had to reload data from a different location per voice
    // instead of reusing one small, cache-resident table, which was a
    // real, measured contributor to a slowdown when Pad Synth's own
    // Process() (touching plenty of its own SRAM state) ran in the same
    // audio block right before it. No thread-safety concern here --
    // single-core, and Init()/Process() only ever run from this same
    // audio context, never concurrently. Force-inlined into Process()
    // (see WrapPhase()'s own comment for why not its own DSY_ITCM_TEXT
    // tag) -- called 4x per voice per sample via ReadTable() below.
    __attribute__((always_inline)) static inline const float* SineTable()
    {
        static float table[kSineTableSize];
        static bool  built = false;
        if(!built)
        {
            for(int i = 0; i < kSineTableSize; i++)
                table[i] = sinf(2.f * 3.14159265358979323846f * (float)i / (float)kSineTableSize);
            built = true;
        }
        return table;
    }

    // Called from Process() 4x per voice per sample -- the single
    // hottest function in this class. Force-inlined into Process() (see
    // WrapPhase()'s own comment for why not its own DSY_ITCM_TEXT tag).
    __attribute__((always_inline)) inline float ReadTable(float phase01) const
    {
        const float* table = SineTable();
        float        idxf  = phase01 * (float)kSineTableSize;
        int          i0    = (int)idxf;
        float        frac  = idxf - (float)i0;
        int          i1    = (i0 + 1) % kSineTableSize;
        return table[i0] + (table[i1] - table[i0]) * frac;
    }

    float sample_rate_ = 48000.f;
    float freq_        = 220.f;
    float ratio_[kNumOperators];
    float index_[kNumOperators];
    float phase_[kNumOperators];
    float inc_[kNumOperators];
    Algorithm algorithm_ = Algorithm::Stack;
};
