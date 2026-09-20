<img height="220" alt="IMG_20260829_153832" src="https://github.com/user-attachments/assets/2b5ef4e8-39c5-4f6e-b0f3-ebab1151912c" /><img height="220" alt="IMG_20260829_153742" src="https://github.com/user-attachments/assets/c5dccf4c-f985-4fb2-90eb-8b50f2cbe780" /><img height="220" alt="IMG_20260829_153938" src="https://github.com/user-attachments/assets/f0e90580-c2f8-4c6a-9800-b38b15a6b568" />

Video and looper generated WAV file below!

# PZD-4LOOP-DEXED-GRAINS

A 4-layer stereo loop station firmware for the [Electrosmith Daisy
Pod](https://daisy.audio/products/pod), with an I2C OLED
display added for a full on-device menu — live level/waveform display,
per-layer filter/effects/reverb, tempo-locked count-in, and SD card
save/load, all controlled from the Pod's two knobs, two buttons, and
encoder. Also runs **Dexed** (a real 6-operator DX7 clone) and
**Grains** (a granular synth) alongside the looper, both MIDI-played and
independently switchable.

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

- **MIDI IN** (optional) — the Pod's built-in MIDI jack, used to play the
  Dexed and Grains instruments described below. Standard 5-pin DIN or
  TRS MIDI IN, whatever your controller uses; no extra wiring needed.
- **Micro SD Card** formatted to FAT32.
  Inserted into the pod to save and load your files. This is a
  hand-wired socket with no card-detect pin, so swapping cards while the
  Pod is powered on isn't reliably picked up — power off before
  removing/reinserting a card, then power back on. See
  [`sdcard-template/`](sdcard-template/) for a ready-made folder set to
  copy onto a fresh card (most folders are created automatically as you
  use each feature, but the two Import folders aren't).

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
- Reverb send (each layer sends into one shared reverb — see Size below)
- Input gain (1x–4x) for quiet sources

**Global**
- Tempo (40–240 BPM), bars per loop (1–16), metronome on/off — locked
  once any layer holds a recording, so you can't pull layers out of sync
- Master-bus filter, applied to the full mix
- Reverb size/decay — one shared room for every layer's reverb send, plus
  Bypass's own independent send into that same room
- Bypass: hear your live input mixed into the output before you've even
  recorded anything
- Startup defaults — save your usual BPM/Bars/Volume/Metro/Filter/Reverb
  size/Bypass as what the pedal boots into, so powering on lands exactly
  where you like it instead of the factory defaults
- Vari-speed — a project-wide tape-style speed control (0.3x–2x, same
  feel as per-layer Speed) layered on top of everything already
  recorded, changing pitch and tempo together — the metronome and beat
  indicator track it live. Recording or overdubbing while it's off 1.0x
  captures correctly in sync with everything else, so it's genuinely
  usable as a performance/recording tool, not just a monitor toy
- Scrub/Freeze — Button 1 on Global:Speed cycles Normal → Scrub →
  Freeze → Normal. Scrub repurposes the encoder as a tape-scratch
  control over the shared playback position across every layer at once
  (normal playback keeps advancing underneath), with a composite
  waveform + moving playhead to scrub against — fast turns cover more
  ground than slow, deliberate ones. Freeze genuinely holds the
  transport still instead: every layer loops a small (~60ms) window
  around wherever playback currently sits as a smooth, click-free drone
  (Hann-windowed, the same click-avoidance trick Grains' own grains
  use), and the encoder scrubs which part of the loop that window plays
  from. Un-freezing resumes exactly where the drone left off

**Save/load**
- Save and load full performances (all 4 layers' audio plus every
  looper/tempo/global setting) to an SD card, up to 99 slots. Dexed and
  Grains each have their own fully independent preset
  systems (see above) — a performance deals only with the looper, and
  never embeds any instrument's sound, so switching/tweaking a synth's
  patch never disturbs a saved performance and vice versa.
- Saving always offers a choice between Overwrite (the currently loaded
  file) and Save New; loading shows the same style of choice between
  browsing existing files or starting fresh with New — identical
  convention across performances, Dexed presets, and Grains presets.
- Export the current performance as a standard stereo WAV file — one full
  loop, every layer's live filter/effect/reverb chain and the
  master filter applied, peak-normalized so it doesn't come out quiet.
  Two output options: full-quality native 48kHz for general use, or a
  44.1kHz-resampled copy saved straight into a `custom/` folder so the
  same SD card can go directly into a MicroDexed Touch's second card
  slot and be picked up as a sample with no copying needed. Whatever
  vari-speed is currently dialed in gets baked into the export on
  purpose, so you can capture a deliberate vari-speed effect to a file

**Dexed** — a MIDI-played, real 6-operator, 10-voice DX7 clone running
alongside the looper (`Global:Dexed`, opened by clicking the encoder
there), built around Google's own **msfa** DX7 emulation core — the real
32 algorithms, the real envelope/scaling model, not an approximation.
Off by default; switch it on from `Global:Dexed` (independent of
Grains — no mutex, both can run together).
- **3129 factory presets** across 54 folders — **Rom 1**-**Rom 4** (the
  real, unsorted contents of the actual Yamaha factory ROM cartridges),
  plus 50 more from one real ~100-bank collection that came already
  organized into named categories (Synth/Piano/EPiano/Bass/Strings/
  Woodwind/Brass/Organ/Perc/Voice/Bells/FX/Div), each split into as many
  same-named numbered folders (Synth 1, Synth 2, ...) as it takes to
  keep every one at or under 64 presets, so scrolling with Knob 1 always
  stays accurate — real, freely-distributed SysEx data from the broader
  Dexed/MicroDexed open-source ecosystem, never invented.
  Picking "Files" opens a group chooser first — **Roms** / **Dexed**
  (the two factory groups above) / **Imports** / **User** — since the
  flat folder list alone would only keep growing as you import more
  banks (see below). Button 1 is Back, Button 2 is Open at the group and
  folder-list levels; once inside an actual folder, a short Button 2 tap
  previews the highlighted patch live without leaving the browser, hold
  to commit.
- **Import your own SysEx patches** — drop a `.syx` file (a single voice
  or a full 32-voice bank dump) into `DXIMPORT/` on the SD card, then
  Preset → Files → Import → hold Button 2. Each import gets its own
  named folder (named after the file itself) under the Files → Imports
  group, saved as regular files on the card and read back on demand —
  never baked into the firmware, so you can import as many banks as the
  card has room for.
- **Algorithm page** shows a real box-and-arrow diagram of whichever of
  the 32 algorithms is loaded (carriers filled, modulators outlined, feedback
  tagged) — drawn fresh from the actual routing data, not 32 fixed
  pictures. Cycle it with Knob 1 or Buttons 1/2.
- **Brightness** and **Envelope Speed** macros scale every modulator's
  level, or every operator's envelope rate, together — centered at "as
  the preset saved it," so they're a quick sound-shaping layer on top of
  any patch rather than a replacement for editing it.
- Feedback, Vibrato (LFO speed/depth), its own filter, Pan, its own
  DX-FX (reverb/delay send) page, and output level round out the simple
  pages. Automatically compensates its own headroom when several notes
  play at once, so a hot, high-feedback patch doesn't distort just from
  being played as a chord.
- **Advanced editor** — the last simple page before Preset drills into a
  real per-operator editor (encoder click to enter, short click to
  return, long-press to Home like everywhere else): pick one of the 6
  operators (Button 1) and edit its Ratio/Level, Detune, and Attack/
  Decay/Sustain/Release for both its pitch-EG rate and level pairs.

Dexed sound demos:

Flexitone:
https://github.com/user-attachments/assets/fb43b8bd-0961-47e8-9b8a-c146e5467c7b

Marimba with delay:
https://github.com/user-attachments/assets/e26f79ab-3f50-418c-a878-0a9233a4e0d5

Clav DNS:
https://github.com/user-attachments/assets/ee4c3608-c0e0-40c6-aa30-00c4e3d72f9b

**Grains** — a MIDI-played, monophonic granular instrument
(`Global:Granular`), also running alongside the looper. Capture audio
either by recording live input directly (also picks up Dexed's own
sound if it's playing) or pulling in whatever's already
on one of the 4 loop layers, then play it back through two overlapping
grain layers: a fixed-position **Grain** cloud and a sweeping **Scan**
layer, each with its own independent Fill/Gap and Direction (forward/
reverse/random) — Grain also has its own Jitter (randomizes each
grain's read position slightly, smearing out an otherwise perfectly
periodic click some sources can produce), Scan has its own adjustable
Start/End range with Bounce (reverses at each edge) or Wrap (jumps
straight to the opposite edge). Grain size ranges 0-1000ms. Plus
Position, Tune (with optional note-tracking), its own ADSR/filter/
reverb+delay send (its own FX page)/pan, and a full-sample waveform
display with live grain markers, including the Scan range's own
boundary lines. A long Release now genuinely sustains a tail (new grains
keep triggering through the whole release, not just while a note is
held). The Grain layer can also be given a **rhythmic pattern** (Off, a
small named groove bank, or two Euclidean density stops) and its own
independent **Speed** (including syncing to the Looper's live tempo) —
Fill still controls how thick each hit sounds, Speed controls how often
they happen. Presets save the captured audio itself alongside every
parameter, so loading one restores the exact sound, not just settings.
Capture buffer holds up to ~10 seconds; importing a `.wav` from the SD
card's `IMPORT/` folder accepts 16/24/32-bit PCM or 32-bit float, mono
or stereo, and any of the common real-world WAV rates (8000-192000Hz) —
anything other than 48kHz is resampled automatically, with a real
anti-aliasing filter for genuine downsampling. Also switchable off from
`Global:Granular` to free its CPU share.

Grains demo:
https://github.com/user-attachments/assets/8aa7bd86-f749-4e34-a360-c5dd3af774d0

**Recording Dexed or Grains into a loop layer** — playing either
instrument while a layer is actively recording/overdubbing captures its
live sound directly into the take, mixed with your physical input, with
no patch cable needed.

**Mixer** — one screen (`Global:Mixer`, opened by clicking the encoder
there) with Volume/Pan/Reverb-Send for all 4 loop layers, Grains, Dexed,
and Bypass, plus Volume/Reverb-Size for the final Master bus, ending in
a live oscilloscope of the actual output mix. Rotate the encoder to
step through each channel in turn; `Global:Mixer` itself shows an
at-a-glance overview grid of all 8 channels' levels.

**SD card management** — `Global:SdMgmt` lets you browse either of 2
save categories (Performances/Grains Presets — Dexed Presets aren't
covered by this screen yet, a known gap; remove one by deleting its
`DEXP/PRESnnn.DAT` file directly on a computer instead) and Duplicate or
Delete individual files directly on the card, without needing a
computer. Deleting a file automatically closes the gap left behind —
every higher-numbered file in that category shifts down to keep the
numbering contiguous, so a later Save New always continues right after
the last real file instead of quietly reusing whatever number you just
freed.

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
- **Click** the encoder: from Home, opens the selected layer's pages; on
  `Global:Dexed`/`Global:Granular`/`Global:Mixer`, drills into
  that instrument/mixer's own screen; on any Dexed or
  Grains page, mutes/unmutes all 4 loop layers, same as every other
  Global page's click. On the Mixer screen itself, rotate steps through
  channels instead of pages (see below).
- **Long-press** the encoder: from Home, opens Global settings; from
  anywhere else (including Dexed, Grains, and Mixer), goes
  back to Home.

Both knobs and both buttons are "soft" — what they do depends on which
page is open, and it's always shown on screen: each footer row has a
label on either side of a small icon (a circle for the knobs, a square
for the buttons).

**Knob pickup** — knobs don't jump the moment you touch them. Each one
shows its live current value; turning the physical knob only takes
control once it reaches that value, so switching pages or layers never
suddenly yanks a setting to wherever the knob happens to be sitting.

**Basic workflow**, using pocket operators (applied effects before
making the video). Also includes a WAV file of acoustic guitar input,
MicroDexed Touch for drums, and testing using some headphones as a mic
input (put the gain up to x4):
https://github.com/user-attachments/assets/1c081390-e61f-4a1b-9088-a552860b9a5d
[Headphones_as_mic.WAV](https://github.com/user-attachments/files/31609801/Headphones_as_mic.WAV)

**4 layers with Dexed, Grains, PO-12 rhythm, and live mic input** — a
full performance combining everything above at once:
https://github.com/user-attachments/assets/025a71a1-f06f-424a-bb34-b3bdb560e121

**LEDs** — LED1 shows the currently-selected layer's state at a glance
(off = empty, amber = counting in, red = recording, orange = overdubbing,
green = playing, blue = paused). LED2 is a steady Bypass on/off indicator.

**Startup defaults** — dial in the Tempo/Volume/Metro/Filter/Reverb size/
Bypass you always want to start with, then hold Button 2 on Global:Tempo
for 800ms to remember it. From then on, powering the pedal on applies
that saved state automatically — nothing saved yet just boots with the
firmware's own defaults. This only covers global settings, never
per-layer ones, and it's separate from "New" on Global:File, which keeps
every current setting exactly as-is and only clears recorded audio.

**Vari-speed, Scrub, and Freeze** — Global:Speed's Knob 1 is a
tape-machine-style speed control for the whole project, not just one
layer: dead-zone centered on 1.0x, sweeping down to 0.3x or up to 2x
changes pitch and tempo together, and the metronome/beat indicator move
with it so everything stays honest about what's actually audible.
Button 1 cycles Normal → Scrub → Freeze → Normal:
- **Scrub** repurposes the encoder to scratch the shared playback
  position across every layer at once, against a composite waveform of
  everything currently recorded — normal playback keeps advancing
  underneath the whole time.
- **Freeze** genuinely stops the transport instead: every layer loops a
  small (~60ms) window around wherever playback currently sits as a
  smooth, click-free drone (Hann-windowed, the same trick Grains' own
  grains use to avoid clicks), and the encoder scrubs which part of the
  loop that window plays from. Un-freezing resumes exactly where the
  drone left off, no jump.

Navigate away or long-press to Home and the encoder goes right back to
normal (leaving while frozen un-freezes everything first). Recording or
overdubbing while vari-speed is off 1.0x captures correctly (returning
to 1.0x afterward won't throw anything out of sync), which also makes it
a genuine creative tool — record something while sped up, and it comes
back lower and slower once you return to normal speed, the classic tape
trick. Button 2 resets vari-speed straight back to 1.0x. Vari-speed is a
live-performance control like Master Volume — it always starts at 1.0x
on boot and after loading a save, and it's *not* excluded from WAV
export the way Master Volume is: whatever it's set to when you hit
Export gets baked into the file on purpose.

**Control reference**

| Page | Knob 1 | Knob 2 | Button 1 | Button 2 |
|---|---|---|---|---|
| Home | Master volume | Metronome volume | Rec / Pause / Overdub (tap/hold, state-dependent) | Toggle bypass |
| Layer: Status | Volume | Pan | Same transport as Home | Hold 800ms = Clear layer |
| Layer: Gain | Input gain | — | — | — |
| Layer: Speed | Speed | — | Tap = reset to 1.0x | — |
| Layer: Filter | Cutoff | Resonance | Cycle filter mode | — |
| Layer: Effect | Effect param A | Effect param B | Cycle effect | — |
| Layer: LYR-FX | Send | — | — | — |
| Global: Tempo | BPM | Bars | Toggle metronome | Hold 800ms = Save as startup default |
| Global: Speed | Vari-speed | — | Cycle Normal/Scrub/Freeze | Reset to 1.0x |
| Global: Filter | Cutoff | Resonance | Cycle filter mode | — |
| Global: GLB-FX | Size | Bypass reverb send | — | — |
| Global: File | Choosing Save: Overwrite/Save New; Browsing Load: scroll files | — | Save / Select file list (Load) / Back | Hold 800ms = Save or Load, whichever's open |
| Global: SD Mgmt | Scroll (folder or file list) | — | Tap = drill in/back, Hold 800ms = Duplicate | Hold 1500ms = Delete |
| Global: Export | — | — | Tap = Export, native 48kHz ("Studio") | Tap = Export, 44.1kHz for MicroDexed ("CD") |
| Global: Dexed | — | — | Toggle Dexed on/off (off by default) | — |
| Global: Granular | — | — | Toggle Grains on/off | — |
| Global: Looper | — | — | Toggle the whole 4-layer looper on/off | — |
| Global: Mixer | — | — | — | — (push encoder to enter the Mixer screen) |

**Dexed (`Screen::Dexed`, rotate to cycle pages)**

| Page | Knob 1 | Knob 2 | Button 1 | Button 2 |
|---|---|---|---|---|
| Algo | Cycle algorithm (all 32, quantized) | — | Cycle back | Cycle forward |
| Feedback | Feedback amount (0-7) | — | — | — |
| Vibrato | LFO speed | LFO pitch-mod depth | — | — |
| Brightness | Modulator level scale (center = as saved) | — | — | — |
| EnvSpeed | Envelope rate scale (center = as saved) | — | — | — |
| Filter | Cutoff | Resonance | Cycle filter mode | — |
| Mix | Reverb send | Output level | — | — |
| DX-FX | Reverb send / Delay send | Shared reverb size / delay time | Toggle Reverb/Delay | — |
| Advanced | — | — | — | — (push encoder to enter the operator editor) |
| Preset | Top: Files/Import/New. Files → Roms/Dexed/Imports/User group → folder → presets | — | Save / Select (top) / Back (group+folder) | Open (group+folder) / Hold: Import / Tap: preview, Hold: confirm |

The Algo page shows a real box-and-arrow diagram of whichever algorithm
is loaded (filled boxes = carriers, outlined = modulators, "FB" tags the
one operator with feedback), redrawn fresh from the actual routing data
for all 32 rather than 32 fixed pictures. Clicking the encoder on any
Dexed page other than Advanced mutes/unmutes all 4 loop layers, same as
every Global page. Pan isn't on this screen — it's on Dexed's own Mixer
channel instead (see Mixer below).

**Dexed advanced editor (`Screen::DexedOperator`, entered from the
Advanced page above, rotate to cycle pages)**

| Page | Knob 1 | Knob 2 | Button 1 | Button 2 |
|---|---|---|---|---|
| Ratio | Coarse ratio (0-31) | Output level | Cycle operator (1-6) | — |
| Detune | Detune (0-14, 7=centered) | — | Cycle operator (1-6) | — |
| EG Rate | Attack | Decay (or Sustain/Release) | Cycle operator (1-6) | Toggle AD/SR |
| EG Level | Attack Level | Decay Level (or Sustain/Release Level) | Cycle operator (1-6) | Toggle AD/SR |

A short encoder click here returns to Screen::Dexed's own Advanced
page; long-press still goes all the way to Home, same as everywhere
else. The real DX7 operator number (1-6) is always shown in the title.

**Grains (`Screen::Granular`, rotate to cycle pages)**

| Page | Knob 1 | Knob 2 | Button 1 | Button 2 |
|---|---|---|---|---|
| Grain | Size / Grain's own Gap | Fill / Jitter | Knobs → Size+Fill | Knobs → Gap+Jitter |
| Position | Position | Direction (Grain's own) | Cycle Rhythm pattern | Cycle Speed |
| Scan | Scan speed/direction | Scan's own Fill | Toggle Bounce/Wrap | Cycle Scan's own Direction |
| ScanRange | Scan Start / Scan's own Gap | Scan End | Knobs → Start+End | Knob → Gap |
| Tune | Tune | — | Toggle Map-to-Note | — |
| ADSR | Attack / Sustain | Decay / Release | Knobs → Attack+Decay | Knobs → Sustain+Release |
| Filter | Cutoff | Resonance | Cycle filter mode | — |
| Mix | Grain volume / Reverb send | Scan volume | Knobs → Grain+Scan | Knobs → Reverb send |
| GR-FX | Reverb send / Delay send | Shared reverb size / delay time | Toggle Reverb/Delay | — |
| Capture | — | — | Cycle source (Direct/Layer/Import) | Hold = record or capture |
| Trim | Trim start | Trim end | — | — |
| Preset | See Save/load above | — | Save / Select (Load) / Back | Hold 800ms = confirm |

Clicking the encoder on any Grains page also mutes/unmutes all 4 loop
layers.

**Mixer (`Screen::Mixer`, rotate to step through channels)**

Each of the 8 channels (Layer 1–4, Grains, Dexed ("DXD"), Bypass,
Master) gets its own stop; rotating past the last one shows a live
oscilloscope of the final output mix, then wraps back to Layer 1.

| Channel | Knob 1 | Knob 2 | Button 1 | Button 2 |
|---|---|---|---|---|
| Layer 1–4 / Grains / Dexed / Bypass | Volume | Pan | Knobs → Volume+Pan | Knobs → Reverb send |
| Master | Volume | Reverb size | — | — |

## Building and flashing

As of v1.6.0 this firmware boots via a small separate bootloader and runs
from the Pod's external QSPI flash chip instead of internal flash, which
was nearly full — see [DESIGN.md](DESIGN.md) for why. This changes how
flashing works, and means a **one-time setup step** on any Pod that
hasn't run this bootloader before.

**One-time setup** (skip this if your Pod already has the bootloader —
e.g. you've flashed v1.6.0 or later before):

1. Put the Pod into DFU mode (hold the `BOOT` button, tap `RESET`, release
   `BOOT`).
2. Flash the bootloader itself — either:
   - `dsy_bootloader_v6_4-intdfu-2000ms.bin`, attached to this project's
     [Releases](../../releases), via
     [Electrosmith's web programmer](https://flash.daisy.audio/) (no
     toolchain needed), or
   - from source: `cd src && make program-boot`

From then on, the Pod boots straight into whatever app is on QSPI —
normal power-on, no button-holding, just a slightly longer boot.

**Flashing the looper firmware itself** (every time, including that very
first time right after the bootloader install above):

- **Easiest — SD card**: copy `main.bin` (from
  [Releases](../../releases), or your own `src/build/main.bin`) onto the
  root of a FAT32 SD card, insert it into the Pod, and power-cycle. The
  bootloader notices any `.bin` file on the card and flashes it
  automatically — no DFU window to catch, works every time. If it's a
  fresh card, copy the contents of [`sdcard-template/`](sdcard-template/)
  onto the same root first (or alongside `main.bin`, order doesn't
  matter) — it sets up the folders Grains/Dexed importing needs
  (`IMPORT/`, `DXIMPORT/`), which aren't created automatically.
- **Alternative — DFU**: put the Pod into DFU mode the same way as above,
  then either `make program-dfu` from `src/`, or drop `main.bin` onto
  [Electrosmith's web programmer](https://flash.daisy.audio/). The
  bootloader's own DFU window is brief (~2 seconds) once it boots, so the
  SD card method above is generally more forgiving.

**Building from source**:

```
git clone --recurse-submodules <this repo's URL>
cd src && make
```

`build/main.bin` is what you flash via either method above.

## Thanks

- [**kooliha's Ouroboros Loop Station**](https://github.com/kooliha/Ouroboros_Loop_Station)
  — this project's name and general concept come from that original
  design, a DIY 5-track stereo loop station for the Daisy Seed. This
  firmware follows its per-layer loop-buffer struct as a direct
  structural template, then rewrites everything else — no direct
  hardware coupling in the DSP layer, plus filter/effects/reverb,
  the OLED menu, and SD save/load, none of which exist in the original.
  See [DESIGN.md](DESIGN.md) for the specifics.
- **[Robey Pointer](http://robey.lag.net/2010/01/23/tiny-monospace-font.html)**,
  for the tiny "Tom Thumb" font used throughout the OLED UI — a
  readability-tuned version of the original 3x5 font by Brian J.
  Swetland and Vassilii Khachaturov, ported here from Adafruit-GFX.
- **Google's [msfa](https://github.com/google/music-synthesizer-for-android)**
  (Apache License 2.0) — the real DX7 emulation core Dexed is built
  around, vendored unmodified into `src/msfa/`.
- The broader **Dexed/MicroDexed** open-source ecosystem — source of
  Dexed's 3129 factory presets, freely distributed real DX7 SysEx
  bank dumps.
