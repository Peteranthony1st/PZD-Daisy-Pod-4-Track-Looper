#include "font_tomthumb.h"
#include "hid/disp/display.h"
#include <cstdint>

using namespace daisy;

namespace
{
struct TomThumbGlyph
{
    uint16_t offset;
    uint8_t  w, h, adv;
    int8_t   xoff, yoff;
};

// clang-format off

// Raw glyph bits, MSB-first, packed continuously across each glyph (NOT
// row-aligned/byte-padded per row like oled_fonts.c's fonts) -- see
// TomThumbDrawChar()'s bit-reader below. Ported verbatim from Adafruit-GFX's
// TomThumbBitmaps[], base ASCII range only (0x20-0x7E).
const uint8_t kTomThumbBitmaps[] = {
    0x00, // 0x20 ' '
    0xE8, // 0x21 '!'
    0xB4, // 0x22 '"'
    0xBE, 0xFA, // 0x23 '#'
    0x79, 0xE4, // 0x24 '$'
    0x85, 0x42, // 0x25 '%'
    0xDB, 0xD6, // 0x26 '&'
    0xC0, // 0x27 '\''
    0x6A, 0x40, // 0x28 '('
    0x95, 0x80, // 0x29 ')'
    0xAA, 0x80, // 0x2A '*'
    0x5D, 0x00, // 0x2B '+'
    0x60, // 0x2C ','
    0xE0, // 0x2D '-'
    0x80, // 0x2E '.'
    0x25, 0x48, // 0x2F '/'
    0x76, 0xDC, // 0x30 '0'
    0x75, 0x40, // 0x31 '1'
    0xC5, 0x4E, // 0x32 '2'
    0xC5, 0x1C, // 0x33 '3'
    0xB7, 0x92, // 0x34 '4'
    0xF3, 0x1C, // 0x35 '5'
    0x73, 0xDE, // 0x36 '6'
    0xE5, 0x48, // 0x37 '7'
    0xF7, 0xDE, // 0x38 '8'
    0xF7, 0x9C, // 0x39 '9'
    0xA0, // 0x3A ':'
    0x46, // 0x3B ';'
    0x2A, 0x22, // 0x3C '<'
    0xE3, 0x80, // 0x3D '='
    0x88, 0xA8, // 0x3E '>'
    0xE5, 0x04, // 0x3F '?'
    0x57, 0xC6, // 0x40 '@'
    0x57, 0xDA, // 0x41 'A'
    0xD7, 0x5C, // 0x42 'B'
    0x72, 0x46, // 0x43 'C'
    0xD6, 0xDC, // 0x44 'D'
    0xF3, 0xCE, // 0x45 'E'
    0xF3, 0xC8, // 0x46 'F'
    0x73, 0xD6, // 0x47 'G'
    0xB7, 0xDA, // 0x48 'H'
    0xE9, 0x2E, // 0x49 'I'
    0x24, 0xD4, // 0x4A 'J'
    0xB7, 0x5A, // 0x4B 'K'
    0x92, 0x4E, // 0x4C 'L'
    0xBF, 0xDA, // 0x4D 'M'
    0xBF, 0xFA, // 0x4E 'N'
    0x56, 0xD4, // 0x4F 'O'
    0xD7, 0x48, // 0x50 'P'
    0x56, 0xF6, // 0x51 'Q'
    0xD7, 0xEA, // 0x52 'R'
    0x71, 0x1C, // 0x53 'S'
    0xE9, 0x24, // 0x54 'T'
    0xB6, 0xD6, // 0x55 'U'
    0xB6, 0xA4, // 0x56 'V'
    0xB7, 0xFA, // 0x57 'W'
    0xB5, 0x5A, // 0x58 'X'
    0xB5, 0x24, // 0x59 'Y'
    0xE5, 0x4E, // 0x5A 'Z'
    0xF2, 0x4E, // 0x5B '['
    0x88, 0x80, // 0x5C '\\'
    0xE4, 0x9E, // 0x5D ']'
    0x54, // 0x5E '^'
    0xE0, // 0x5F '_'
    0x90, // 0x60 '`'
    0xCE, 0xF0, // 0x61 'a'
    0x9A, 0xDC, // 0x62 'b'
    0x72, 0x30, // 0x63 'c'
    0x2E, 0xD6, // 0x64 'd'
    0x77, 0x30, // 0x65 'e'
    0x2B, 0xA4, // 0x66 'f'
    0x77, 0x94, // 0x67 'g'
    0x9A, 0xDA, // 0x68 'h'
    0xB8, // 0x69 'i'
    0x20, 0x9A, 0x80, // 0x6A 'j'
    0x97, 0x6A, // 0x6B 'k'
    0xC9, 0x2E, // 0x6C 'l'
    0xFF, 0xD0, // 0x6D 'm'
    0xD6, 0xD0, // 0x6E 'n'
    0x56, 0xA0, // 0x6F 'o'
    0xD6, 0xE8, // 0x70 'p'
    0x76, 0xB2, // 0x71 'q'
    0x72, 0x40, // 0x72 'r'
    0x79, 0xE0, // 0x73 's'
    0x5D, 0x26, // 0x74 't'
    0xB6, 0xB0, // 0x75 'u'
    0xB7, 0xA0, // 0x76 'v'
    0xBF, 0xF0, // 0x77 'w'
    0xA9, 0x50, // 0x78 'x'
    0xB5, 0x94, // 0x79 'y'
    0xEF, 0x70, // 0x7A 'z'
    0x6A, 0x26, // 0x7B '{'
    0xD8, // 0x7C '|'
    0xC8, 0xAC, // 0x7D '}'
    0x78, // 0x7E '~'
};

// {bitmapOffset, width, height, xAdvance, xOffset, yOffset} per glyph, one
// entry per character 0x20-0x7E in order. yOffset is relative to the
// baseline (negative = above it); xOffset is almost always 0 here.
const TomThumbGlyph kTomThumbGlyphs[] = {
    {0, 1, 1, 2, 0, -5}, // 0x20 ' '
    {1, 1, 5, 2, 0, -5}, // 0x21 '!'
    {2, 3, 2, 4, 0, -5}, // 0x22 '"'
    {3, 3, 5, 4, 0, -5}, // 0x23 '#'
    {5, 3, 5, 4, 0, -5}, // 0x24 '$'
    {7, 3, 5, 4, 0, -5}, // 0x25 '%'
    {9, 3, 5, 4, 0, -5}, // 0x26 '&'
    {11, 1, 2, 2, 0, -5}, // 0x27 '\''
    {12, 2, 5, 3, 0, -5}, // 0x28 '('
    {14, 2, 5, 3, 0, -5}, // 0x29 ')'
    {16, 3, 3, 4, 0, -5}, // 0x2A '*'
    {18, 3, 3, 4, 0, -4}, // 0x2B '+'
    {20, 2, 2, 3, 0, -2}, // 0x2C ','
    {21, 3, 1, 4, 0, -3}, // 0x2D '-'
    {22, 1, 1, 2, 0, -1}, // 0x2E '.'
    {23, 3, 5, 4, 0, -5}, // 0x2F '/'
    {25, 3, 5, 4, 0, -5}, // 0x30 '0'
    {27, 2, 5, 3, 0, -5}, // 0x31 '1'
    {29, 3, 5, 4, 0, -5}, // 0x32 '2'
    {31, 3, 5, 4, 0, -5}, // 0x33 '3'
    {33, 3, 5, 4, 0, -5}, // 0x34 '4'
    {35, 3, 5, 4, 0, -5}, // 0x35 '5'
    {37, 3, 5, 4, 0, -5}, // 0x36 '6'
    {39, 3, 5, 4, 0, -5}, // 0x37 '7'
    {41, 3, 5, 4, 0, -5}, // 0x38 '8'
    {43, 3, 5, 4, 0, -5}, // 0x39 '9'
    {45, 1, 3, 2, 0, -4}, // 0x3A ':'
    {46, 2, 4, 3, 0, -4}, // 0x3B ';'
    {47, 3, 5, 4, 0, -5}, // 0x3C '<'
    {49, 3, 3, 4, 0, -4}, // 0x3D '='
    {51, 3, 5, 4, 0, -5}, // 0x3E '>'
    {53, 3, 5, 4, 0, -5}, // 0x3F '?'
    {55, 3, 5, 4, 0, -5}, // 0x40 '@'
    {57, 3, 5, 4, 0, -5}, // 0x41 'A'
    {59, 3, 5, 4, 0, -5}, // 0x42 'B'
    {61, 3, 5, 4, 0, -5}, // 0x43 'C'
    {63, 3, 5, 4, 0, -5}, // 0x44 'D'
    {65, 3, 5, 4, 0, -5}, // 0x45 'E'
    {67, 3, 5, 4, 0, -5}, // 0x46 'F'
    {69, 3, 5, 4, 0, -5}, // 0x47 'G'
    {71, 3, 5, 4, 0, -5}, // 0x48 'H'
    {73, 3, 5, 4, 0, -5}, // 0x49 'I'
    {75, 3, 5, 4, 0, -5}, // 0x4A 'J'
    {77, 3, 5, 4, 0, -5}, // 0x4B 'K'
    {79, 3, 5, 4, 0, -5}, // 0x4C 'L'
    {81, 3, 5, 4, 0, -5}, // 0x4D 'M'
    {83, 3, 5, 4, 0, -5}, // 0x4E 'N'
    {85, 3, 5, 4, 0, -5}, // 0x4F 'O'
    {87, 3, 5, 4, 0, -5}, // 0x50 'P'
    {89, 3, 5, 4, 0, -5}, // 0x51 'Q'
    {91, 3, 5, 4, 0, -5}, // 0x52 'R'
    {93, 3, 5, 4, 0, -5}, // 0x53 'S'
    {95, 3, 5, 4, 0, -5}, // 0x54 'T'
    {97, 3, 5, 4, 0, -5}, // 0x55 'U'
    {99, 3, 5, 4, 0, -5}, // 0x56 'V'
    {101, 3, 5, 4, 0, -5}, // 0x57 'W'
    {103, 3, 5, 4, 0, -5}, // 0x58 'X'
    {105, 3, 5, 4, 0, -5}, // 0x59 'Y'
    {107, 3, 5, 4, 0, -5}, // 0x5A 'Z'
    {109, 3, 5, 4, 0, -5}, // 0x5B '['
    {111, 3, 3, 4, 0, -4}, // 0x5C '\\'
    {113, 3, 5, 4, 0, -5}, // 0x5D ']'
    {115, 3, 2, 4, 0, -5}, // 0x5E '^'
    {116, 3, 1, 4, 0, -1}, // 0x5F '_'
    {117, 2, 2, 3, 0, -5}, // 0x60 '`'
    {118, 3, 4, 4, 0, -4}, // 0x61 'a'
    {120, 3, 5, 4, 0, -5}, // 0x62 'b'
    {122, 3, 4, 4, 0, -4}, // 0x63 'c'
    {124, 3, 5, 4, 0, -5}, // 0x64 'd'
    {126, 3, 4, 4, 0, -4}, // 0x65 'e'
    {128, 3, 5, 4, 0, -5}, // 0x66 'f'
    {130, 3, 5, 4, 0, -4}, // 0x67 'g'
    {132, 3, 5, 4, 0, -5}, // 0x68 'h'
    {134, 1, 5, 2, 0, -5}, // 0x69 'i'
    {135, 3, 6, 4, 0, -5}, // 0x6A 'j'
    {138, 3, 5, 4, 0, -5}, // 0x6B 'k'
    {140, 3, 5, 4, 0, -5}, // 0x6C 'l'
    {142, 3, 4, 4, 0, -4}, // 0x6D 'm'
    {144, 3, 4, 4, 0, -4}, // 0x6E 'n'
    {146, 3, 4, 4, 0, -4}, // 0x6F 'o'
    {148, 3, 5, 4, 0, -4}, // 0x70 'p'
    {150, 3, 5, 4, 0, -4}, // 0x71 'q'
    {152, 3, 4, 4, 0, -4}, // 0x72 'r'
    {154, 3, 4, 4, 0, -4}, // 0x73 's'
    {156, 3, 5, 4, 0, -5}, // 0x74 't'
    {158, 3, 4, 4, 0, -4}, // 0x75 'u'
    {160, 3, 4, 4, 0, -4}, // 0x76 'v'
    {162, 3, 4, 4, 0, -4}, // 0x77 'w'
    {164, 3, 4, 4, 0, -4}, // 0x78 'x'
    {166, 3, 5, 4, 0, -4}, // 0x79 'y'
    {168, 3, 4, 4, 0, -4}, // 0x7A 'z'
    {170, 3, 5, 4, 0, -5}, // 0x7B '{'
    {172, 1, 5, 2, 0, -5}, // 0x7C '|'
    {173, 3, 5, 4, 0, -5}, // 0x7D '}'
    {175, 3, 2, 4, 0, -5}, // 0x7E '~'
};
// clang-format on

inline bool GetBit(const uint8_t* bitmap, int bitpos)
{
    return (bitmap[bitpos / 8] >> (7 - (bitpos % 8))) & 1;
}

inline const TomThumbGlyph& GlyphFor(char ch)
{
    if(ch < 0x20 || ch > 0x7E)
        ch = ' ';
    return kTomThumbGlyphs[ch - 0x20];
}
} // namespace

int TomThumbDrawChar(OneBitGraphicsDisplay* disp,
                      int                    x,
                      int                    baseline_y,
                      char                   ch,
                      bool                   on)
{
    const TomThumbGlyph& g      = GlyphFor(ch);
    int                  bitpos = g.offset * 8;
    for(int row = 0; row < g.h; row++)
    {
        for(int col = 0; col < g.w; col++)
        {
            if(GetBit(kTomThumbBitmaps, bitpos))
                disp->DrawPixel(x + g.xoff + col, baseline_y + g.yoff + row, on);
            bitpos++;
        }
    }
    return g.adv;
}

int TomThumbDrawText(
    OneBitGraphicsDisplay* disp, int x, int baseline_y, const char* s, bool on)
{
    int cx = x;
    for(; *s; s++)
        cx += TomThumbDrawChar(disp, cx, baseline_y, *s, on);
    return cx - x;
}

int TomThumbAdvanceWidth(const char* s)
{
    int w = 0;
    for(; *s; s++)
        w += GlyphFor(*s).adv;
    return w;
}

int TomThumbInkWidth(const char* s)
{
    if(!*s)
        return 0;
    int w = 0;
    for(; s[1]; s++)
        w += GlyphFor(*s).adv;
    const TomThumbGlyph& last = GlyphFor(*s);
    return w + last.xoff + last.w;
}
