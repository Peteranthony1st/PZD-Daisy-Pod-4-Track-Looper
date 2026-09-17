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

// 11 categories organized by real sound type (Synth/Piano/E.Piano/Bass/
// Strings/Woodwind/Brass/Organ/Percussion/Choir/Bells), not by source
// ROM bank -- real DX7 ROM banks mix categories internally, which would
// make for poor folder-browsing by sound type -- plus 4 more, "Rom 1"
// through "Rom 4", each the real, unsorted 64-voice contents of the
// actual Yamaha factory ROM cartridges (ROM1=1A+1B ... ROM4=4A+4B).
// Real quality/popularity rankings for DX7 patches aren't something
// that can be sourced reliably, so these stand in as an objective proxy
// instead: genuine factory data every real DX7 shipped with (ROM1) or
// that Yamaha sold as official cartridges (ROM2-4), kept in their own
// folders exactly as originally organized rather than re-sorted into
// the sound-type categories above.
constexpr int kDexedNumFactoryCategories = 15;
extern const DexedFactoryCategory kDexedFactoryCategories[kDexedNumFactoryCategories];
