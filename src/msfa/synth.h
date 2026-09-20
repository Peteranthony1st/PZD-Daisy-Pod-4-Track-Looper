// Vendored, unmodified-where-possible, from:
//   google/music-synthesizer-for-android (original "msfa" DX7 emulation
//   core, 2012) -> dcoredump/Synth_Dexed (Teensy-proven fork used by
//   MicroDexed/MiniDexed) -> this project (src/msfa/, copied verbatim
//   from a local Synth_Dexed checkout).
//
// Every file in src/msfa/ is the real msfa engine and carries its own
// Apache License 2.0 header below/inside it (see each file) -- kept
// deliberately separate from this project's own GPL-free codebase so it
// stays freely re-syncable from upstream. Deliberately NOT copied from
// Synth_Dexed's own src/ tree, because they are GPLv3 (voice-pool/
// note-management, SysEx bulk/single decode+encode, all MIDI controller
// plumbing) or Teensy/JUCE-specific and not needed here (this project
// has its own audio pipeline, MIDI dispatch, and shared reverb bus --
// see dexed_synth.h/.cpp, written original against this msfa core, not
// ported from any of the files below):
//   dexed.h/.cpp        -- GPLv3, the voice-pool + SysEx layer
//   PluginFx.h/.cpp      -- GPLv3, Dexed's own onboard effects (unused --
//                            this project routes into its own shared
//                            reverb bus instead, see main.cpp)
//   EngineMkI.h/.cpp,
//   EngineOpl.h/.cpp     -- GPLv3, alternate non-msfa emulation modes
//   synth_dexed.h/.cpp   -- GPLv3, a Teensy AudioStream wrapper
//   compressor.h         -- MIT, but tied to the onboard-effects path
//                            above and not needed without it
//
// Do not hand-edit files in this directory beyond what's strictly
// needed to compile under this project's ARM GCC toolchain (e.g. the
// TEENSYDUINO/__circle__ #ifdefs below already fall through cleanly to
// generic definitions for this target, so no edit was needed there) --
// keeping diffs from upstream near-zero is what makes re-syncing a
// future msfa bugfix/update a copy, not a re-port.

/*
   Copyright 2012 Google Inc.

   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at

        http://www.apache.org/licenses/LICENSE-2.0

   Unless required by applicable law or agreed to in writing, software
   distributed under the License is distributed on an "AS IS" BASIS,
   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
   See the License for the specific language governing permissions and
   limitations under the License.
*/

#ifndef SYNTH_H
#define SYNTH_H

//#define SUPER_PRECISE

#include <stdint.h>

// For TeensyDuino, put lookup tables
// in DMAMEM - they're initialised in code
// so this is safe, and saves RAM1 space
#if defined(TEENSYDUINO)
#include <WProgram.h> // for DMAMEM definition
#define TABLE_MEM DMAMEM
#else
#define TABLE_MEM
#endif // defined(TEENSYDUINO)

#define MIDI_CONTROLLER_MODE_MAX 2
#define TRANSPOSE_FIX 24
#define VOICE_SILENCE_LEVEL 1100

#define LG_N 6
#define _N_ (1 << LG_N)

/*template<typename T>
inline static T min(const T& a, const T& b) {
  return a < b ? a : b;
}

template<typename T>
inline static T max(const T& a, const T& b) {
  return a > b ? a : b;
}*/

#define QER(n,b) ( ((float)n)/(1<<b) )

#define FRAC_NUM float
#define SIN_FUNC sinf
// #define SIN_FUNC arm_sin_f32  // very fast but not as accurate
#define COS_FUNC cosf
// #define COS_FUNC arm_cos_f32  // very fast but not as accurate
#define LOG_FUNC logf
#define EXP_FUNC expf
#define SQRT_FUNC sqrtf
// #define ARM_SQRT_FUNC arm_sqrt_f32 // fast but not as accurate

#if defined(__circle__)

#include <circle/timer.h>

#ifndef M_PI
  #define M_PI 3.14159265358979323846
#endif

#define constrain(amt, low, high) ({ \
  __typeof__(amt) _amt = (amt); \
  __typeof__(low) _low = (low); \
  __typeof__(high) _high = (high); \
  (_amt < _low) ? _low : ((_amt > _high) ? _high : _amt); \
})

static inline int32_t signed_saturate_rshift(int32_t val, int32_t bits, int32_t rshift)
{
  int32_t out, max;

  out = val >> rshift;
  max = 1 << (bits - 1);
  if (out >= 0)
  {
    if (out > max - 1) out = max - 1;
  }
  else
  {
    if (out < -max) out = -max;
  }
  return out;
}

static inline uint32_t millis (void)
{
        return uint32_t(CTimer::Get ()->GetClockTicks () / (CLOCKHZ / 1000));
}

#endif
#endif
