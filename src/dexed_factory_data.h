#pragma once
#include <cstdint>

// See dexed_factory_data.cpp's own doc comment for the real source of
// this data (freely-distributed real DX7 SysEx banks from the official
// MicroDexed project) and its format (raw 128-byte-per-voice packed
// payload, unpacked on demand via DexedSysex::UnpackVoice()).
struct DexedFactoryCategory
{
    const char*    name;
    const uint8_t* packed_data; // count * 128 bytes
    int            count;
};

// 11 categories organized by real sound type (Synth/Piano/E.Piano/Bass/
// Strings/Woodwind/Brass/Organ/Percussion/Choir/Bells), not by source
// ROM bank -- real DX7 ROM banks mix categories internally, which would
// make for poor folder-browsing by sound type.
constexpr int kDexedNumFactoryCategories = 11;
extern const DexedFactoryCategory kDexedFactoryCategories[kDexedNumFactoryCategories];
