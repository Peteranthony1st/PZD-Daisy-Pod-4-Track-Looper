#include "fm_synth.h"
#include "itcm.h"

using namespace daisysp;

namespace
{
inline float Clampf(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

// Musically-useful FM ratios only -- see SetOp2Ratio01()'s own doc
// comment in fm_synth.h for why this is a lookup table, not a
// continuous sweep: any ratio not a clean multiple of the carrier
// produces inharmonic, clangorous overtones ("out of tune"), and a
// continuous knob almost never lands exactly on a clean value.
constexpr int   kNumRatios             = 10;
constexpr float kRatioTable[kNumRatios] = {0.5f, 1.f, 1.5f, 2.f, 3.f, 4.f, 5.f, 6.f, 7.f, 8.f};

// v01 bucket-center values that resolve to each kRatioTable entry via
// SetOp2Ratio01()/SetOp3Ratio01()'s own idx=(int)(v01*kNumRatios) lookup
// -- used below so each factory preset's ratio reads as "which multiplier"
// rather than an opaque 0..1 number. 0.5x=.05 1x=.15 1.5x=.25 2x=.35
// 3x=.45 4x=.55 5x=.65 6x=.75 7x=.85 8x=.95.
constexpr float kR0_5 = 0.05f, kR1 = 0.15f, kR1_5 = 0.25f, kR2 = 0.35f, kR3 = 0.45f, kR4 = 0.55f,
                kR5 = 0.65f, kR6 = 0.75f, kR7 = 0.85f, kR8 = 0.95f;

// Category name + how many of kFactoryPresetNames/kFactoryPresets'
// contiguous entries, starting right after the previous category's own
// range, belong to it -- see GetFactoryCategorySlot()'s own comment for
// how a (category, local index) pair resolves back to a flat slot number.
struct FactoryCategory
{
    const char* name;
    int         count;
};
constexpr FactoryCategory kFactoryCategories[FmSynth::kNumFactoryCategories] = {
    {"Basic", 2},
    {"E.Piano", 5},
    {"Bells", 5},
    {"Mallets", 4},
    {"Bass", 6},
    {"Brass", 4},
    {"Pad", 5},
    {"Lead", 5},
};

const char* kFactoryPresetNames[FmSynth::kNumFactoryPresets] = {
    // Basic
    "Init",
    "Neutral Pad",
    // E.Piano
    "Classic E.Piano",
    "Tine Bell EP",
    "Soft EP",
    "Bright EP",
    "FM Grand",
    // Bells
    "Digital Bells",
    "Church Bell",
    "Glass Bells",
    "Tubular Bell",
    "Music Box",
    // Mallets
    "Vibraphone",
    "Marimba",
    "Kalimba",
    "Steel Drum",
    // Bass
    "FM Bass",
    "Slap Bass",
    "Wobble Bass",
    "Sub Bass",
    "Growl Bass",
    "Pluck Bass",
    // Brass
    "Brass Section",
    "Poly Brass",
    "Synth Horn",
    "Muted Brass",
    // Pad
    "FM Strings",
    "Glass Pad",
    "Warm Pad",
    "Space Pad",
    "Choir Pad",
    // Lead
    "FM Lead",
    "Metallic Pluck",
    "Clav",
    "Sci-Fi Sweep",
    "Bright Lead",
};

// algorithm, op2_ratio01, op3_ratio01, op4_ratio01, op2_index01,
// op3_index01, op4_index01, attack01, decay01, sustain01, release01,
// chorus_depth01, chorus_rate01, filter_mode, filter_cutoff01,
// filter_res01, reverb_send01, output_level01, mod_destination,
// vibrato_depth01, vibrato_rate01. (tune01/pan01 both left at their
// 0.5f/0.5f struct defaults -- no preset here needs a transpose or an
// off-center pan.)
//
// Modeled on classic FM-synth patch archetypes (electric pianos, bells,
// basses, brass, pads, leads) -- first-pass values, meant to be ear-tuned
// on real hardware, not treated as final, same disclaimer as PadSynth's
// own kFactoryPresets. Each one is deliberately built to use all 4
// operators for real (not just carrying a neutral 4th operator along) --
// e.g. the electric piano/bass/pluck patches below use DualStack's two
// independent 2-op carriers (one warm/pure, one bright/percussive) for
// the classic "transient + body" FM shape real EPs use, rather than
// Parallel/Stack's single-carrier density.
using Alg = FastFmVoice::Algorithm;
const FmSynth::FmPresetData kFactoryPresets[FmSynth::kNumFactoryPresets] = {
    // --- Basic ---
    // Init -- neutral baseline, nothing added.
    {(int32_t)Alg::Stack, kR1, kR1, kR1, 0.3f, 0.2f, 0.15f, 0.1f, 0.3f, 0.8f, 0.3f, 0.f, 0.3f,
     (int32_t)FilterMode::Off, 1.f, 0.f, 0.2f, 0.7f, (int32_t)FmSynth::ModDestination::Vibrato,
     0.3f, 0.4f},
    // Neutral Pad -- same idea as Init but slower attack/release and
    // higher sustain, a plain sustained-pad starting point distinct from
    // Init's short/plucky neutral.
    {(int32_t)Alg::Stack, kR1, kR1, kR1, 0.25f, 0.15f, 0.1f, 0.5f, 0.4f, 0.85f, 0.6f, 0.2f, 0.3f,
     (int32_t)FilterMode::Off, 1.f, 0.f, 0.25f, 0.7f, (int32_t)FmSynth::ModDestination::Vibrato,
     0.3f, 0.4f},
    // --- E.Piano ---
    // Classic E.Piano -- DualStack's two independent carriers give the
    // real EP shape: Op2->Op1 stays warm/lightly modulated (the sustained
    // body), Op4->Op3 is bright and heavily modulated (the struck "bell"
    // transient) -- summed, exactly the "bell over body" attack real FM
    // pianos use, rather than faking it with one deep chain.
    {(int32_t)Alg::DualStack, kR1, kR1, kR8, 0.25f, 0.f, 0.6f, 0.05f, 0.4f, 0.4f, 0.3f, 0.15f, 0.3f,
     (int32_t)FilterMode::Off, 1.f, 0.f, 0.15f, 0.75f, (int32_t)FmSynth::ModDestination::Vibrato,
     0.15f, 0.4f},
    // Tine Bell EP -- Parallel with three simultaneous modulators (2x,
    // 4x, 7x) gives a denser, more metallic tine-piano color than a
    // single chain or pair could.
    {(int32_t)Alg::Parallel, kR2, kR4, kR7, 0.3f, 0.35f, 0.45f, 0.05f, 0.4f, 0.35f, 0.35f, 0.15f,
     0.3f, (int32_t)FilterMode::Off, 1.f, 0.f, 0.2f, 0.75f,
     (int32_t)FmSynth::ModDestination::Vibrato, 0.15f, 0.4f},
    // Soft EP -- same DualStack shape as Classic E.Piano but a milder
    // bright carrier (6x not 8x) and lower index, slower attack, longer
    // sustain -- a mellower Rhodes-like tone.
    {(int32_t)Alg::DualStack, kR1, kR1, kR6, 0.2f, 0.f, 0.4f, 0.08f, 0.5f, 0.55f, 0.4f, 0.25f, 0.3f,
     (int32_t)FilterMode::LowPass, 0.8f, 0.f, 0.2f, 0.7f, (int32_t)FmSynth::ModDestination::Vibrato,
     0.2f, 0.4f},
    // Bright EP -- same shape pushed further: 8x bright carrier, high
    // index, faster attack/decay -- a more aggressive, percussive
    // "digital piano" bite.
    {(int32_t)Alg::DualStack, kR1, kR1, kR8, 0.3f, 0.f, 0.8f, 0.03f, 0.3f, 0.3f, 0.25f, 0.1f, 0.3f,
     (int32_t)FilterMode::Off, 1.f, 0.f, 0.1f, 0.8f, (int32_t)FmSynth::ModDestination::Vibrato,
     0.1f, 0.4f},
    // FM Grand -- Stack's full 4-operator chain (1x -> 1.5x -> 3x) for a
    // denser, more acoustic-leaning piano tone, medium decay/sustain, a
    // touch of low-pass warmth.
    {(int32_t)Alg::Stack, kR1, kR1_5, kR3, 0.35f, 0.3f, 0.25f, 0.02f, 0.45f, 0.45f, 0.35f, 0.1f, 0.3f,
     (int32_t)FilterMode::LowPass, 0.85f, 0.f, 0.2f, 0.78f,
     (int32_t)FmSynth::ModDestination::Vibrato, 0.15f, 0.4f},
    // --- Bells ---
    // Digital Bells -- Parallel, three simultaneous inharmonic modulators
    // (4x/7x/8x), high index, long decay with almost no sustain (a struck
    // bell has no sustained body).
    {(int32_t)Alg::Parallel, kR4, kR7, kR8, 0.5f, 0.55f, 0.6f, 0.02f, 0.6f, 0.1f, 0.55f, 0.1f,
     0.25f, (int32_t)FilterMode::Off, 1.f, 0.f, 0.35f, 0.7f,
     (int32_t)FmSynth::ModDestination::Vibrato, 0.2f, 0.3f},
    // Church Bell -- Stack's full 4-operator chain, now rooted on a
    // genuinely INHARMONIC 1.5x (the nearest this project's ratio table
    // gets to the classic ~1.4:1 "bell" ratio real FM patch design uses --
    // a whole-number ratio like the old 1x here is actually harmonic, so
    // it read more like a buzzy organ than a real bell) -> 4x -> 7x,
    // very long decay/release, heavy reverb.
    {(int32_t)Alg::Stack, kR1_5, kR4, kR7, 0.5f, 0.55f, 0.6f, 0.02f, 0.75f, 0.05f, 0.8f, 0.1f, 0.2f,
     (int32_t)FilterMode::Off, 1.f, 0.f, 0.55f, 0.65f, (int32_t)FmSynth::ModDestination::Vibrato,
     0.1f, 0.3f},
    // Glass Bells -- Parallel, same inharmonic-root fix as Church Bell
    // (1.5x/6x/8x, not 3x) for a genuinely glassy/detuned shimmer rather
    // than a purely harmonic (if bright) buzz, high index, sparkly long
    // decay, low sustain, chorus for extra shimmer.
    {(int32_t)Alg::Parallel, kR1_5, kR6, kR8, 0.45f, 0.5f, 0.55f, 0.02f, 0.55f, 0.15f, 0.5f, 0.3f,
     0.3f, (int32_t)FilterMode::Off, 1.f, 0.f, 0.4f, 0.7f, (int32_t)FmSynth::ModDestination::Vibrato,
     0.2f, 0.3f},
    // Tubular Bell -- Stack's deepest chain, same inharmonic-root fix
    // (1.5x/5x/8x, not 2x) -- real tubular bells/chimes are famously
    // inharmonic, which a purely integer ratio chain can't reach, high
    // index, very long decay/release, heavy reverb.
    {(int32_t)Alg::Stack, kR1_5, kR5, kR8, 0.5f, 0.55f, 0.6f, 0.02f, 0.8f, 0.05f, 0.85f, 0.05f, 0.2f,
     (int32_t)FilterMode::Off, 1.f, 0.f, 0.6f, 0.65f, (int32_t)FmSynth::ModDestination::Vibrato,
     0.1f, 0.3f},
    // Music Box -- YBranch: Op3 and Op4 (1.5x, 7x) fork into Op2 (1x)
    // before it drives the carrier -- Op3's inharmonic 1.5x (not the old
    // harmonic 4x) gives the tiny metallic detuning real music-box tines
    // have, Op4's 7x keeps the sparkle, moderate index, quick decay, a
    // bright twinkly short tone.
    {(int32_t)Alg::YBranch, kR1, kR1_5, kR7, 0.4f, 0.35f, 0.4f, 0.01f, 0.35f, 0.1f, 0.3f, 0.1f, 0.4f,
     (int32_t)FilterMode::Off, 1.f, 0.f, 0.3f, 0.75f, (int32_t)FmSynth::ModDestination::Vibrato,
     0.1f, 0.4f},
    // --- Mallets ---
    // Vibraphone -- Parallel, 1x/4x modulate the carrier as before, Op4
    // left at a near-zero index (its 1x ratio makes it inaudible either
    // way) since the real instrument's simple two-partial shimmer doesn't
    // need a third modulator crowding it.
    {(int32_t)Alg::Parallel, kR1, kR4, kR1, 0.3f, 0.35f, 0.02f, 0.05f, 0.5f, 0.4f, 0.5f, 0.25f,
     0.35f, (int32_t)FilterMode::Off, 1.f, 0.f, 0.3f, 0.7f,
     (int32_t)FmSynth::ModDestination::Vibrato, 0.35f, 0.5f},
    // Marimba -- DualStack, pure Op1 fundamental thump plus a 3x-
    // modulated Op3/Op4 pair for the wood-like overtone, very fast decay
    // on both.
    {(int32_t)Alg::DualStack, kR1, kR1, kR3, 0.1f, 0.f, 0.55f, 0.02f, 0.2f, 0.15f, 0.15f, 0.f,
     0.3f, (int32_t)FilterMode::Off, 1.f, 0.f, 0.2f, 0.8f,
     (int32_t)FmSynth::ModDestination::Vibrato, 0.1f, 0.4f},
    // Kalimba -- DualStack, pure Op1 fundamental plus a 3x-modulated
    // Op3/Op4 pluck, fast decay, tight and woody.
    {(int32_t)Alg::DualStack, kR1, kR1, kR3, 0.1f, 0.f, 0.5f, 0.02f, 0.25f, 0.2f, 0.2f, 0.f, 0.3f,
     (int32_t)FilterMode::Off, 1.f, 0.f, 0.25f, 0.78f, (int32_t)FmSynth::ModDestination::Vibrato,
     0.1f, 0.4f},
    // Steel Drum -- Parallel, 2x/3x/5x with moderate-high index for the
    // metallic, slightly inharmonic steel-pan character, medium decay.
    {(int32_t)Alg::Parallel, kR2, kR3, kR5, 0.4f, 0.45f, 0.35f, 0.02f, 0.4f, 0.25f, 0.35f, 0.15f,
     0.3f, (int32_t)FilterMode::Off, 1.f, 0.f, 0.3f, 0.75f, (int32_t)FmSynth::ModDestination::Vibrato,
     0.15f, 0.4f},
    // --- Bass ---
    // FM Bass -- Stack, low ratios throughout (1x/1x/2x) keep the
    // harmonics tight and fundamental-heavy, quick punchy envelope,
    // filter closed down for a rounder low end.
    {(int32_t)Alg::Stack, kR1, kR1, kR2, 0.4f, 0.25f, 0.3f, 0.05f, 0.3f, 0.6f, 0.2f, 0.f, 0.3f,
     (int32_t)FilterMode::LowPass, 0.5f, 0.2f, 0.05f, 0.9f,
     (int32_t)FmSynth::ModDestination::FilterCutoff, 0.1f, 0.4f},
    // Slap Bass -- DualStack's pure Op1 (Op2 barely modulating it) plus a
    // 3x-modulated Op3/Op4 pair gives a percussive attack transient over
    // the pure low tone, fast decay, resonant filter for the "slap" bite.
    {(int32_t)Alg::DualStack, kR1, kR1, kR3, 0.1f, 0.f, 0.7f, 0.02f, 0.25f, 0.3f, 0.15f, 0.f, 0.3f,
     (int32_t)FilterMode::LowPass, 0.6f, 0.35f, 0.05f, 0.9f,
     (int32_t)FmSynth::ModDestination::FilterCutoff, 0.1f, 0.4f},
    // Wobble Bass -- Stack (2x -> 3x -> 1x), mod wheel assigned to the
    // filter cutoff for a classic wobble gesture, sustain high so a held
    // note keeps wobbling.
    {(int32_t)Alg::Stack, kR2, kR3, kR1, 0.5f, 0.4f, 0.2f, 0.02f, 0.3f, 0.8f, 0.2f, 0.f, 0.3f,
     (int32_t)FilterMode::LowPass, 0.55f, 0.4f, 0.05f, 0.85f,
     (int32_t)FmSynth::ModDestination::FilterCutoff, 0.55f, 0.55f},
    // Sub Bass -- Stack, all low ratios (1x/1x/1x), very low index for a
    // near-pure sine sub, filter closed further, output boosted to
    // compensate for how little harmonic energy is left.
    {(int32_t)Alg::Stack, kR1, kR1, kR1, 0.1f, 0.05f, 0.05f, 0.05f, 0.25f, 0.7f, 0.25f, 0.f, 0.3f,
     (int32_t)FilterMode::LowPass, 0.3f, 0.1f, 0.05f, 0.95f,
     (int32_t)FmSynth::ModDestination::Vibrato, 0.05f, 0.3f},
    // Growl Bass -- Parallel, 1x/2x/3x with higher index for a buzzier
    // growl, resonant low-pass, mod wheel assigned to filter cutoff for
    // extra growl movement.
    {(int32_t)Alg::Parallel, kR1, kR2, kR3, 0.55f, 0.5f, 0.45f, 0.03f, 0.3f, 0.65f, 0.2f, 0.f, 0.3f,
     (int32_t)FilterMode::LowPass, 0.5f, 0.45f, 0.05f, 0.88f,
     (int32_t)FmSynth::ModDestination::FilterCutoff, 0.5f, 0.5f},
    // Pluck Bass -- DualStack, pure Op1 low carrier plus a 2x-modulated
    // Op3/Op4 percussive transient, very fast decay, tight.
    {(int32_t)Alg::DualStack, kR1, kR1, kR2, 0.1f, 0.f, 0.6f, 0.01f, 0.2f, 0.2f, 0.15f, 0.f, 0.3f,
     (int32_t)FilterMode::LowPass, 0.6f, 0.2f, 0.05f, 0.88f,
     (int32_t)FmSynth::ModDestination::FilterCutoff, 0.1f, 0.4f},
    // --- Brass ---
    // Brass Section -- Parallel, three modulators (1x/2x/3x) for a
    // denser section-like buzz, a touch of attack lag for a section
    // swell, high sustain, resonant low-pass and chorus for ensemble width.
    {(int32_t)Alg::Parallel, kR1, kR2, kR3, 0.5f, 0.4f, 0.35f, 0.12f, 0.25f, 0.85f, 0.3f, 0.3f,
     0.35f, (int32_t)FilterMode::LowPass, 0.85f, 0.15f, 0.2f, 0.8f,
     (int32_t)FmSynth::ModDestination::FilterCutoff, 0.2f, 0.4f},
    // Poly Brass -- YBranch: Op3 and Op4 (2x, 3x) both modulate Op2 in
    // parallel before it drives the carrier, a genuinely brighter, more
    // synthetic edge than a single modulator chain, faster attack,
    // heavier chorus/filter movement.
    {(int32_t)Alg::YBranch, kR1, kR2, kR3, 0.55f, 0.3f, 0.3f, 0.06f, 0.2f, 0.9f, 0.25f, 0.4f, 0.4f,
     (int32_t)FilterMode::LowPass, 0.9f, 0.25f, 0.2f, 0.8f,
     (int32_t)FmSynth::ModDestination::FilterCutoff, 0.25f, 0.4f},
    // Synth Horn -- YBranch, 1x/2x fork into Op2 before the carrier,
    // moderate index, a medium attack for a horn-like swell, sustained,
    // resonant low-pass.
    {(int32_t)Alg::YBranch, kR1, kR1, kR2, 0.45f, 0.4f, 0.35f, 0.1f, 0.25f, 0.85f, 0.3f, 0.25f,
     0.35f, (int32_t)FilterMode::LowPass, 0.8f, 0.2f, 0.2f, 0.8f,
     (int32_t)FmSynth::ModDestination::FilterCutoff, 0.2f, 0.4f},
    // Muted Brass -- Stack, 1x/2x/3x with higher index for a buzzier
    // "muted" edge, quick attack, high-pass for a nasal, cutting character.
    {(int32_t)Alg::Stack, kR1, kR2, kR3, 0.5f, 0.45f, 0.4f, 0.05f, 0.2f, 0.75f, 0.25f, 0.2f, 0.4f,
     (int32_t)FilterMode::HighPass, 0.35f, 0.15f, 0.2f, 0.78f,
     (int32_t)FmSynth::ModDestination::FilterCutoff, 0.2f, 0.4f},
    // --- Pad ---
    // FM Strings -- Parallel, low index throughout (1x/2x/1x) for a
    // smooth/gentle tone, slow attack/release, generous chorus wash.
    {(int32_t)Alg::Parallel, kR1, kR2, kR1, 0.25f, 0.2f, 0.05f, 0.55f, 0.4f, 0.85f, 0.7f, 0.5f,
     0.3f, (int32_t)FilterMode::LowPass, 0.8f, 0.05f, 0.35f, 0.7f,
     (int32_t)FmSynth::ModDestination::ChorusDepth, 0.3f, 0.4f},
    // Glass Pad -- DualStack with a detuned first carrier (1.5x) plus a
    // 6x-modulated second carrier gives a genuinely richer shimmering
    // glassy wash than a single pure/modulated pair.
    {(int32_t)Alg::DualStack, kR1_5, kR6, kR6, 0.25f, 0.f, 0.4f, 0.6f, 0.5f, 0.8f, 0.75f, 0.45f,
     0.3f, (int32_t)FilterMode::Off, 1.f, 0.f, 0.4f, 0.7f,
     (int32_t)FmSynth::ModDestination::Vibrato, 0.25f, 0.35f},
    // Warm Pad -- Stack's full chain (1x -> 1x -> 1.5x), low index
    // throughout for a soft/rounded tone, slow attack/release, gentle
    // low-pass and chorus.
    {(int32_t)Alg::Stack, kR1, kR1, kR1_5, 0.3f, 0.15f, 0.2f, 0.65f, 0.5f, 0.85f, 0.7f, 0.3f,
     0.25f, (int32_t)FilterMode::LowPass, 0.65f, 0.1f, 0.35f, 0.7f,
     (int32_t)FmSynth::ModDestination::Vibrato, 0.3f, 0.4f},
    // Space Pad -- Parallel, three simultaneous high/inharmonic
    // modulators (5x/7x/8x) for a denser evolving ambient wash than two
    // could give, very slow attack/release, band-pass filter and heavy
    // chorus.
    {(int32_t)Alg::Parallel, kR5, kR7, kR8, 0.3f, 0.3f, 0.35f, 0.8f, 0.6f, 0.85f, 0.85f, 0.55f,
     0.2f, (int32_t)FilterMode::BandPass, 0.5f, 0.3f, 0.5f, 0.65f,
     (int32_t)FmSynth::ModDestination::Vibrato, 0.3f, 0.25f},
    // Choir Pad -- Parallel, low index (1x/1.5x/2x) for a smooth,
    // vocal-leaning tone, very slow attack/release, heavy chorus wash.
    {(int32_t)Alg::Parallel, kR1, kR1_5, kR2, 0.2f, 0.15f, 0.15f, 0.75f, 0.6f, 0.9f, 0.85f, 0.6f,
     0.25f, (int32_t)FilterMode::LowPass, 0.75f, 0.f, 0.4f, 0.7f,
     (int32_t)FmSynth::ModDestination::ChorusDepth, 0.3f, 0.4f},
    // --- Lead ---
    // FM Lead -- Parallel, three modulators (1x/2x/3x) for a richer/
    // denser lead than two could give, fast attack, high sustain for
    // melodic playing, resonant low-pass and vibrato depth for
    // expressiveness.
    {(int32_t)Alg::Parallel, kR1, kR2, kR3, 0.45f, 0.35f, 0.3f, 0.04f, 0.2f, 0.9f, 0.25f, 0.25f,
     0.4f, (int32_t)FilterMode::LowPass, 0.85f, 0.3f, 0.15f, 0.85f,
     (int32_t)FmSynth::ModDestination::Vibrato, 0.4f, 0.5f},
    // Metallic Pluck -- YBranch: Op3 and Op4 (1.5x, 7x) both modulate
    // Op2 in parallel -- Op3's inharmonic 1.5x (not the old harmonic 6x)
    // gives a genuinely clangorous, detuned edge real metallic-percussion
    // FM patches rely on, Op4's 7x keeps the bright top end, fast decay.
    {(int32_t)Alg::YBranch, kR1, kR1_5, kR7, 0.3f, 0.6f, 0.7f, 0.02f, 0.3f, 0.15f, 0.2f, 0.f, 0.3f,
     (int32_t)FilterMode::Off, 1.f, 0.f, 0.2f, 0.75f, (int32_t)FmSynth::ModDestination::Vibrato,
     0.1f, 0.4f},
    // Clav -- Stack's full chain (2x -> 5x -> 7x), high index throughout,
    // near-instant snap attack/decay, resonant high-pass for a buzzier,
    // more biting edge than a 3-operator clav could reach.
    {(int32_t)Alg::Stack, kR2, kR5, kR7, 0.55f, 0.45f, 0.4f, 0.01f, 0.15f, 0.1f, 0.1f, 0.f, 0.3f,
     (int32_t)FilterMode::HighPass, 0.4f, 0.3f, 0.1f, 0.85f,
     (int32_t)FmSynth::ModDestination::FilterCutoff, 0.1f, 0.4f},
    // Sci-Fi Sweep -- Stack's full chain (4x -> 6x -> 8x), high index,
    // slow attack and long release, band-pass filter assigned to mod
    // wheel for movement -- a genuinely denser sweep than 3 operators.
    {(int32_t)Alg::Stack, kR4, kR6, kR8, 0.7f, 0.6f, 0.65f, 0.7f, 0.55f, 0.6f, 0.8f, 0.35f, 0.25f,
     (int32_t)FilterMode::BandPass, 0.55f, 0.4f, 0.45f, 0.7f,
     (int32_t)FmSynth::ModDestination::FilterCutoff, 0.5f, 0.3f},
    // Bright Lead -- DualStack, warm Op1 carrier plus a 4x-modulated
    // Op3/Op4 bright carrier, fast attack, resonant low-pass, strong
    // vibrato for lead expressiveness.
    {(int32_t)Alg::DualStack, kR1, kR1, kR4, 0.2f, 0.f, 0.55f, 0.03f, 0.15f, 0.9f, 0.2f, 0.2f, 0.4f,
     (int32_t)FilterMode::LowPass, 0.9f, 0.35f, 0.15f, 0.85f,
     (int32_t)FmSynth::ModDestination::Vibrato, 0.45f, 0.5f},
};
} // namespace

const char* FmSynth::GetFactoryPresetName(int index)
{
    if(index < 0 || index >= kNumFactoryPresets)
        return "?";
    return kFactoryPresetNames[index];
}

FmSynth::FmPresetData FmSynth::GetFactoryPreset(int index)
{
    if(index < 0 || index >= kNumFactoryPresets)
        return FmPresetData{};
    return kFactoryPresets[index];
}

const char* FmSynth::GetFactoryCategoryName(int cat)
{
    if(cat < 0 || cat >= kNumFactoryCategories)
        return "?";
    return kFactoryCategories[cat].name;
}

int FmSynth::GetFactoryCategoryCount(int cat)
{
    if(cat < 0 || cat >= kNumFactoryCategories)
        return 0;
    return kFactoryCategories[cat].count;
}

int FmSynth::GetFactoryCategorySlot(int cat, int local_index)
{
    if(cat < 0 || cat >= kNumFactoryCategories)
        return -1;
    if(local_index < 0 || local_index >= kFactoryCategories[cat].count)
        return -1;
    int start = 0;
    for(int c = 0; c < cat; c++)
        start += kFactoryCategories[c].count;
    return start + local_index + 1; // slots are 1-based
}

void FmSynth::Init(float sample_rate)
{
    sample_rate_ = sample_rate;

    for(int i = 0; i < kMaxVoices; i++)
    {
        voices_[i].fm.Init(sample_rate);
        voices_[i].fm.SetRatio(0, 1.f); // carrier -- always 1:1 with the held note
        voices_[i].adsr.Init(sample_rate);
        voices_[i].held_note    = -1;
        voices_[i].base_hz      = 0.f;
        voices_[i].triggered_at = 0;
    }

    chorus_.Init(sample_rate);
    chorus_.SetDelayMs(15.f);
    chorus_.SetFeedback(0.15f);

    filter_l_.Init(sample_rate);
    filter_r_.Init(sample_rate);

    // Boot state is always exactly factory preset 0 ("Init"), same "no
    // second, separately-hand-coded default set to drift out of sync"
    // reasoning as PadSynth::Init()'s own GetFactoryPreset(0) call.
    ApplyPreset(GetFactoryPreset(0));
}

int FmSynth::FindVoiceForNote(uint8_t note)
{
    for(int i = 0; i < kMaxVoices; i++)
        if(voices_[i].held_note == (int)note)
            return i;

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

    int oldest = 0;
    for(int i = 1; i < kMaxVoices; i++)
        if(voices_[i].triggered_at < voices_[oldest].triggered_at)
            oldest = i;
    return oldest;
}

void FmSynth::ApplyFrequencyToVoice(Voice& v)
{
    v.fm.SetFrequency(v.base_hz * bend_ratio_ * tune_rate_);
}

void FmSynth::NoteOn(uint8_t note, uint8_t velocity)
{
    (void)velocity; // no velocity mapping yet -- SetOutputLevel01 covers overall level
    int    vi            = FindVoiceForNote(note);
    Voice& v              = voices_[vi];
    bool   retrigger_same = (v.held_note == (int)note);

    v.held_note    = note;
    v.base_hz      = 440.f * powf(2.f, ((float)note - 69.f) / 12.f);
    v.triggered_at = ++trigger_seq_;
    if(mod_dest_ != ModDestination::Vibrato)
        ApplyFrequencyToVoice(v);

    if(retrigger_same)
        v.adsr.Retrigger(false);
}

void FmSynth::NoteOff(uint8_t note)
{
    for(int i = 0; i < kMaxVoices; i++)
        if(voices_[i].held_note == (int)note)
            voices_[i].held_note = -1;
}

void FmSynth::SetTuneSemitones01(float v01)
{
    v01             = v01 < 0.f ? 0.f : (v01 > 1.f ? 1.f : v01);
    tune_semitones_ = (int)(v01 * 48.f + 0.5f) - 24; // -24..+24
    tune_rate_      = powf(2.f, (float)tune_semitones_ / 12.f);
}

float FmSynth::GetTuneSemitones01() const
{
    // Bucket center, not its exclusive upper edge -- see
    // GranularEngine::GetGrainTuneSemitones01()'s own comment for the
    // exact bug this avoids.
    return (float)(tune_semitones_ + 24) / 48.f;
}

void FmSynth::SetPan01(float v01)
{
    pan01_      = Clampf(v01, 0.f, 1.f);
    pan_l_gain_ = 1.f - pan01_;
    pan_r_gain_ = pan01_;
}

void FmSynth::SetPitchBendSemis(float semis)
{
    pitch_bend_semis_ = semis;
    bend_ratio_       = powf(2.f, semis / 12.f);
}

void FmSynth::SetModWheel01(float v01)
{
    mod_wheel01_ = Clampf(v01, 0.f, 1.f);
}

void FmSynth::ApplyOperatorsToAllVoices()
{
    for(int v = 0; v < kMaxVoices; v++)
    {
        voices_[v].fm.SetAlgorithm(algorithm_);
        voices_[v].fm.SetRatio(1, op2_ratio_);
        voices_[v].fm.SetRatio(2, op3_ratio_);
        voices_[v].fm.SetRatio(3, op4_ratio_);
        voices_[v].fm.SetIndex(0, op2_index01_ * kMaxIndex); // depth feeding the carrier stage
        voices_[v].fm.SetIndex(1, op3_index01_ * kMaxIndex); // depth feeding the Op2 stage
        voices_[v].fm.SetIndex(2, op4_index01_ * kMaxIndex); // depth feeding the Op3 stage
    }
}

void FmSynth::SetAlgorithm(int algo)
{
    if(algo < 0)
        algo = 0;
    if(algo > (int)FastFmVoice::Algorithm::YBranch)
        algo = (int)FastFmVoice::Algorithm::YBranch;
    algorithm_ = (FastFmVoice::Algorithm)algo;
    ApplyOperatorsToAllVoices();
}

void FmSynth::SetOp2Ratio01(float v01)
{
    int idx = (int)(Clampf(v01, 0.f, 1.f) * (float)kNumRatios);
    if(idx >= kNumRatios)
        idx = kNumRatios - 1;
    op2_ratio_idx_ = idx;
    op2_ratio_     = kRatioTable[idx];
    ApplyOperatorsToAllVoices();
}

float FmSynth::GetOp2Ratio01() const
{
    // Bucket center, not its exclusive upper edge -- see
    // GranularEngine::GetGrainTuneSemitones01()'s own comment for the
    // exact bug this avoids.
    return ((float)op2_ratio_idx_ + 0.5f) / (float)kNumRatios;
}

void FmSynth::SetOp3Ratio01(float v01)
{
    int idx = (int)(Clampf(v01, 0.f, 1.f) * (float)kNumRatios);
    if(idx >= kNumRatios)
        idx = kNumRatios - 1;
    op3_ratio_idx_ = idx;
    op3_ratio_     = kRatioTable[idx];
    ApplyOperatorsToAllVoices();
}

float FmSynth::GetOp3Ratio01() const
{
    return ((float)op3_ratio_idx_ + 0.5f) / (float)kNumRatios;
}

void FmSynth::SetOp4Ratio01(float v01)
{
    int idx = (int)(Clampf(v01, 0.f, 1.f) * (float)kNumRatios);
    if(idx >= kNumRatios)
        idx = kNumRatios - 1;
    op4_ratio_idx_ = idx;
    op4_ratio_     = kRatioTable[idx];
    ApplyOperatorsToAllVoices();
}

float FmSynth::GetOp4Ratio01() const
{
    return ((float)op4_ratio_idx_ + 0.5f) / (float)kNumRatios;
}

void FmSynth::SetOp2Index01(float v01)
{
    op2_index01_ = Clampf(v01, 0.f, 1.f);
    ApplyOperatorsToAllVoices();
}

void FmSynth::SetOp3Index01(float v01)
{
    op3_index01_ = Clampf(v01, 0.f, 1.f);
    ApplyOperatorsToAllVoices();
}

void FmSynth::SetOp4Index01(float v01)
{
    op4_index01_ = Clampf(v01, 0.f, 1.f);
    ApplyOperatorsToAllVoices();
}

void FmSynth::ApplyEnvelopeTimesToAllVoices()
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

void FmSynth::SetAttack01(float v01)
{
    attack01_ = Clampf(v01, 0.f, 1.f);
    ApplyEnvelopeTimesToAllVoices();
}
void FmSynth::SetDecay01(float v01)
{
    decay01_ = Clampf(v01, 0.f, 1.f);
    ApplyEnvelopeTimesToAllVoices();
}
void FmSynth::SetSustain01(float v01)
{
    sustain01_ = Clampf(v01, 0.f, 1.f);
    ApplyEnvelopeTimesToAllVoices();
}
void FmSynth::SetRelease01(float v01)
{
    release01_ = Clampf(v01, 0.f, 1.f);
    ApplyEnvelopeTimesToAllVoices();
}

float FmSynth::GetAttackSeconds() const
{
    return kMinAdsrSeconds * powf(kMaxAdsrSeconds / kMinAdsrSeconds, attack01_);
}
float FmSynth::GetDecaySeconds() const
{
    return kMinAdsrSeconds * powf(kMaxAdsrSeconds / kMinAdsrSeconds, decay01_);
}
float FmSynth::GetReleaseSeconds() const
{
    return kMinAdsrSeconds * powf(kMaxAdsrSeconds / kMinAdsrSeconds, release01_);
}

void FmSynth::SetChorusDepth01(float v01)
{
    chorus_depth01_ = Clampf(v01, 0.f, 1.f);
}
void FmSynth::SetChorusRate01(float v01)
{
    chorus_rate01_ = Clampf(v01, 0.f, 1.f);
}

void FmSynth::SetOutputLevel01(float v01)
{
    output_level01_ = Clampf(v01, 0.f, 1.f);
    output_level_   = powf(output_level01_, 2.5f) * 1.4f; // same curve as PadSynth/LooperLayer
    if(output_level_ < 0.f)
        output_level_ = 0.f;
}

bool FmSynth::IsVoiceActive(int i) const
{
    if(i < 0 || i >= kMaxVoices)
        return false;
    return voices_[i].held_note >= 0 || voices_[i].adsr.IsRunning();
}

void FmSynth::ApplyPreset(const FmPresetData& p)
{
    SetAlgorithm(p.algorithm);
    SetOp2Ratio01(p.op2_ratio01);
    SetOp3Ratio01(p.op3_ratio01);
    SetOp4Ratio01(p.op4_ratio01);
    SetOp2Index01(p.op2_index01);
    SetOp3Index01(p.op3_index01);
    SetOp4Index01(p.op4_index01);
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
    SetTuneSemitones01(p.tune01);
    SetPan01(p.pan01);
}

FmSynth::FmPresetData FmSynth::CapturePreset() const
{
    FmPresetData p;
    p.algorithm       = GetAlgorithm();
    p.op2_ratio01     = GetOp2Ratio01();
    p.op3_ratio01     = GetOp3Ratio01();
    p.op4_ratio01     = GetOp4Ratio01();
    p.op2_index01     = op2_index01_;
    p.op3_index01     = op3_index01_;
    p.op4_index01     = op4_index01_;
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
    p.tune01          = GetTuneSemitones01();
    p.pan01           = pan01_;
    return p;
}

DSY_ITCM_TEXT
void FmSynth::Process(size_t size,
                       float* out_l,
                       float* out_r,
                       float* reverb_send_l,
                       float* reverb_send_r)
{
    float filter_cutoff01_eff = filter_cutoff01_;
    float chorus_depth01_eff  = chorus_depth01_;
    if(mod_dest_ == ModDestination::FilterCutoff)
        filter_cutoff01_eff = Clampf(filter_cutoff01_ + mod_wheel01_ * 0.5f, 0.f, 1.f);
    else if(mod_dest_ == ModDestination::ChorusDepth)
        chorus_depth01_eff = Clampf(chorus_depth01_ + mod_wheel01_ * 0.5f, 0.f, 1.f);

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

    float vibrato_lfo_hz = kVibratoMinRateHz + vibrato_rate01_ * (kVibratoMaxRateHz - kVibratoMinRateHz);
    float vibrato_depth_fraction = vibrato_depth01_ * kVibratoMaxDepthFraction;

    // Non-vibrato voices only need their carrier frequency recomputed
    // once per block (pitch bend is control-rate); vibrato voices
    // recompute every sample below instead, since the LFO changes
    // continuously -- same structure as PadSynth::Process(), just via
    // ApplyFrequencyToVoice() (which recomputes all 3 operators'
    // increments, not just one oscillator's).
    if(mod_dest_ != ModDestination::Vibrato)
    {
        for(int v = 0; v < kMaxVoices; v++)
            if(voices_[v].held_note >= 0)
                ApplyFrequencyToVoice(voices_[v]);
    }

    for(size_t i = 0; i < size; i++)
    {
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
                voice.fm.SetFrequency(voice.base_hz * bend_ratio_ * tune_rate_
                                        * (1.f + lfo_val * mod_wheel01_ * vibrato_depth_fraction));
            }
            voice_sum += voice.fm.Process() * env;
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

        out_l[i] = fl * output_level_ * pan_l_gain_;
        out_r[i] = fr * output_level_ * pan_r_gain_;
        reverb_send_l[i] += out_l[i] * reverb_send01_;
        reverb_send_r[i] += out_r[i] * reverb_send01_;
    }
}
