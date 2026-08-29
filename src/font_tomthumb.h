#pragma once

// Tiny proportional 3x5 font ("Tom Thumb"), modified for readability by
// Robey Pointer -- http://robey.lag.net/2010/01/23/tiny-monospace-font.html
// Ported from Adafruit-GFX's TomThumb.h (BSD-licensed; original font
// Copyright 1999 Brian J. Swetland, Copyright 1999 Vassilii Khachaturov).
//
// Kept separate from oled_fonts.c/.h because it doesn't fit that pipeline:
// libDaisy's FontDef/WriteChar() (see hid/disp/display.h) assumes a
// fixed-width font with one 16-bit row per glyph row, whereas Tom Thumb is
// proportional (per-glyph advance width) with its bits packed continuously
// across the whole glyph rather than row-aligned. So this draws directly
// via DrawPixel() instead of going through WriteChar()/FontDef at all.
//
// Only the base ASCII range (0x20 ' ' .. 0x7E '~') is included -- the
// upstream font also has an optional extended Latin block gated behind
// TOMTHUMB_USE_EXTENDED, which nothing here needs.

namespace daisy
{
class OneBitGraphicsDisplay;
}

// Row height (yAdvance) for this font -- callers doing their own row/gap
// layout math (see Ui::DrawControlRow()) need this.
constexpr int kTomThumbRowHeight = 6;

// Draws one character with its baseline at (x, baseline_y) -- not the top
// of the glyph, see each glyph's own negative y-offset. Returns the
// x-advance to move the pen for the next character. Characters outside
// the printable ASCII range draw as a space.
int TomThumbDrawChar(daisy::OneBitGraphicsDisplay* disp,
                      int                           x,
                      int                           baseline_y,
                      char                          ch,
                      bool                          on);

// Draws a whole string starting at (x, baseline_y). Returns the total
// width advanced (sum of each character's advance).
int TomThumbDrawText(daisy::OneBitGraphicsDisplay* disp,
                      int                           x,
                      int                           baseline_y,
                      const char*                   s,
                      bool                          on);

// Sum of each character's nominal advance width. Fine for left-aligning
// text or reserving horizontal space, but do NOT use this to align a
// string's trailing edge against something else (e.g. an icon sitting
// just past it) -- the last glyph's advance can include a little trailing
// bearing past its own ink. Use TomThumbInkWidth() for that.
int TomThumbAdvanceWidth(const char* s);

// Width to the last character's actual rightmost inked pixel, not its
// nominal advance. Use this when abutting text's right edge against
// something (e.g. positioning a value so it sits flush against an icon),
// so differently-shaped trailing characters (e.g. "35%" vs "72%") end up
// visually equidistant instead of off by the last glyph's own bearing.
int TomThumbInkWidth(const char* s);
