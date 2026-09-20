#pragma once
#include <cstdint>

// See dexed_factory_data.cpp's own doc comment for the real source of
// this data (freely-distributed real DX7 SysEx banks from the broader
// Dexed/MicroDexed open-source ecosystem) and its format (raw
// 128-byte-per-voice packed payload, unpacked on demand via
// DexedSysex::UnpackVoice()).
struct DexedFactoryCategory
{
    const char*    name;
    const uint8_t* packed_data; // count * 128 bytes
    int            count;
};

// 4 categories, "Rom 1" through "Rom 4", each the real, unsorted
// 64-voice contents of an actual Yamaha factory ROM cartridge pair
// (ROM1=1A+1B ... ROM4=4A+4B) -- kept in their own folders exactly as
// originally organized, real quality/popularity rankings for DX7
// patches not being something that can be sourced reliably otherwise.
//
// Plus 50 more, all from one real ~100-bank, ~2900-voice patch
// collection (found already organized into 13 named categories --
// Synth/Piano/EPiano/Bass/Strings/Woodwind/Brass/Organ/Perc/Voice/
// Bells/FX/Div -- by its own upstream source manifest; an 8th category
// in that same source, its own "ROM" folder, was confirmed byte-
// identical to Rom 1-4 above and skipped). Each of those 13 is split
// into as many same-named "N" folders (Synth 1, Synth 2, ...) as it
// takes to keep every single one at or under 64 voices -- 2 source
// bank files (64 voices) per folder, 1 for any odd file left over --
// after real hardware testing showed a single knob's worth of physical
// rotation can't reliably land on one of e.g. 320 presets crammed into
// one folder. An earlier version of this project also carried 11
// hand-picked "Synth/Piano/E.Piano/..." categories of its own (2 bank
// files each) sourced independently of this collection; once this
// much larger, already-categorized collection was added, those were
// removed rather than kept alongside it as a smaller, redundant
// duplicate covering the same sound types.
constexpr int kDexedNumFactoryCategories = 54;
extern const DexedFactoryCategory kDexedFactoryCategories[kDexedNumFactoryCategories];
// The first 4 entries of kDexedFactoryCategories above are "Rom 1"
// through "Rom 4" specifically (see this file's own doc comment) --
// used by Ui's own Roms/Dexed group split (DexedFilesGroup, ui.h) to
// know where the real ROM categories end and the other ~50
// curated-by-sound-type ones begin.
constexpr int kDexedNumRomCategories = 4;
