# Ouroboros on Daisy Pod — design notes

## Credit

This project's name and original concept are based on
[kooliha's Ouroboros Loop Station](https://github.com/kooliha/Ouroboros_Loop_Station)
— a DIY 5-track stereo loop station for the Electrosmith Daisy Seed. This
firmware is a from-scratch rewrite adapted for the Daisy Pod's built-in
controls plus an added OLED menu, with a different internal architecture
(the DSP layer here has no direct hardware dependency) and an expanded
feature set (per-layer filters/effects/reverb, tempo-locked count-in, SD
card save/load), but the name, general concept, and file organization carry
over from that original project.

This is the promised firmware for porting Ouroboros to the Daisy Pod with an
I2C OLED added to its header, built around everything discussed: encoder+OLED
menu, the two knobs/buttons going "soft" (their meaning depends on the
screen), a BPM/bars/metronome/count-in engine with a shared loop length
across all 5 tracks, overdub on any layer, and a per-layer filter + one
selectable character effect + an optional loop-synced filter envelope.

Read the "Known limitations & assumptions" section before wiring anything —
a couple of things changed on purpose from the original pedal's behaviour.

## Hardware

- I2C OLED (SSD1306/SSD1309, 128x64 assumed) wired to the Pod's header:
  SCL -> D11, SDA -> D12, plus 3V3 and GND. These are the *default* pins
  libDaisy's I2C1 driver already uses, so no pin configuration is needed
  in code — `display.Init(disp_cfg)` just works. Default I2C address is
  0x3C; if your module is silent, it's very likely a 0x3D module — there's
  a commented-out line in `main.cpp` to switch it.
- Because the OLED is I2C, the header's SPI1 pins (D7/D8/D9/D10) are
  entirely free — more room than my first read of the Pod's spare pins
  suggested when I assumed you'd want SPI for the display. That's spare
  capacity if you want to keep an outboard input-conditioning board (see
  below) or add something else later (D14 and the two spare ADC pins
  D16/D22 are also still free).
- Everything else — both knobs, both buttons, the encoder+click, the two
  RGB LEDs — is the Pod's built-in hardware; nothing else needs wiring for
  the core looper.

### Input front-end (read this if you still want Mic/Guitar/Line switching)

The Daisy Pod's two audio inputs are plain 3.5mm **line-level** stereo
jacks — no mic preamp, no Hi-Z guitar buffering, no input relay, unlike your
original PCB. The firmware still has a 3-way input selector (Mic/Guitar/
Line) in the Global > Input menu, but on bare Pod hardware that's only
choosing *which physical jack* (left/right) feeds a recording, and true
stereo for "Line". Plugging a passive guitar straight into a Hi-Z-unaware
line input will sound weak and dull (impedance mismatch), and there's no
gain for a real microphone.

If you want the original's proper multi-input behaviour back, the practical
path is to keep your existing preamp/Hi-Z-buffer/relay board as an outboard
front-end feeding the Pod's line input, with the channel relay driven from
one of the Pod's free header GPIOs (e.g. D7, now that SPI isn't needed for
the OLED). That's not wired up in this firmware (I didn't want to guess at
hardware you haven't confirmed keeping) — it's a small addition to
`main.cpp` if you go that way: init a `GPIO` on D7 and write it based on
`ui.GetInputChannel()`.

## The menu system

One consistent grammar everywhere:

- **Rotate** the encoder: move the cursor / switch page (what it navigates
  depends on which screen you're on).
- **Click** the encoder: only meaningful on the Home screen, where it drills
  into the layer under the cursor.
- **Long-press** the encoder (~600ms): go back up a level. From Home (which
  has no parent) it instead opens Global Settings.

```
Home ──(click layer)──► Layer[n] ──(rotate)──► Status / Speed / Filter / Effect / Envelope
  │                                                     (long-press ⤴ back to Home)
  └──(long-press)──► Global ──(rotate)──► Tempo / Input
                                                (long-press ⤴ back to Home)
```

Both knobs and both physical buttons are "soft" — their function is shown on
the OLED's footer line and changes with whichever screen/page is open, per
your request. Full control map:

| Screen / Page      | Knob 1        | Knob 2          | Button 1 (short / long / release)          | Button 2 |
|---------------------|---------------|-----------------|---------------------------------------------|----------|
| Home                | Master volume | Metronome volume| Rec/Play/Overdub cycle for cursor layer      | Toggle bypass (dry monitor mix) |
| Layer / Status      | Volume        | Pan             | Same transport as Home, for this layer       | Hold 800ms = Clear |
| Layer / Speed       | Speed         | —               | Short = reset speed to 1.0x                  | — |
| Layer / Filter      | Cutoff        | Resonance       | Cycle filter mode (Off/Low/High/Band)        | — |
| Layer / Effect      | FX Param A    | FX Param B      | Cycle effect (Off/Drive/Bitcrush/Chorus/Tremolo) | — |
| Layer / Envelope    | Env Attack    | Env Depth       | Toggle envelope on/off                       | — |
| Global / Tempo      | BPM           | Bars per loop   | Toggle metronome on/off                      | — |
| Global / Input      | —             | —               | Cycle input channel (Mic/Guitar/Line)        | — |

BPM/Bars are greyed out ("locked") on screen once any layer holds a
recording, and the knobs stop affecting them, so you can't accidentally pull
every track out of sync — clear every layer to unlock and pick a new tempo.

### Per-layer transport state machine

```
Empty ──long-press──► ArmedCountIn ──(count-in finishes)──► Recording
                                                                 │ (auto-stops at
                                                                 │  the shared loop
                                                                 │  length, or short-
                                                                 │  press to stop early)
                                                                 ▼
                                                              Playing ◄──┐
                                                                 │ short  │ short
                                                                 ▼        │
                                                              Paused ─────┘
      Playing/Paused ──long-press (hold)──► Overdubbing ──release──► Playing
      Playing/Paused ──Button2 held 800ms (Status page only)──► Empty (cleared)
```

## Tempo, metronome, and count-in

One `TempoClock`, shared by all 5 layers, is the single source of timing
truth:

- **BPM** (40–240) and **Bars** (1–16) together define the loop length
  every layer records to: `samples = 60/BPM * 4 beats/bar * Bars * sample_rate`.
  4/4 time is assumed throughout (not configurable — it's one constant in
  `tempo_clock.h` if you ever want to change it).
- The metronome is a **single on/off switch** that governs both the audible
  click during playback *and* whether arming a recording does a count-in
  first, per how you described it. Metronome off = pressing record starts
  immediately (same feel as the original pedal). Metronome on = a 1-bar
  (4-beat) count-in always precedes recording, with an accented downbeat
  click.
- Once the first layer finishes recording, the tempo locks (see above) so
  every subsequent layer's recording is exactly the same length and they
  all loop in perfect sync — this is what "shared master length" means in
  practice.
- Changing BPM only affects *future* recordings once unlocked; it does not
  retroactively time-stretch anything already captured (no pitch/tempo
  shifting is implemented — that's a much bigger DSP undertaking than
  scope here).

## Overdubbing

Long-press-and-hold the transport button on any layer that's Playing or
Paused to overdub: new input is additively mixed into the existing loop
buffer, sample-for-sample, in sync with playback, then clamped to ±1.5 to
stop runaway buildup over many passes. Release to stop. This works
identically on every layer (not just a "drum" layer) per your answer.

## Per-layer signal chain

Applied on **playback only**, never baked into the recorded buffer — so
retakes, overdubs, and effect tweaking are always working with clean
material:

```
looped sample -> Filter (Svf: Low/High/Band, with optional loop-synced
                  envelope pushing the cutoff upward) -> one selectable
                  character effect -> Volume/Pan -> mixed to output bus
```

- **Filter**: cutoff (~20Hz–9kHz, log taper) and resonance are both live,
  turnable while the loop plays — this is the "cool cutoff/resonance while
  playing back" feature. Off/Low-pass/High-pass/Band-pass, cycled with
  Button 1.
- **Envelope**: an ADSR (DaisySP `Adsr`) retriggers every time the layer
  loops back to the start, sweeping the filter cutoff upward by an amount
  you dial in with "Depth", for an automatic filter-opening effect on every
  repeat. To keep the 2-knob UI simple, only Attack and Depth are
  knob-controlled; Decay/Sustain/Release sit at fixed defaults in
  `looper_layer.cpp`'s `Init()` (0.2s/0.3/0.3s) — easy constants to change
  in code if you want them exposed too, just not on the initial control
  surface built here (5 ADSR parameters don't fit meaningfully on 2 knobs
  at once).
- **Character effect** (one per layer, DaisySP-backed): Drive (Overdrive),
  Bitcrush (Decimator — Param A = bit depth, Param B = downsample),
  Chorus (Param A = depth, Param B = rate — chorus is fed the left channel
  only and generates its own stereo spread, a deliberate simplification),
  Tremolo (Param A = depth, Param B = rate).

## What I verified vs. what still needs a real board

I don't have an ARM cross-compiler or your libDaisy/STM32 build environment
in this sandbox, so I can't produce a flashable `.bin` here or prove this
builds with `make`. What I *did* do, against the actual current
`electro-smith/libDaisy` and `electro-smith/DaisySP` source (cloned fresh,
not from memory):

- Verified every class/method signature this firmware calls (`Encoder`,
  `Switch`, `DaisyPod`, `RgbLed`, `AnalogControl`, the SSD130x OLED driver
  stack, `Svf`, `Adsr`, `Overdrive`, `Decimator`, `Chorus`, `Tremolo`,
  `Metro`) against the real headers, not from training-data recall.
- Host-compiled and ran `tempo_clock.cpp` + `looper_layer.cpp` on this
  machine against the **real** DaisySP source (it has no hardware
  dependency, so this is a genuine build, not a simulation) — this caught
  and fixed a real off-by-one bug in the count-in state machine (the first
  count-in click was getting silently eaten by a state transition). Tests
  covered: tempo/loop-length math, arm → count-in → record → auto-stop,
  overdub start/stop, and a full loop pass with filter+envelope+chorus
  active checked for NaNs, plus every effect × every filter mode.
- Compiled `ui.cpp` and `main.cpp` against a hand-built mock of the Pod
  hardware API (matching the real headers' signatures exactly) and linked
  the whole thing together — this exercises every actual libDaisy call
  site in the UI/wiring code for type-correctness, though it can't catch
  STM32-HAL-specific issues since the mock doesn't model real hardware
  registers/timing.

What that leaves for you on real hardware: whether `make` succeeds against
your actual `libDaisy`/`DaisySP` submodules (should be a drop-in — the
Makefile only changed its source list), OLED orientation/contrast/I2C
address quirks specific to your exact module, and obviously all the "does
it feel good" tuning (filter range, envelope times, effect parameter
scaling) that only makes sense with the display in front of you and a loop
actually playing.

Note: the MAX7219 7-segment driver (`max7219.h`) and its SPI wiring are no
longer used at all — the OLED plus the two RGB LEDs replace it entirely, so
that board/chip doesn't need to come along to the Pod.

## Files

- `main.cpp` — hardware init, audio callback, main loop
- `looper_layer.h/.cpp` — per-layer state machine, filter/envelope/effects, audio
- `tempo_clock.h/.cpp` — BPM/bars/metronome/count-in engine
- `ui.h/.cpp` — encoder/button/knob handling + OLED menu rendering
- `Makefile` — unchanged shape, just an updated source list
