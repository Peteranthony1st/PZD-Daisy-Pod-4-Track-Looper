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

#ifndef __FM_CORE_H
#define __FM_CORE_H

#include <stdint.h>
#include "aligned_buf.h"
#include "fm_op_kernel.h"
#include "synth.h"
#include "controllers.h"

class FmOperatorInfo {
  public:
    int32_t in;
    int32_t out;
};

enum FmOperatorFlags {
  OUT_BUS_ONE = 1 << 0,
  OUT_BUS_TWO = 1 << 1,
  OUT_BUS_ADD = 1 << 2,
  IN_BUS_ONE = 1 << 4,
  IN_BUS_TWO = 1 << 5,
  FB_IN = 1 << 6,
  FB_OUT = 1 << 7
};

class FmAlgorithm {
  public:
    int32_t ops[6];
};

// Per-operator routing info for a given algorithm, exposing exactly what
// render() itself decodes from an FmAlgorithm's flag byte (see
// EngineMsfa::render()) -- added so callers outside FmCore (Dexed's own
// Algo page diagram) can reconstruct an algorithm's real signal-flow
// graph without duplicating or peeking at the protected algorithms[]
// table.
struct FmOperatorRouting {
  int  input_bus;    // 0 (no FM input -- a "root" oscillator), 1, or 2
  int  output_bus;   // 0 (writes straight to the final mix -- a carrier), 1, or 2
  bool sums;         // true if this operator adds into its target (bus or
                      // final output) rather than overwriting it -- see
                      // render()'s own `add` flag. A bus can be written by
                      // more than one earlier-processed operator when this
                      // is true, which is how converging modulator chains
                      // (e.g. algorithms 16-18) are built.
  bool has_feedback; // true only for the one operator (if any) with real
                      // self-feedback -- both FB_IN and FB_OUT set, the
                      // same `(flags & 0xc0) == 0xc0` condition render()
                      // itself checks before routing through compute_fb().
};

class FmCore {
  public:
    FmCore() {};
    virtual ~FmCore() {};
    static void dump();
    static uint8_t get_carrier_operators(uint8_t algorithm);
    static FmOperatorRouting get_operator_routing(uint8_t algorithm, int op);
    virtual void render(int32_t *output, FmOpParams *params, int32_t algorithm, int32_t *fb_buf, int32_t feedback_gain) = 0;
  protected:
    AlignedBuf<int32_t, _N_>buf_[2];
    const static FmAlgorithm algorithms[32];
};

#endif
