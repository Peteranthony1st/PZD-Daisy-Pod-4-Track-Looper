<img height="220" alt="IMG_20260829_153832" src="https://github.com/user-attachments/assets/2b5ef4e8-39c5-4f6e-b0f3-ebab1151912c" /><img height="220" alt="IMG_20260829_153742" src="https://github.com/user-attachments/assets/c5dccf4c-f985-4fb2-90eb-8b50f2cbe780" /><img height="220" alt="IMG_20260829_153938" src="https://github.com/user-attachments/assets/f0e90580-c2f8-4c6a-9800-b38b15a6b568" />

# PZD: Daisy Pod 4 Track Looper

A 4-layer stereo loop station firmware for the [Electrosmith Daisy
Pod](https://daisy.audio/products/pod), with an I2C OLED
display added for a full on-device menu — live level/waveform display,
per-layer filter/effects/reverb/pitch, tempo-locked count-in, and SD card
save/load, all controlled from the Pod's two knobs, two buttons, and
encoder.

The name, general concept, and file organization are based on
[kooliha's Ouroboros Loop Station](https://github.com/kooliha/Ouroboros_Loop_Station)
— see **Thanks** below.

## Hardware

- **[Daisy Pod](https://daisy.audio/products/pod)** — two
  knobs, two buttons, one clickable encoder, two RGB LEDs, stereo in/out,
  all built in.
- **I2C OLED display** (SSD1306/SSD1309, 128x64), wired to the pins
  broken out on the Pod's header:
  - `SCL` → `D11`
  - `SDA` → `D12`
  - plus `3V3` and `GND`

  These are libDaisy's default I2C1 pins, so no extra pin configuration
  is needed in code. Default I2C address is `0x3C` — if the display stays
  dark, it's likely a `0x3D` module; there's a commented-out line in
  `main.cpp` to switch it.

- **Micro SD Card** formatted to FAT32.
  Inserted into the pod to save and load your files.

- **PZD Daisy Pod Case**
  available at:
  [CULTS3D](https://cults3d.com/en/users/Planet_Zero_Designs_PZD/3d-models)
  [PRINTABLES](https://www.printables.com/@PlanetZeroDe_5266703)
  and
  [ETSY](https://www.etsy.com/uk/shop/PlanetZeroDesignsPZD?ref=shop_profile&listing_id=4560482983)

## Features

**Looping**
- 4 independent stereo layers sharing one master loop length — the first
  layer recorded sets the tempo-locked length every other layer records
  to, so everything stays in sync automatically.
- Record, play/pause, and overdub (additively layer new material onto an
  existing take) on any layer.
- Tempo-locked count-in with an audible metronome click, or record
  instantly with the metronome off.
- Live waveform display per layer with a moving playhead, auto-scaled so
  quiet input still shows clearly.

**Per-layer controls**
- Volume, Pan, Speed (0.3x–2x, tape-style — affects pitch too)
- Filter: Off / Low-pass / High-pass / Band-pass, with live cutoff and
  resonance
- One character effect at a time: Drive, Bitcrush, Chorus, Tremolo,
  Phaser, AutoWah, or Flanger
- Independent Pitch shift (±12 semitones) that can run *alongside* the
  character effect, with 3 selectable delay presets trading sync latency
  against pitch-shift smoothness (Fast/Med/Smooth)
- Reverb send and size
- Input gain (1x–4x) for quiet sources

**Global**
- Tempo (40–240 BPM), bars per loop (1–16), metronome on/off — locked
  once any layer holds a recording, so you can't pull layers out of sync
- Master-bus filter, applied to the full mix
- Bypass: hear your live input mixed into the output before you've even
  recorded anything

**Save/load**
- Save and load full performances (all 4 layers' audio plus every
  setting) to an SD card, up to 99 slots

## How to use it

**Recording your first loop**
1. On the Home screen, rotate the encoder to pick a layer (1–4).
2. Hold Button 1 to arm recording — if the metronome is on, it'll count
   in a bar first.
3. Play. Recording stops automatically once the bar loop length is
   reached (or tap Button 1 to stop early).
4. Tap Button 1 again to pause/resume playback; hold it again to overdub
   more onto the same layer.

**Navigating the menu**
- **Rotate** the encoder: on Home, moves between layers; on any other
  page, cycles through that page's sub-pages.
- **Click** the encoder: from Home, opens the selected layer's pages.
- **Long-press** the encoder: from Home, opens Global settings; from
  anywhere else, goes back to Home.

Both knobs and both buttons are "soft" — what they do depends on which
page is open, and it's always shown on screen: each footer row has a
label on either side of a small icon (a circle for the knobs, a square
for the buttons).

**Knob pickup** — knobs don't jump the moment you touch them. Each one
shows its live current value; turning the physical knob only takes
control once it reaches that value, so switching pages or layers never
suddenly yanks a setting to wherever the knob happens to be sitting.

**Control reference**

| Page | Knob 1 | Knob 2 | Button 1 | Button 2 |
|---|---|---|---|---|
| Home | Master volume | Metronome volume | Rec / Pause / Overdub (tap/hold, state-dependent) | Toggle bypass |
| Layer: Status | Volume | Pan | Same transport as Home | Hold 800ms = Clear layer |
| Layer: Speed | Speed | — | Tap = reset to 1.0x | — |
| Layer: Filter | Cutoff | Resonance | Cycle filter mode | — |
| Layer: Effect | Effect param A | Effect param B | Cycle effect | — |
| Layer: Pitch | Amount | Fun (modulation) | Toggle Pitch on/off | Cycle delay preset |
| Layer: Reverb | Send | Size | — | — |
| Layer: Gain | Input gain | — | — | — |
| Global: Tempo | BPM | Bars | Toggle metronome | — |
| Global: Filter | Cutoff | Resonance | Cycle filter mode | — |
| Global: File | Browse save slots | — | Tap = Save, Hold 400ms = New | Hold 800ms = Load |

## Building and flashing

```
git clone --recurse-submodules <this repo's URL>
cd src && make
```

Then flash `build/main.bin` to a Pod in DFU/bootloader mode, either with
`make program-dfu` or via Electrosmith's browser-based
[Daisy Programmer](https://flash.daisy.audio/) — no toolchain needed for
that route, just the `.bin` file.

Prebuilt binaries are also available under
[Releases](releases) for anyone who just wants to flash it without
building from source.

## Thanks

- [**kooliha's Ouroboros Loop Station**](https://github.com/kooliha/Ouroboros_Loop_Station)
  — this project's name, general concept, and file organization are
  based on that original design, a DIY 5-track stereo loop station for
  the Daisy Seed. This firmware is a from-scratch rewrite for the Pod
  with an expanded feature set, but the lineage carries over.
- **[Robey Pointer](http://robey.lag.net/2010/01/23/tiny-monospace-font.html)**,
  for the tiny "Tom Thumb" font used throughout the OLED UI — a
  readability-tuned version of the original 3x5 font by Brian J.
  Swetland and Vassilii Khachaturov, ported here from Adafruit-GFX.
