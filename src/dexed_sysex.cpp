#include "dexed_sysex.h"
#include <cstring>

namespace DexedSysex
{
namespace
{
// Per-operator block sizes -- 17 bytes packed, 21 bytes unpacked (see
// dexed_sysex.h's own doc comment for the source of these offsets).
constexpr int kPackedOpSize   = 17;
constexpr int kUnpackedOpSize = 21;
} // namespace

void UnpackVoice(const uint8_t packed[128], uint8_t out_unpacked[156])
{
    std::memset(out_unpacked, 0, 156);

    for(int op = 0; op < 6; op++)
    {
        const uint8_t* p = packed + op * kPackedOpSize;
        uint8_t*       u = out_unpacked + op * kUnpackedOpSize;

        u[0] = p[0]; // EG rate 1
        u[1] = p[1]; // EG rate 2
        u[2] = p[2]; // EG rate 3
        u[3] = p[3]; // EG rate 4
        u[4] = p[4]; // EG level 1
        u[5] = p[5]; // EG level 2
        u[6] = p[6]; // EG level 3
        u[7] = p[7]; // EG level 4
        u[8] = p[8]; // keyboard level scaling breakpoint
        u[9] = p[9]; // scaling left depth
        u[10] = p[10]; // scaling right depth

        uint8_t b11 = p[11];
        u[11] = b11 & 0x03;        // scaling left curve (0-3)
        u[12] = (b11 >> 2) & 0x03; // scaling right curve (0-3)

        uint8_t b12 = p[12];
        u[13] = b12 & 0x07;        // oscillator rate scaling (0-7)
        u[20] = (b12 >> 3) & 0x0F; // oscillator detune (0-14)

        uint8_t b13 = p[13];
        u[14] = b13 & 0x03;        // amp mod sensitivity (0-3)
        u[15] = (b13 >> 2) & 0x07; // key velocity sensitivity (0-7)

        u[16] = p[14]; // operator output level

        uint8_t b15 = p[15];
        u[17] = b15 & 0x01;        // oscillator mode (0=ratio, 1=fixed)
        u[18] = (b15 >> 1) & 0x1F; // frequency coarse (0-31)

        u[19] = p[16]; // frequency fine
    }

    // Global parameters -- packed 102-127 -> unpacked 126-155.
    std::memcpy(out_unpacked + 126, packed + 102, 4); // pitch EG rates 1-4
    std::memcpy(out_unpacked + 130, packed + 106, 4); // pitch EG levels 1-4

    out_unpacked[134] = packed[110] & 0x1F; // algorithm (0-31)

    uint8_t b111       = packed[111];
    out_unpacked[135]  = b111 & 0x07;        // feedback (0-7)
    out_unpacked[136]  = (b111 >> 3) & 0x01; // oscillator key sync

    out_unpacked[137] = packed[112]; // LFO speed
    out_unpacked[138] = packed[113]; // LFO delay
    out_unpacked[139] = packed[114]; // LFO pitch mod depth
    out_unpacked[140] = packed[115]; // LFO amp mod depth

    uint8_t b116       = packed[116];
    out_unpacked[141]  = b116 & 0x01;        // LFO sync
    out_unpacked[142]  = (b116 >> 1) & 0x07; // LFO waveform (0-5)
    out_unpacked[143]  = (b116 >> 4) & 0x07; // pitch mod sensitivity (0-7)

    out_unpacked[144] = packed[117]; // transpose

    std::memcpy(out_unpacked + 145, packed + 118, 10); // voice name, 10 ASCII chars
    // out_unpacked[155] stays 0 -- unused padding, see this function's
    // own doc comment in dexed_sysex.h.
}

void PackVoice(const uint8_t unpacked[156], uint8_t out_packed[128])
{
    std::memset(out_packed, 0, 128);

    for(int op = 0; op < 6; op++)
    {
        const uint8_t* u = unpacked + op * kUnpackedOpSize;
        uint8_t*       p = out_packed + op * kPackedOpSize;

        p[0] = u[0];
        p[1] = u[1];
        p[2] = u[2];
        p[3] = u[3];
        p[4] = u[4];
        p[5] = u[5];
        p[6] = u[6];
        p[7] = u[7];
        p[8] = u[8];
        p[9] = u[9];
        p[10] = u[10];

        p[11] = (u[11] & 0x03) | ((u[12] & 0x03) << 2);
        p[12] = (u[20] & 0x0F) << 3 | (u[13] & 0x07);
        p[13] = (u[14] & 0x03) | ((u[15] & 0x07) << 2);
        p[14] = u[16];
        p[15] = (u[17] & 0x01) | ((u[18] & 0x1F) << 1);
        p[16] = u[19];
    }

    std::memcpy(out_packed + 102, unpacked + 126, 4);
    std::memcpy(out_packed + 106, unpacked + 130, 4);

    out_packed[110] = unpacked[134] & 0x1F;
    out_packed[111] = (unpacked[135] & 0x07) | ((unpacked[136] & 0x01) << 3);
    out_packed[112] = unpacked[137];
    out_packed[113] = unpacked[138];
    out_packed[114] = unpacked[139];
    out_packed[115] = unpacked[140];
    out_packed[116]
        = (unpacked[141] & 0x01) | ((unpacked[142] & 0x07) << 1) | ((unpacked[143] & 0x07) << 4);
    out_packed[117] = unpacked[144];

    std::memcpy(out_packed + 118, unpacked + 145, 10);
}
} // namespace DexedSysex
