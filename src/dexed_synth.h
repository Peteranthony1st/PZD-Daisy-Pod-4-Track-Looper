#pragma once
#include <cstdint>
#include <cstddef>
#include "msfa/dx7note.h"
#include "msfa/controllers.h"
#include "msfa/EngineMsfa.h"
#include "msfa/lfo.h"

// DX7/msfa port (see the approved plan). Phase 2 measured real per-voice
// CPU cost on hardware (1/2/8/10 simultaneous voices, ~7% fixed + ~2.3%/
// voice, no cache-thrashing surprise even at 10) before committing to a
// voice count -- 10 was chosen from that real measurement (~76%
// estimated combined worst case alongside the looper's own real worst
// case), matching this project's own established "measure before
// committing" convention. Phase 4 wires real MIDI dispatch/engine-select
// gating; Phase 5 adds UI. Still exactly one hardcoded test patch
// (Phase 6 adds real preset save/load, Phase 7 real SysEx import).
//
// Owns Dx7Note instances directly (msfa's real Apache-2.0 note engine,
// see msfa/dx7note.h) -- everything else in this class is original code
// written against that engine's public interface, not ported from the
// original Dexed plugin's own (GPLv3, excluded) voice-pool/SysEx layer.
// The oldest-note-steal allocation below mirrors the SHAPE of the
// removed FmSynth::FindVoiceForNote()'s own original logic (same
// project, no license concern -- not from any GPL'd source), adapted to
// Dx7Note's own envelope-position query (VoiceStatus::ampStep) in place
// of DaisySP's Adsr::GetCurrentSegment().
class DexedSynth
{
  public:
    void Init(float sample_rate);

    // Polyphonic voice allocation: retriggers the same voice if `note`
    // is already held there; otherwise claims a free voice (preferring
    // one that's fully released/silent over one still ringing out from
    // a recent NoteOff -- see VoiceIsIdle()); otherwise steals the
    // oldest-triggered voice outright. NoteOff() marks a voice free
    // immediately (so it's eligible for reuse right away) but leaves it
    // rendering its own real release tail -- see RenderQuantum(), which
    // always computes every voice unconditionally: msfa's own internal
    // per-operator gain threshold (see EngineMsfa::render()) already
    // collapses a fully decayed voice's real per-sample cost to nearly
    // nothing, so no extra bookkeeping is needed here to get that for
    // free.
    void NoteOn(uint8_t note, uint8_t velocity);
    void NoteOff(uint8_t note);

    // Raw 14-bit MIDI pitch bend (0..16383, 8192 = centered) -- this is
    // exactly the CC value Dx7Note::compute() itself expects (see
    // Controllers::values_[kControllerPitch] there), unlike a simplified
    // "bend amount in semitones" input (what the removed FastFmVoice
    // used): msfa's own pitch-bend math already combines this raw value
    // with a separate bend-range-in-semitones setting (hardcoded to +-2
    // in Init() for now -- a real Global/Dexed page control is future
    // incremental-editing scope, not this phase's), so re-deriving a
    // semitone offset here would just be undone downstream.
    void SetPitchBend14bit(uint16_t raw14) { ctrls_.values_[kControllerPitch] = raw14; }
    // 0..1, same convention as every other engine's own mod-wheel-style
    // input in this project.
    void SetModWheel01(float v01)
    {
        v01 = v01 < 0.f ? 0.f : (v01 > 1.f ? 1.f : v01);
        ctrls_.modwheel_cc = (uint8_t)(v01 * 127.f + 0.5f);
    }

    // Same WRITES-not-adds / reverb-send-ADDS convention as every other
    // engine's own Process() in this project (see the removed FmSynth::
    // Process()'s doc comment) -- main.cpp needs this exact out_l/out_r
    // signal for more than one consumer (dry mix + the master scope).
    //
    // Internally decouples this project's 48-sample audio block from
    // msfa's native 64-sample rendering quantum (_N_ in msfa/synth.h,
    // not a multiple of 48) via a small staging buffer -- see
    // dexed_synth.cpp's own comment.
    void Process(size_t size, float* out_l, float* out_r, float* reverb_send_l,
                 float* reverb_send_r);

    void  SetReverbSend01(float v01) { reverb_send01_ = v01; }
    float GetReverbSend01() const { return reverb_send01_; }
    void  SetOutputLevel01(float v01);
    float GetOutputLevel01() const { return output_level01_; }

    // The real, committed voice count -- chosen from Phase 2's own
    // hardware CPU measurement (see the class doc comment above), not a
    // guess or a value carried over from any other engine.
    static constexpr int kMaxVoices = 10;

  private:
    void RenderQuantum(); // fills stage_i32_ with one fresh 64-sample msfa quantum

    // Real oldest-note-steal allocation -- see NoteOn()'s own doc
    // comment for the 3-tier shape (retrigger / free / steal).
    int  FindVoiceForNote(uint8_t note);
    // True once every operator's envelope has fully reached its final
    // rest level (VoiceStatus::ampStep -- see msfa/env.cpp's own ix_
    // semantics: ix_ >= 4 means the envelope is no longer advancing at
    // all), i.e. genuinely safe to reassign with no audible cut -- vs.
    // merely "released" (NoteOff already called, but still ringing out).
    bool VoiceIsIdle(int voice_index);

    struct Voice
    {
        Dx7Note note;
        // -1 = free (eligible for reuse -- see NoteOff(), which frees a
        // voice immediately rather than waiting for its release tail to
        // finish). Otherwise the MIDI note currently assigned here.
        int      held_note   = -1;
        uint32_t triggered_at = 0;
    };
    Voice       voices_[kMaxVoices];
    // Monotonic, so "oldest" is always a plain min() over this -- same
    // idiom the removed FmSynth used for its own oldest-note-steal.
    uint32_t    trigger_seq_ = 0;
    Controllers ctrls_;
    EngineMsfa  engine_;
    // ONE shared LFO, not per-voice -- matches real DX7 hardware (the
    // LFO is global across every simultaneously-sounding voice, not an
    // independent oscillator per note).
    Lfo     lfo_;
    uint8_t patch_[156];

    float sample_rate_     = 48000.f;
    float reverb_send01_   = 0.f;
    // Same default every other engine in this project uses -- real
    // headroom safety against the additive test patch's occasional
    // constructive-interference peaks comes from Process()'s own soft
    // limiter now, not from permanently quieting this default (a
    // genuine reduction here just made real playing too quiet without
    // actually fixing the clipping, which was really a peak-headroom
    // problem, not an average-loudness one).
    float output_level01_  = 0.8f;
    float output_level_    = 1.f; // powf(output_level01_, 2.5f)*1.4f, cached by the setter

    // msfa's native quantum -- Dx7Note::compute()/EngineMsfa::render()
    // always produce exactly this many samples per call (hardcoded via
    // _N_ throughout fm_op_kernel.cpp/env.cpp, see msfa/synth.h).
    static constexpr int kMsfaBlock = 64;
    int32_t stage_i32_[kMsfaBlock];
    // Index of the next not-yet-drained sample in stage_i32_ -- starts
    // at kMsfaBlock so the very first Process() call renders a fresh
    // quantum immediately rather than reading uninitialized data.
    int stage_pos_ = kMsfaBlock;
};
