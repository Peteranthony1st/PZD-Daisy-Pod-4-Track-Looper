#pragma once
#include <cstdint>

// Original, clean-room DX7 SysEx packed-voice <-> unpacked-patch
// conversion -- written directly from the public, decades-old DX7
// SysEx format specification (the Yamaha MIDI Data Format Sheet /
// Steve De Furia's Keyboard magazine article, widely mirrored as plain-
// text protocol documentation -- e.g. asb2m10/dexed's own
// Documentation/sysex-format.txt, itself just a factual byte/bit-layout
// description, not copyrighted source code). Never ported from or
// cross-checked against dexed.cpp's own (GPLv3, excluded) decoder --
// see msfa/synth.h's own provenance comment for why that file is
// excluded from this project entirely.
//
// "Packed" = the 128-byte-per-voice format used inside a 32-voice bulk
// dump SysEx message (6-byte header + 32*128 data bytes + checksum +
// 0xF7) -- this is the format real DX7 patch banks (including the
// classic ROM1A/1B/2A/2B carts) are actually distributed in.
// "Unpacked" = the 156-byte flat parameter layout Dx7Note::init()/
// update() consume directly (see dexed_synth.cpp's own patch_[156]).
// Both formats order operators OP6 first ... OP1 last (confirmed
// directly from the spec's own "Single Voice Dump" section), matching
// this project's existing op-index convention (op=0..5 in patch_[]
// already means OP6..OP1, not OP1..OP6) -- so unpacking is a plain
// per-operator-block expansion with no operator reordering needed.
namespace DexedSysex
{
// packed: exactly 128 bytes (one voice's worth, e.g. bank[voice_index*128])
// out_unpacked: exactly 156 bytes (bytes 0-154 filled; byte 155 is
// unused padding, matching the real spec's own note that the 156th
// "parameter" -- operator on/off -- is never part of a stored voice).
void UnpackVoice(const uint8_t packed[128], uint8_t out_unpacked[156]);

// Inverse of the above -- not currently called by anything (no SysEx
// export feature yet), included as the natural mirror of UnpackVoice()
// since it's the same spec either direction.
void PackVoice(const uint8_t unpacked[156], uint8_t out_packed[128]);
} // namespace DexedSysex
