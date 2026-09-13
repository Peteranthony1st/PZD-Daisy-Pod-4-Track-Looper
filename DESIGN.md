# Ouroboros on Daisy Pod — design notes

## Credit

This project's name and original concept are based on
[kooliha's Ouroboros Loop Station](https://github.com/kooliha/Ouroboros_Loop_Station)
— a DIY 5-track stereo loop station for the Electrosmith Daisy Seed. This
firmware follows that project's per-layer loop-buffer struct as a direct
structural template — the same field set (`buffer_l`/`buffer_r`/
`record_len`/`write_idx`/`play_pos`/`speed`/`volume`/`pan`) and the same
linear-interpolated playback loop — but removes its direct hardware
coupling (this DSP layer takes no `Switch*`/ADC pointers at all) and adds
everything else from scratch: per-layer filter/effect/reverb,
tempo-locked count-in, a live waveform display, the whole OLED menu/UI
layer, and SD card save/load, none of which exist in the original.

See **Known limitations & assumptions** below for the handful of things
that deliberately differ from the original pedal's behaviour.

## Hardware

- I2C OLED (SSD1306/SSD1309, 128x64 assumed) wired to the Pod's header:
  SCL -> D11, SDA -> D12, plus 3V3 and GND. These are the *default* pins
  libDaisy's I2C1 driver already uses, so no pin configuration is needed
  in code — `display.Init(disp_cfg)` just works. Default I2C address is
  0x3C; if your module is silent, it's very likely a 0x3D module — there's
  a commented-out line in `main.cpp` to switch it.
- Because the OLED is I2C, the header's SPI1 pins (D7/D8/D9/D10) are
  entirely free, along with D14 and the two spare ADC pins D16/D22, if you
  ever want to add something else.
- Everything else — both knobs, both buttons, the encoder+click, the two
  RGB LEDs — is the Pod's built-in hardware; nothing else needs wiring
  for the core looper.

### Input front-end

The Daisy Pod's two audio inputs are plain 3.5mm **line-level** stereo
jacks — no mic preamp, no Hi-Z guitar buffering, no input relay, unlike
the original pedal's PCB. There is deliberately **no** Mic/Guitar/Line
input-routing menu here (an earlier iteration had one, but on bare Pod
hardware it never did anything beyond choosing which physical jack fed
the recording — no actual gain difference — so it wasn't earning its
menu slot). Both jacks are always captured in true stereo instead (see
`LooperLayer::Process()`'s recording path).

The actual fix for a quiet source (a passive guitar, a mic, anything
without its own preamp) is the per-layer **Gain** page — 1x-4x (0 to
+12dB), applied at record/overdub time only, live on the OLED so you can
see the level before committing a take. If you want the original
pedal's proper multi-input hardware behaviour back (real mic preamp,
Hi-Z buffering, an input relay), that's an outboard front-end feeding the
Pod's line input — not wired up in this firmware since it depends on
hardware nobody's confirmed keeping.

## The menu system

One consistent grammar everywhere:

- **Rotate** the encoder: on Home, moves the layer cursor; on any other
  screen, cycles through that screen's pages — with one exception,
  `Screen::Mixer`, where rotate instead steps through channel "stops"
  (see its own section below) since there's no page-switching job left
  for it there.
- **Click** the encoder: on Home, drills into the layer under the cursor.
  On `Global:Fm`/`Global:Pad`/`Global:Granular`/`Global:Mixer`
  specifically, it's instead this Global page's own entry point into a
  real top-level screen (`Screen::Fm`/`Screen::Pad`/`Screen::Granular`/
  `Screen::Mixer`) — the same encoder-click gesture, just repurposed
  per-page since Home's own drill-in doesn't apply outside Home. On
  `Screen::Fm`/`Screen::Pad`/`Screen::Granular` themselves (and every
  other Global page), a click mutes/unmutes all 4 loop layers
  (`Ui::TogglePauseAll()`) — there's no drill-in job left for it on those
  screens, so it's free for the same mute Global pages already use.
  Unused on `Screen::Mixer` (rotate already picks the stop).
- **Long-press** the encoder (~600ms): from Home, opens Global settings;
  from anywhere else — including `Screen::Fm`/`Screen::Pad`/
  `Screen::Granular`/`Screen::Mixer` — goes back to Home.

```
Home ──(click layer)──► Layer[n] ──(rotate)──► Status / Speed / Filter /
  │                                             Effect / Reverb / Gain
  │                                                     (long-press ⤴ back to Home)
  └──(long-press)──► Global ──(rotate)──► Tempo / Filter / Reverb / File /
       │                                    SdMgmt / Export / Fm / Pad /
       │                                    Granular / Looper / Mixer
       │                                       (long-press ⤴ back to Home)
       ├──(click on Global:Fm)──► Screen::Fm ──(rotate)──► Algo / Ratio / Index /
       │                                                   Op4 / Tune / ADSR /
       │                                                   Chorus / Vibrato / Filter /
       │                                                   Mix / ModAssign / Preset
       │                                                        (click ⤴ mute loop layers)
       │                                                        (long-press ⤴ back to Home)
       ├──(click on Global:Pad)──► Screen::Pad ──(rotate)──► Tone / Tune / ADSR /
       │                                                     Chorus / Vibrato / Filter /
       │                                                     Mix / ModAssign / Preset
       │                                                        (click ⤴ mute loop layers)
       │                                                        (long-press ⤴ back to Home)
       ├──(click on Global:Granular)──► Screen::Granular ──(rotate)──► Grain / Position /
       │                                                     TuneDirection / ADSR / Filter /
       │                                                     Mix / Capture / Trim / Preset
       │                                                        (click ⤴ mute loop layers)
       │                                                        (long-press ⤴ back to Home)
       └──(click on Global:Mixer)──► Screen::Mixer ──(rotate)──► 8 channel stops, then
                                                                  a Scope stop, wrapping
                                                                     (long-press ⤴ back to Home)
```

Fm and Pad are mutually exclusive (see *Per-engine on/off* below) — both
have their own Global page and full screen, but enabling one always
disables the other, since they fill the same "melodic voice" role and
share one Mixer channel.

Both knobs and both buttons are "soft" — their function depends on
whichever page is open, and it's always shown on the OLED's footer: two
rows, each with a label on either side of a small icon (a circle for the
knob row, a square for the button row). Button labels are dynamic where
it matters (e.g. Home's Button 1 reads "Rec"/"Pause/Overdub"/"Stop"
depending on the layer's current state, not one static label covering
several different behaviours).

**Knob pickup**: a knob only starts driving its parameter once its
physical position reaches the value already shown on screen (see
`Ui::KnobPickUp()`) — switching pages/layers never yanks a value to
wherever the knob physically happens to be sitting.

Full control map:

| Screen / Page | Knob 1 | Knob 2 | Button 1 (tap / hold) | Button 2 |
|---|---|---|---|---|
| Home | Master volume | Metronome volume | Rec/Pause/Overdub cycle for cursor layer (see transport state machine) | Toggle bypass |
| Layer: Status | Volume | Pan | Same transport as Home, for this layer | Hold 800ms = Clear |
| Layer: Speed | Speed (0.3x-2x, deadzone-centered on 1.0x) | — | Tap = reset to 1.0x | — |
| Layer: Filter | Cutoff (~20Hz-9kHz, log taper) | Resonance | Cycle filter mode (Off/Low/High/Band) | — |
| Layer: Effect | Effect param A | Effect param B | Cycle effect (Off/Drive/Bitcrush/Chorus/Tremolo/Phaser/AutoWah/Flanger) | — |
| Layer: Reverb | Send (into the shared reverb bus, see below) | — | — | — |
| Layer: Gain | Input gain (1x-4x) | — | — | — |
| Global: Tempo | BPM (40-240) | Bars per loop (1-16) | Toggle metronome | Hold 800ms = save as startup default |
| Global: Filter | Cutoff (master bus) | Resonance | Cycle filter mode | — |
| Global: Reverb | Size/decay (shared bus, see below) | Bypass reverb send (independent of every layer's own Send) | — | — |
| Global: Speed | Project vari-speed (0.3x-2x, deadzone-centered on 1.0x, same curve as Layer:Speed) | — | Toggle scrub mode (see below) | Reset to 1.0x |
| Global: File | Idle: nothing. ChoosingSave: Overwrite/Save New. BrowsingLoad chooser: Files/New, drilled-in: browse files | — | Idle: tap = reveal Save; chooser: tap = drill into file list (Files) or Back (New); elsewhere: tap = Back | Idle: tap = reveal Load; ChoosingSave/drilled-into-Load: hold 800ms = confirm |
| Global: SD Mgmt | Folder select / file browse (same list, always "Scroll") | — | Tap = drill into folder / back up a level; hold 800ms = Duplicate the browsed file | Hold `kSdMgmtDeleteHoldMs` (1500ms) = Delete the browsed file |
| Global: Export | — | — | Tap = render to native 48kHz WAV ("Studio") | Tap = render to 44.1kHz WAV for MicroDexed ("CD") |
| Global: Fm | — | — | Toggle Fm Synth on/off (mutex with Pad, see *Per-engine on/off* below) | — |
| Global: Pad | — | — | Toggle Pad Synth on/off (mutex with Fm, see *Per-engine on/off* below) | — |
| Global: Granular | — | — | Toggle Grains on/off | — |
| Global: Looper | — | — | Toggle the whole 4-layer loop system on/off (also resets `TempoClock`'s phase) | — |
| Global: Mixer | — (entry point + at-a-glance overview grid, see *Screen::Mixer* below) | — | — | — |
| Fm: Algo | — | — | Cycle `FastFmVoice::Algorithm` (Stack/Parallel/DualStack/YBranch) | — |
| Fm: Ratio | Op2 ratio (quantized) | Op3 ratio (quantized) | — | — |
| Fm: Index | Op2 index | Op3 index | — | — |
| Fm: Op4 | Op4 ratio (quantized) | Op4 index | — | — |
| Fm: Tune | Coarse tune, ±24 semitones | — | — | — |
| Fm: ADSR | Attack (or Sustain) | Decay (or Release) | Knobs → Attack+Decay | Knobs → Sustain+Release |
| Fm: Chorus | Depth | Rate | — | — |
| Fm: Vibrato | Depth | Rate (ceiling the mod wheel scales up to, see *ModDestination*) | — | — |
| Fm: Filter | Cutoff | Resonance | Cycle filter mode | — |
| Fm: Mix | Reverb send | Output level | — | — |
| Fm: Mod Assign | — | — | Cycle `ModDestination` (Vibrato/FilterCutoff/ChorusDepth) | — |
| Fm: Preset | Folder list: scroll folders. Inside a folder: scroll presets | — | Save / Open folder (or Select at top chooser) / Back | Tap = preview live (stays in browser); Hold 800ms = confirm + exit |
| Pad: Tone | Registration (dark↔bright oscillator morph) | Osc gain | — | — |
| Pad: Tune | Coarse tune, ±24 semitones | — | — | — |
| Pad: ADSR | Attack (or Sustain) | Decay (or Release) | Knobs → Attack+Decay | Knobs → Sustain+Release |
| Pad: Chorus | Depth | Rate | — | — |
| Pad: Vibrato | Depth | Rate (ceiling the mod wheel scales up to, see *ModDestination*) | — | — |
| Pad: Filter | Cutoff | Resonance | Cycle filter mode | — |
| Pad: Mix | Reverb send | Output level | — | — |
| Pad: Mod Assign | — | — | Cycle `ModDestination` (Vibrato/FilterCutoff/ChorusDepth) | — |
| Pad: Preset | Same Files/New chooser and browse-list convention as Global:File | — | Same Save/Load/Back convention as Global:File | Same hold-to-confirm convention as Global:File |
| Granular: Grain | Size (or Gap) | Fill (or Scan) | Knobs → Size+Fill | Knobs → Gap+Scan |
| Granular: Position | Position (Grain layer's fixed anchor) | — | — | — |
| Granular: TuneDirection | Tune (grain pitch, ±24 semitones) | Direction (Forward/Reverse/Random) | Toggle Map-to-Note | — |
| Granular: ADSR | Attack (or Sustain) | Decay (or Release) | Knobs → Attack+Decay | Knobs → Sustain+Release |
| Granular: Filter | Cutoff | Resonance | Cycle filter mode | — |
| Granular: Mix | Grain volume (or Reverb send) | Scan volume | Knobs → Grain+Scan volume | Knobs → Reverb send |
| Granular: Capture | — (Import mode: browse .wav files) | — | Cycle source: Direct Record → Layer 1..N → Import | Hold = record (Direct) or capture/import (Layer/Import) |
| Granular: Trim | Trim start | Trim end | — | — |
| Granular: Preset | Same Files/New chooser and browse-list convention as Global:File | — | Same Save/Load/Back convention as Global:File | Same hold-to-confirm convention as Global:File (live progress bar — audio-inclusive) |
| Mixer: Layer/Pad-or-Fm/Grains/Bypass | Volume | Pan | Knobs → Volume+Pan | Knobs → Reverb send |
| Mixer: Master | Volume | Reverb size | — | — |

BPM/Bars are locked once any layer holds a recording, and the knobs stop
affecting them, so you can't accidentally pull every track out of sync —
clear every layer to unlock and pick a new tempo.

### LED indicators

Both are set once per `Ui::Update()` call (`Ui::UpdateLeds()`), not from
the audio ISR:

- **LED1**: the cursor layer's state (Empty=off, ArmedCountIn=amber,
  Recording=red, Overdubbing=orange, Playing=green, Paused=blue) —
  whichever layer `cursor_layer_` currently points at, not a fixed layer.
- **LED2**: a steady Bypass on/off indicator (white when on). This used
  to flash with the metronome instead, as a visual tempo reference when
  the click was turned down — removed because that flash could only ever
  be as accurate as the main loop's call rate, which under heavy DSP load
  isn't perfectly steady, so it visibly stopped landing on the beat. A
  plain on/off readout has no timing to get wrong.

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

One `TempoClock`, shared by all 4 layers, is the single source of timing
truth:

- **BPM** (40-240) and **Bars** (1-16) together define the loop length
  every layer records to: `samples = 60/BPM * 4 beats/bar * Bars * sample_rate`.
  4/4 time is assumed throughout (not configurable — it's one constant in
  `tempo_clock.h` if you ever want to change it).
- The metronome is a single on/off switch governing both the audible
  click during playback *and* whether arming a recording does a count-in
  first. Metronome off = pressing record starts immediately. Metronome
  on = a 1-bar (4-beat) count-in always precedes recording, with an
  accented downbeat click.
- Once the first layer finishes recording, the tempo locks so every
  subsequent layer's recording is exactly the same length and they all
  loop in perfect sync — this is what "shared master length" means in
  practice.
- Changing BPM only affects *future* recordings once unlocked; it does
  not retroactively time-stretch anything already captured.

## Vari-speed and scrub

Global:Speed's Knob 1 is a project-wide tape-machine-style speed control
— `SpeedCurve01()` (`looper_layer.h`, shared with `LooperLayer::SetSpeed01()`
so the two controls feel identical), applied as a second multiplier on
top of whatever each layer's own Speed already is. This is genuine
vari-speed (pitch and tempo move together), not time-stretching — the
project explicitly doesn't attempt pitch-independent tempo change (see
Known limitations).

**Playback**: `LooperLayer::Process()`'s per-sample position advance
(`play_pos_ += speed_ * project_speed`) is the only change needed for
existing content to speed up/slow down correctly.

**Tempo/metronome**: `TempoClock::Process(float speed)` scales its own
phase accumulator the same way (`phase_samples_ += speed`), so the
metronome click, on-screen beat indicator, and bar boundaries all track
the *actual* audible tempo instead of the originally-locked one — without
this, the click would audibly drift out of sync with a sped-up/slowed-down
loop within a few bars.

**Recording/overdubbing while vari-speed isn't 1.0** is the correctness-
sensitive part: since every layer shares one fixed-length loop buffer,
newly captured audio has to land in the same native (1x) coordinate space
every other layer already lives in, or it wouldn't stay in sync with
everything else once vari-speed returns to 1.0x. Real input still arrives
1-per-real-sample regardless of `project_speed`, so both the fresh-Recording
write path and the Overdubbing write path now track a fractional native-
space write cursor that advances by `project_speed` per real sample
(mirroring `play_pos_`'s own advance) and linearly interpolate between the
current and previous real input sample whenever a step needs to fill more
than one native slot (`project_speed > 1`, upsampling) or skip some
(`project_speed < 1`, decimation, same "no anti-alias filter"
simplification the existing playback read path already accepts). This is
also a deliberate creative surface, not just a compatibility fix: record
something while sped up, and it comes back lower and slower once you
return to 1.0x — the classic tape vari-speed trick — because what got
captured is honestly in the same coordinate space as everything else, at
whatever relative pitch you performed it against.

**Scrub**: Button 1 on Global:Speed toggles `scrub_mode_active_`, which
`HandleEncoder()` checks *before* its normal page-cycling logic — while
it's on, encoder rotation calls `Ui::ScrubBy()` instead of moving between
Global's sub-pages. `ScrubBy()` nudges every non-empty layer's `play_pos_`
by the same raw-sample amount via `SetPlayPosRaw()` (the same accessor
`PerformanceStore::ExportWav()`'s snapshot/restore already proven safe for
direct position manipulation), with **turn-speed acceleration**: it
tracks the time between consecutive scrub ticks (`daisy::System::GetNow()`)
and scales the per-click distance up when ticks arrive close together (a
fast spin), from a base ~50ms of audio per click up to an 8x cap
(~400ms/click) for fast turns — slow, deliberate turns stay fine-grained.
Scrubbing also calls `TempoClock::SetPhaseToPosition()` (using the same
lowest-indexed non-empty layer the composite waveform's playhead reads)
so the metronome/beat indicator/bar-start jump to match wherever scrub
moved the audio, instead of continuing to free-run from wherever they
already were — without this, scrubbing back would leave the audio and
the metronome permanently out of sync by however far you scrubbed.
Scrub mode auto-clears the instant you leave Global:Speed (including via
the normal long-press-to-Home), so the encoder can never get stuck
scrubbing somewhere it shouldn't.

Global:Speed's display is a composite waveform (per-column max across all
4 layers' existing `waveform_peaks_` caches, not just one layer) with a
single shared playhead — reusing the same drawing routine Layer:Status
uses for its own single-layer view (`Ui::DrawWaveform()`), just fed a
synthesized peaks array and a different position source.

**Persistence**: vari-speed is a live-performance control like Master
Volume — it always resets to 1.0x on boot and after `Load()`, and is
never written into a saved performance (no `FileHeader`/version changes
at all for this feature). Unlike Master Volume, though, it's *not*
excluded from `ExportWav()` — whatever it's set to when Export is
pressed gets rendered into the file on purpose, since vari-speed is
something dialed in as a deliberate part of a performance, not an
incidental monitor-level knob position. This does mean an export's frame
count depends on `project_speed` too: `ExportWav()` computes a
`render_len` (`total_samples / project_speed`, i.e. how many render ticks
complete one full pass at that speed) and drives everything -- the render
loop, the WAV header's `data_size`, and (for the MicroDexed/CD path) the
`Resampler48to44_1` frame count -- from that instead of the raw native
loop length, so a sped-up export is correctly shorter and a slowed-down
one correctly longer.

## Overdubbing

Long-press-and-hold the transport button on any layer that's Playing or
Paused to overdub: new input is additively mixed into the existing loop
buffer, sample-for-sample, in sync with playback, then clamped to ±1.5 to
stop runaway buildup over many passes. Release to stop. Works identically
on every layer.

## Per-layer signal chain

Applied on **playback only**, never baked into the recorded buffer — so
retakes, overdubs, and tweaking any of this are always working with
clean material:

```
looped sample -> Filter (Svf: Low/High/Band) -> one selectable character
                  effect -> Volume/Pan -> this layer's Reverb send ->
                  ONE shared reverb bus, Process()'d once per sample in
                  main.cpp -> mixed to output bus
```

- **Filter**: cutoff and resonance are both live, turnable while the
  loop plays. Off/Low-pass/High-pass/Band-pass, cycled with Button 1.
- **Character effect** (one per layer, DaisySP-backed, mutually
  exclusive — pick one): Drive (Overdrive), Bitcrush (Decimator — Param
  A = bit depth, Param B = downsample), Chorus (Param A = depth, Param B
  = rate — fed the left channel only, generates its own stereo spread, a
  deliberate simplification), Tremolo (depth/rate), Phaser (depth/rate),
  AutoWah (wah amount/level), Flanger (depth/rate). Switching effect type
  resets both params to 0 (a genuine "off" state for every one of these,
  verified against DaisySP's actual source, not assumed) so a newly
  selected effect never starts already dialed in loud — you turn each
  knob up from 0 to bring it in.
- **Reverb**: ONE shared `ReverbSc` bus (`main.cpp`'s `fx_reverb_shared`),
  not one per layer. Every layer's Send-scaled signal is summed into a
  shared accumulator (`LooperLayer::Process()`'s `reverb_send_out`
  parameter) and run through the single instance once per sample, in
  `AudioCallback()`, after every layer's own `Process()` call. Send stays
  independent per layer; Size/decay (Global:Reverb) is one setting shared
  by everyone's send. This used to be 4 independent instances (one per
  layer) — that was by far the heaviest thing in this signal chain
  (~386KB per instance) and, in the worst case (reverb active on all 4
  layers at once), was enough real-time DSP cost to starve the main loop/
  TIM5 badly enough to cause actual audio glitches and OLED corruption,
  not just UI lag. Consolidating to one instance cut that worst case
  ~4x. See *Boot process: bootloader + QSPI flash* below for the current,
  real-hardware-measured worst-case CPU number.

### Bypass reverb send

Bypass (Home, Button 2 — the live-input monitor mix, e.g. for a powered
mic plugged straight into the Pod) has its own independent send into
that same shared reverb bus, set on Global:Reverb's Knob 2. In
`AudioCallback()` this is one small block right before the shared
reverb's `Process()` call (the same `bypass_gain`-scaled, L+R-summed-to-
mono treatment the existing dry bypass mix already uses, for the same
reason — see *Bypass sums to mono* below): if Bypass is on and the send
is above zero, the live input adds into `reverb_send_l/r` alongside
whatever the 4 layers are already contributing, then the one shared
`ReverbSc` processes the combined total as usual. No new reverb
instance, no measurable extra cost — a multiply-add per sample. Turning
Bypass off silences this contribution too, and recordings stay
completely dry regardless of this setting (it only ever feeds the
monitor-mix reverb tail, never the recording path).

### SDRAM-placed effects are explicitly zeroed before use

`Phaser` (per-layer) and `ReverbSc` (the shared bus, plus its counterpart
in `PerformanceStore::ExportWav()`) both live in SDRAM — libDaisy's
`sdram.h` documents, and this project's linker script/startup code
confirm, that `.sdram_bss` is **not** zero-initialized at boot, unlike
ordinary SRAM `.bss`. `LooperLayer::Init()` and the equivalent setup in
`main.cpp`/`performance_store.cpp` `memset()` each of these objects to
zero before calling their own `Init()`. This isn't cosmetic: it was a
real, reproducible bug, first found via the (since-removed, see *Pitch
removal* below) `PitchShifter` effect, whose `Init()` didn't touch
several of its own internal fields, so without zeroing first they started
out holding raw leftover SDRAM contents, and on some boots that decoded
as NaN/Inf. Since NaN survives a plain min/max clamp unchanged (NaN
compares false against both bounds) and DaisySP's `ReverbSc` has no
NaN/Inf guard anywhere in its feedback path, one bad sample could
permanently poison the shared reverb bus's internal state — silencing the
*entire* mix, not just that layer, until a full power cycle. The zeroing
is kept as cheap insurance against this exact failure shape for any
current or future SDRAM-placed effect, not just the one that first
surfaced it.

### Bypass's own mix volume/pan

Home's Bypass (Button 2) mixes live input directly into the output; it
now has its own Volume/Pan on `Screen::Mixer`'s Bypass channel,
independent of the per-layer recording input **Gain** (which only ever
affects what gets *captured*, not the Bypass monitor mix). Like Master
Volume and vari-speed, these are live-performance controls, not part of
a saved performance — they reset to their defaults (Volume 0.8, Pan
center) on boot and after `Load()`, never persisted to `PERF/*.DAT`.

## MIDI

Two MIDI-played instruments — Pad Synth and Grains, both described below
— run alongside the looper, driven by the Pod's built-in MIDI IN jack
(`hw.midi`, a `MidiUartHandler` on the Pod's UART). One real hardware
quirk this needed working around: libDaisy's default MIDI transport
config claims the same physical pin as the encoder's own click button
(`D13`) for MIDI TX, but the Pod's MIDI jack only ever wires up RX per
Electrosmith's own pinout — that TX claim is never actually usable here.
`main.cpp` re-inits MIDI with TX explicitly disabled
(`Pin(PORTX, 0)`, the standard "don't claim this pin" sentinel) and
re-inits the encoder afterward to reclaim `D13`, rather than relying on
TX simply never being driven.

MIDI is polled from the **1kHz control-rate callback**, not the main
loop — the same reasoning buttons already use: the main loop's OLED
redraw is a blocking I2C write, and if a redraw stall delayed draining
the UART, a very fast note tap's NoteOn and NoteOff could end up sitting
in the queue together, both getting drained in the same pass with no
audio block ever seeing the note as held — a silent note instead of a
short blip. Pitch bend updates read more naturally at this steady rate
too, for the same reason.

- **NoteOn/NoteOff** currently drive **both** Pad Synth and Grains from
  the same incoming stream — there's no per-engine MIDI channel or
  screen-based routing yet (flagged in `main.cpp` as a temporary,
  first-pass simplification). Playing a note plays it on whichever
  engines are enabled (see *Per-engine on/off* below) — Pad Synth and
  Fm Synth both receive it (only one is ever actually enabled at a time,
  see their mutex), and so does Grains.
- **Pitch bend** (±2 semitones, the MIDI standard default) feeds Pad
  Synth and Fm Synth (`SetPitchBendSemis()` on each) — Grains has no
  pitch bend input.
- **Mod wheel** (CC1) feeds Pad Synth's and Fm Synth's own mod-wheel-
  assignable destination (Vibrato/FilterCutoff/ChorusDepth, see below,
  each engine has its own independent assignment) — Grains has no mod
  wheel input.

## Per-engine on/off

`Global:Fm`/`Global:Pad`/`Global:Granular`/`Global:Looper` each carry a
plain Button-1-tap on/off toggle (`fm_enabled_`/`pad_enabled_`/
`granular_enabled_`/`looper_enabled_`) — a real CPU lever, not just a
mute: `AudioCallback()` skips calling that engine's `Process()` entirely
while disabled and writes silence instead, so someone using only Grains
(say) genuinely gets the other engines' CPU share back, not just their
silence. Switching the looper off also resets `TempoClock`'s phase
(bar/beat position, count-in state, click envelope) back to bar 1 beat 1,
so turning it back on always resumes cleanly rather than picking up
mid-phase from wherever it happened to be frozen.

**Fm and Pad are mutually exclusive** — `fm_enabled_`/`pad_enabled_` are
enforced as a real mutex, not just two independent toggles that happen to
usually be used one at a time: toggling either one's Button 1 on
`Global:Fm`/`Global:Pad` forces the other off. They fill the same
"melodic voice" role and share one Mixer channel (see *Screen::Mixer*
below), so there's no meaningful case for both being live simultaneously
— unlike Grains, which is independent of both and can run alongside
either.

## Fm Synth (Screen::Fm)

A MIDI-played, polyphonic FM instrument (`fast_fm_voice.h`, `fm_synth.h/
.cpp`) built as Pad Synth's sibling and eventual CPU-cheaper alternative,
not a full replacement — see *Per-engine on/off* above for how the two
coexist via a mutex. Ownership model, page/knob conventions, and most
member names are deliberately mirrored 1:1 from `pad_synth.h` so its own
Ui pages/`KnobContext`/save-load plumbing carry over with minimal
changes (ModDestination routing, curved-seconds ADSR, linear pan law,
etc.) — not repeated here where they're identical.

**Why FM, and why it took several real-hardware measurement rounds to
get right**: the motivating question was whether FM synthesis could
support more simultaneous voices than Pad Synth's own divide-down
oscillator-bank approach for the same CPU budget. The honest answer
turned out to be "yes, but only once two real bugs were found and fixed,
and the answer isn't a big margin" — worth recording in full since the
same class of bug (code silently landing in slow QSPI flash instead of
fast ITCM memory) has now bitten this project twice.

1. **Stock `daisysp::Fm2` measured worse than Pad Synth, not better** —
   32% CPU for 6 voices in isolation (vs Pad's own 25% for 6), because
   `Fm2`'s `Oscillator` calls `sinf()` twice per sample per voice with no
   hardware transcendental unit backing it on this chip.
2. **A custom lookup-table sine oscillator (`FastFmVoice`)**, table size
   256 with linear interpolation (same idiom as Grains' own Hann window),
   measured 10% for a 2-operator/6-voice version — confirming `sinf()`
   was the real cost, not FM synthesis itself.
3. **Voice/operator count was tuned by direct measurement, not
   estimation** — 4 operators at 6 voices measured 21%, at 8 voices 28%;
   2 operators at 12 voices measured 28% with unclear polyphony value.
   **8 voices × 3 operators, at 20%**, was settled on as the balance
   (more voices AND still cheaper than Pad Synth's own 6-voice engine) —
   until operator count later grew to 4 (see point 5 below) and the
   voice count was revisited (point 6).
4. **A real cache-thrashing bug, found via the CPU number itself
   spiking** (measured ~21% cold, climbing to 57-64% after Pad Synth had
   also run for a while): each `FastFmVoice` instance originally owned
   its own private ~1KB sine table, so 6-8 separate copies were scattered
   through memory. Every time Pad Synth's own `Process()` (touching
   plenty of its own SRAM state) ran in the same audio block, it evicted
   those tables from cache, forcing the FM loop to reload from a
   different location per voice on the next block. Fixed by making the
   table a single shared function-local `static` array
   (`FastFmVoice::SineTable()`), built lazily once across every instance
   — single-core, so no thread-safety concern, since `Init()`/`Process()`
   only ever run from the same audio context.
5. **A 4th operator, and a genuinely richer algorithm set, added once 3
   operators felt musically thin** (see *Algorithms* below) — measured
   safe to add given the 8-voice/3-op number's own headroom, at the cost
   of revisiting the voice count once more (point 6).
6. **A second real ITCM/QSPI placement bug, found from a user report of
   "the screen goes sluggish when Fm is active"**, not a proactive
   measurement this time. `FmSynth::Process()` is tagged `DSY_ITCM_TEXT`
   (see *Boot process* below for what that means and why it matters), but
   the actual hot inner loop it calls — `FastFmVoice::Process()`,
   `ReadTable()`, `WrapPhase()`, `SineTable()`, and `SetFrequency()` —
   are all defined inline in a header and were relying on the compiler
   choosing to inline them into that one ITCM-tagged function. Once the
   4th operator and 4 algorithm branches (point 5) grew `Process()`
   enough, the compiler evidently stopped inlining all of it, and any
   call left out-of-line silently fell back to QSPIFLASH — dramatically
   slower per-instruction than ITCM. The tell: Fm's own measured CPU cost
   stayed high (~50%+) even with **no notes held**, which only makes
   sense if the cost is in how slowly the code executes rather than how
   much DSP work it's doing, since every voice always runs its full
   operator math regardless of note state. Fixed with
   `__attribute__((always_inline))` on the small helpers (a plain
   `DSY_ITCM_TEXT` tag on them directly hits a *different* real GCC
   error — "section type conflict" — since mixing a `static` and a
   non-`static` member function, or a header-inline function and a
   regular out-of-line one, under the same explicit linker section gets
   different COMDAT/linkage treatment that GCC refuses to merge; forcing
   inlining sidesteps this by leaving no separate out-of-line symbol at
   all). This recovered a real, measured ~30% relative reduction in Fm's
   own CPU share (worst-case ~51-59% down to ~40-45%) with zero behavior
   change — pure code-placement, same lesson as the original `BOOT_QSPI`
   migration's own `PitchShifter` gap (see *Boot process* below).
7. **Voice count dropped from 8 to 6** after the above, once real
   hardware testing showed the screen was "a little less sluggish" but
   not fully back to normal even with both ITCM fixes in — the same
   voice-for-headroom trade already made once for the original balance,
   just re-applied now that 4 operators cost more per voice than 3 did.
   Measured **~52-57% Fm-alone CPU** (idle/playing) at 6 voices, and
   **~63% total with 3 loop layers actively playing back alongside it**
   — confirmed on hardware as "much better."

**Algorithms** (`FastFmVoice::Algorithm`, cycled on the dedicated Algo
page, shown as a small Yamaha-DX7-style routing diagram): every algorithm
runs the exact same 4 operators' worth of table-lookup-and-phase-math per
sample regardless of which is selected (`Process()` always does exactly
4 `ReadTable()` calls and 4 phase advances), so switching is a real,
essentially-free sound-design choice, not a CPU trade-off.

- **Stack**: Op4 → Op3 → Op2 → Op1/carrier, one deep serial chain — the
  most complex/evolving single-path timbre.
- **Parallel**: Op2, Op3, and Op4 all modulate the carrier directly and
  independently, summed — the widest/densest single-carrier tone
  available.
- **DualStack**: two fully independent 2-operator chains, both carriers,
  summed and halved (Op2→Op1 and Op4→Op3) — more additive/detuned-pair
  character than a single modulated carrier. Real classic FM electric
  pianos use close to this shape (two "towers," one warm/lightly
  modulated for the sustained body, one bright/heavily modulated for the
  struck transient) — see *Presets* below.
- **YBranch**: Op3 and Op4 both modulate Op2 in parallel, and Op2's own
  (doubly-modulated) output then modulates Op1/carrier — a fork feeding a
  chain, richer than Stack's single-modulator-per-stage without the
  extra carrier Parallel/DualStack add.

The Algo page's diagram lays these out vertically (modulator above the
operator it feeds, carriers dropping a stub onto a shared "OUT" line),
matching the actual convention real DX7-family charts use — Stack's
4-deep chain snakes through a 2x2 grid rather than one tall column, since
a single column of legibly-sized boxes doesn't fit this display's height,
which turns out to be exactly how real charts handle their own longer
chains too. The Ratio/Index/Op4 pages each additionally show a compact
one-line routing summary (e.g. `Route: 4>3>2>1` for Stack) above their
oscilloscope, since which stage Op2/Op3/Op4 actually feed changes
per-algorithm and the static "Op2"/"Op3"/"Op4" labels alone don't convey
that.

**Ratio quantization**: Op2/3/4's ratio (relative to the carrier, which
is always fixed at 1:1 with the held note) is quantized to a 10-entry
table of musically-clean multipliers (0.5, 1, 1.5, 2, 3, 4, 5, 6, 7, 8x),
not a continuous sweep — a real, user-reported bug ("sounds quite out of
tune") traced to exactly this: FM only sounds harmonic when a
modulator's frequency is a clean multiple of the carrier's, and a
continuous knob almost never lands on one, producing inharmonic,
clangorous overtones by default. Real FM hardware avoids this with a
stepped "coarse" ratio control rather than a sweep, for the same reason.
`SetOp2Ratio01()` etc. still take a plain 0..1 knob position, quantizing
to the nearest table entry with the project's usual bucket-center
rounding convention; `GetOp2Ratio()` etc. return the resolved multiplier
for display. Fixing this also surfaced a genuine pre-existing type bug:
`op2_ratio_idx_`/`op2_ratio_` had been declared on one shared `int, int`
multi-declarator line, silently truncating every fractional ratio to its
integer part.

**Index/depth**: mapped linearly 0..5 per operator (matching
`daisysp::Fm2`'s own "5 = a full 2π radians of phase excursion"
convention) — depth/amount has no "in tune" concept the way ratio does,
so a continuous sweep is correct here.

**Presets** (`FmSynth::FmPresetData`, `kNumFactoryPresets = 36`): unlike
the 3-operator version's initial "no factory range yet" state, this
engine now ships 36 hand-tuned patches across 8 named categories (Basic,
E.Piano, Bells, Mallets, Bass, Brass, Pad, Lead — `kFactoryCategories` in
`fm_synth.cpp`), grouped as contiguous ranges of the same flat
`kFactoryPresets` array `GetFactoryPreset()`/`LoadFmPreset()` already
address by a single 1-based slot number (the category grouping is purely
a UI browsing aid layered on top, not a different storage shape — see
*Preset folder browsing* below). Designed from real classic-FM-synth
patch references (DX7/TX81Z-era electric pianos, bells, basses, brass),
not just guessed: notably, several bell/glass/metallic patches (Church
Bell, Glass Bells, Tubular Bell, Music Box, Metallic Pluck) were
corrected to root one operator on a **1.5x ratio** rather than a clean
integer one, since real bell/inharmonic FM patches rely on a ratio close
to √2 (~1.4:1) for their characteristic detuned quality — any integer
ratio, even a high one, is still harmonic and reads more like a bright
buzz than an actual bell. 1.5x is this project's closest quantized match
(see *Ratio quantization* above for why the table doesn't include 1.4
directly). The electric-piano-family patches (Classic E.Piano, Soft EP,
Bright EP, Slap Bass, Marimba, Kalimba, Bright Lead) lean on DualStack's
two-tower shape for exactly the reason described under *Algorithms*
above.

**Preset folder browsing** (`FmParamPage::Preset`, `Ui`'s
`fm_preset_folder_cursor_`/`fm_preset_folder_open_`): the Load side of
the usual Files/New chooser (see *The Files/New Load chooser* below)
gains one extra level here versus Pad's own flat numbered list — a
folder list (the 8 named categories plus one trailing synthetic "User"
folder holding every SD-saved slot) is browsed first, then Button 1
opens the highlighted folder to browse the presets inside it, or backs
back out to the folder list if one's already open. `fm_preset_cursor_`
means "index within the currently open folder" here, not a flat index
across everything — resolved to an actual slot via
`Ui::ResolveFmPresetSlot()` (a factory category's local index through
`FmSynth::GetFactoryCategorySlot()`, or `fm_preset_user_slots_[cursor]`
for the User folder). Entering `BrowsingLoad` fresh always resets back to
the folder list, never wherever it was last left drilled into.

**Live preview while browsing** (`HandleButton2()`'s `Screen::Fm` case):
once a folder is open and a preset is highlighted, a *short* Button 2 tap
applies it to the live engine immediately without leaving the browser or
resetting `save_load_mode_` — added specifically so scrolling Knob 1 and
tapping Button 2 repeatedly auditions one preset after another while
searching for a sound, instead of needing to fully commit-and-re-enter
the Save/Load flow for every single preset tried. Button 2's *hold*
gesture still does the same load AND exits back to `Idle`, for once
you've settled on one — the footer label switches to "Prev./Hold=Load"
in this state to make both behaviors visible on screen.

**Voice count**: `kMaxVoices = 6` (see the CPU-measurement story above
for why, down from an initial 8) — every loop/array in the class already
keys off this one constant, so it's the only line that needs to change
if the trade-off is revisited again.

**Voice allocation, Tune, Envelope, Chorus, Vibrato, Filter, Mix, Pan**:
identical to Pad Synth's own (see below) — same oldest-note voice
stealing, same curved-seconds ADSR convention, same one-shared-Chorus-
instance bus effect, same mod-wheel-gated triangle-LFO vibrato, same
bus-level `Svf` filter pair, same linear pan law. Not repeated here.

## Pad Synth (Screen::Pad)

A MIDI-played, polyphonic pad instrument (`pad_synth.h/.cpp`), meant for
warm/lush sustained sounds (chords held via a MIDI keyboard), not a
percussive/plucked instrument. Owns its own DSP state directly as
members, the same ownership model `LooperLayer` already uses for its own
`Svf` filter pair — a single self-contained instrument with its own
filter/level/send, not a cross-cutting bus effect like `main.cpp`'s
master filter.

**Architecture, proven cheap on real hardware before this class was
written** (measured directly in `main.cpp` with the same DWT
cycle-counter technique used throughout this project): `kMaxVoices` ×
`daisysp::OscillatorBank` (a divide-down organ/"string synth" oscillator
— no transcendental calls at all in its `Process()`), each with its own
`daisysp::Adsr` envelope (also pure arithmetic per sample), plus ONE
shared `daisysp::Chorus` on the summed bus. At 8 voices this cost 49%
worst-case CPU alongside a full 4-layer loop + reverb already playing.
`daisysp::StringVoice` (Karplus-Strong) was measured and ruled out
instead — a SINGLE voice alone cost 68% (two `powf()` + one `atanf()`
every sample, unconditionally), which is why the additive/organ approach
was chosen over a plucked-string model.

**Voice count**: `kMaxVoices` was reduced from 8 to **6** after measuring
the genuine worst case with everything running at once (4 loop layers +
reverb + full Pad Synth + Grains, all simultaneously active) at ~94% —
a direct, proportional trade-off of polyphony for headroom, not a free
optimization. Every loop/array in the class already keys off this one
constant, so this is the only line that needs to change if that
trade-off is revisited (e.g. under the FM-synth experiment mentioned in
project history, where a cheaper per-voice cost might afford more
voices back).

**Voice allocation**: `FindVoiceForNote()` prefers an already-silent
voice (its `Adsr` in `ADSR_SEG_IDLE`) that isn't currently held, then a
genuinely free (never-triggered) voice, then falls back to stealing the
oldest currently-held voice (`triggered_at`, a simple monotonic counter)
— standard oldest-note voice stealing, no special-casing beyond that.

**Tone/Registration**: `SetRegistration01()` is a single-knob morph
between a hand-tuned "dark" (weighted toward 8'/4' organ stops) and
"bright" (weighted toward 2'/1') `OscillatorBank` amplitude vector,
applied identically to every voice — a property of the instrument's
overall character, not a per-note thing.

**Tune**: a coarse ±24-semitone transpose, discretized the same way
Grains' own `GetGrainTuneSemitones01()` is (see *Grains* below and its
bucket-center rounding note) — recomputed at control-rate
(`tune_rate_ = powf(2, semitones/12)`), not per sample, same as pitch
bend's own `bend_ratio_`.

**Envelope**: attack/decay/release use the project's existing curved-
seconds convention (`kMinAdsrSeconds`=5ms .. `kMaxAdsrSeconds`=3s,
exponential) so a knob's physical travel doesn't visually "do nothing"
for most of its range the way a raw-01-to-seconds mapping would — the
same math already backing the ADSR-shape graph drawn on screen.

**Chorus**: one shared `daisysp::Chorus` instance on the summed voice
bus (Depth/Rate knobs), not per-voice — the same "one shared instance,
not N" reasoning as the loop layers' shared reverb bus.

**Vibrato**: a cheap phase-accumulator triangle LFO (not a
`daisysp::Oscillator` instance — this project already has the identical
`phase_ += inc; wrap` pattern elsewhere, e.g. `TempoClock`), scaling
fractional FM up to `kVibratoMaxDepthFraction` (0.06, ≈1 semitone at
full depth). The mod wheel gates/scales it (wheel at 0 = no vibrato
regardless of the Depth knob, wheel at 1 = the full Depth-set amount) —
Vibrato has no meaningful "always-on" base the way Filter/Chorus's mod
destinations do, so this is a ceiling-and-scale relationship, not an
additive one.

**Mod wheel destinations** (`ModDestination`, cycled by the Preset
page's neighbor, Mod Assign): Vibrato (see above), FilterCutoff, or
ChorusDepth — the wheel routes additively on top of whichever page's own
knob as the base value for the latter two; Vibrato is the one exception
(no separate base, a still wheel means no vibrato at all).

**Filter**: one bus-level `Svf` pair (post-chorus, pre-reverb-send),
independent of `main.cpp`'s own master-bus filter, sharing the same
`FilterMode`/curve it uses.

**Mix**: Reverb Send and Output Level, both reachable identically from
this engine's own Mix page and from `Screen::Mixer`'s Pad channel — same
underlying value either way, so changing it in one place updates the
other with no extra sync needed.

**Pan**: linear pan law (`panL = 1-v`, `panR = v`), the same law
`LooperLayer::SetPan01()` uses — kept consistent so Pad Synth pans the
same way the loop layers do at the same knob position, rather than a
"nicer" equal-power curve that would behave differently from everything
else in the mix. Applied to both the dry output and the reverb send
(post-pan), again matching `LooperLayer`'s own `Process()`.

**Presets** (`PadSynth::PadPresetData`): a flat snapshot of every
setting above. 14 hand-tuned factory presets are embedded in firmware
(not SD files), always available even on a blank/unformatted card —
index 0 ("New") is the neutral/default preset, and `Init()` applies it
directly, so what boots is always exactly preset 0, not a second,
separately-hand-coded default that could quietly drift out of sync with
it. User presets save/load to `PADP/PRESnnn.DAT` (up to 99 slots, on top
of the always-available factory range) via the same Save/Load/Back
convention as Global:File (see *Save/load* below). A performance save
does **not** embed the Pad sound (or Fm's, or Grains') at all — see
*Save-file "bubbles"* below for why that changed and what it means in
practice.

Two fields (`tune01`, `pan01`) were added to `PadPresetData` after most
of the factory presets were already written — both are trailing fields
with default member initializers (0.5f = neutral/centered), so every
existing positional initializer list (which only lists the fields that
existed at the time it was written) still gets a sensible neutral value
with no need to touch any of them.

## Grains (Screen::Granular / GranularEngine)

A MIDI-played, **monophonic** granular instrument (`granular_engine.h/
.cpp`) — deliberately much simpler than an earlier 8-voice granular
engine this project shelved on its own git branch: one held note at a
time, no oscillator layer (Pad Synth covers that role now), and two
overlapping-grain layers sharing one Size/Fill/Gap scheduling mechanism
instead of one polyphonic grain cloud plus a separate, thinner "scan
grain".

**Does not own its captured buffer** — `SetSource()` just points the
engine at whatever buffer currently holds a capture (SDRAM, owned by
`main.cpp`), the same convention the earlier engine used. `len == 0`
means "nothing captured yet" (`NoteOn()` is then a no-op); setting a new,
possibly-shorter source outright silences both grain clusters, since a
grain's position was computed against the *old* `src_len_` and could
otherwise read out of bounds against a shorter new capture.

**Two grain layers, one mechanism**: both **Grain** (a fixed-Position
anchor) and **Scan** (a continuously-sweeping anchor, bouncing between
Scan Start/End at a speed/direction set by the Scan knob) are the exact
same `GrainCluster` — `kGrainsPerVoice` = 3 overlapping grain slots
(3-way overlap is the standard granular-synthesis compromise for a
smooth, click-free texture at any grain size), scheduled from the same
Size/Fill/Gap values. This is a deliberate redesign, not a port: the
earlier engine's Scan was a single retriggered grain on its own timer,
and even after several rounds of tuning it stayed thinner/choppier than
the main grain cloud, because one voice retriggering periodically is
inherently less smooth than several overlapping ones. Giving Scan the
identical multi-slot scheduling the Grain layer already uses fixes that
by construction rather than by tuning constants further.

**Size/Fill/Gap** (shared by both layers, since they're reading the same
kind of grains, just from different anchors):
- **Size**: grain length, `kMinGrainMs`(1ms)..`kMaxGrainMs`(500ms).
- **Fill**: how many of the `kGrainsPerVoice` slots are active (0 =
  silence). Renamed from the earlier engine's "Grain Count" to match the
  reference hardware granular device this redesign is modeled after.
- **Gap**: 0 = grains packed with no gap (hop = grain length / Fill), 1 =
  maximally sparse (real silence between grains). Renamed AND
  polarity-flipped from the earlier engine's "Density" (where 1.0 meant
  "packed") to match that same reference device's own "Gap" vocabulary,
  which goes the other way.

**Position/Scan**: Position is a real knob-controlled fixed anchor for
the Grain layer. Scan uses the same dead-zone-centered bidirectional
curve as this project's own vari-speed/scrub controls: center (0.5,
default) is off, above/below picks which end of the Scan Start/End range
it heads toward first, speed proportional to distance from center.

**Tune / Map to Note**: Tune is a fixed per-grain pitch offset
independent of the held note; Map to Note additionally tracks the note's
own pitch (12-TET against middle C) on top of Tune. `SetGrainTuneSemitones01()`
quantizes to whole semitones (±24) — its matching getter,
`GetGrainTuneSemitones01()`, returns each bucket's *center*, not its
edge: a discretizing setter using `(int)(v01*N + 0.5f)` must have its
getter return `(value+offset)/N`, not `(value+offset+0.5f)/N` (which
would land on the bucket edge and round-trip to the next semitone up on
every save/reload) — a real bug once found and fixed here, and
deliberately avoided when `PadSynth::GetTuneSemitones01()` was written
afterward using the same pattern.

**Direction**: per-grain read direction, Forward/Reverse/Random,
quantized from one knob. Spray (randomized per-grain timing jitter) was
dropped in this redesign — it was a source of real, hard-to-diagnose
jitter bugs in the earlier engine.

**Note-level ADSR**: shapes the whole voice's overall loudness across a
held note, layered ON TOP of (not instead of) a fixed per-grain Hann
window (a 256-entry lookup table, `ReadHann()`) — the Hann window only
prevents clicks within a single grain, it has no swell-in/tail-off of
its own, which is what the ADSR adds. Same curved-seconds convention as
Pad Synth's own ADSR.

**Monophonic voice logic**: `NoteOn()` always retriggers the single
voice (last-note priority, plain retrigger, no legato/portamento).
`NoteOff()` only releases if it matches the currently-held note, so
releasing an old note after a new one has already retriggered doesn't
cut the new one short. A genuinely different note interrupting one still
sounding calls `ChokeCluster()` on both clusters — moving every
currently-active grain into its own release-fade slot (one per grain
slot, not one shared slot, so several simultaneously-active grains each
fade independently) rather than an audible instant cut.

**Capture**: three sources, all landing in the same SDRAM-owned buffer
`main.cpp` allocates:
- **Direct Record** — Button 2 held on the Capture page streams live
  input straight into the buffer for as long as it's held (or until the
  buffer's fixed capacity is reached).
- **From Layer** — an instant copy of whichever loop layer is currently
  selected as the capture source (independent of Home's own cursor) into
  the same buffer, then points the engine at it via `SetSource()`.
- **Import** — reads a user-supplied `.wav` file from the SD card's
  `IMPORT/` folder (16-bit PCM, mono or stereo, 48000 or 44100 Hz; 44.1kHz
  files are resampled up to the engine's native 48kHz with the same
  exact-ratio linear resampler `ExportWav()` already uses in the other
  direction). Anything else (24-bit, non-PCM, other rates) is cleanly
  refused rather than misread.
- **Trim**: non-destructive start/end trim over whatever's currently
  captured — the underlying buffer is untouched, only the sub-range
  passed to `SetSource()` changes, so re-trimming always works from the
  original full capture, not whatever the previous trim left behind.

**Filter/Mix/Reverb send/Pan/Output level**: same shape as Pad Synth's
own (see above) — one bus-level `Svf` pair, independent Grain-layer and
Scan-layer volumes summed before a single Output Level stage, a
continuous Reverb Send knob (previously an unconditional full send with
no control at all), and the same linear pan law applied to the
already-stereo captured signal (Grains reads real L/R from whatever was
captured/imported, unlike Pad Synth's mono voice sum).

**Presets** (`GranularEngine::GranularPresetData`): unlike Pad Synth,
Grains presets are **audio-inclusive** — a Grains preset IS a specific
captured sound plus how it's being played back, so saving/loading one
needs to restore both. `PerformanceStore::SaveGranularPreset()`/
`LoadGranularPreset()` stream the captured audio alongside the
parameters into ONE combined file per slot (`GRNP/PRESnnn.DAT`, up to 99
slots — no factory range, there's no hand-tuned-capture equivalent to a
hand-tuned patch), so this can take real SD-transfer time (up to ~1.9MB
of audio) and shows a live progress bar during the save/load, unlike Pad
Presets' instant tiny-struct transfer. Output Level and Reverb Send are
deliberately excluded from the saved struct (same reasoning as Pan) —
session-level mixer settings, not part of the captured sound's own
identity.

## Screen::Mixer

One screen covering all 8 mixable channels — Loop Layers 1–4, Pad
Synth, Grains, Bypass, and Master — reached by clicking the encoder on
`Global:Mixer` (which itself shows a static at-a-glance overview grid:
all 8 channel names in a row, each with a real vertical Volume bar, no
numeric readout, via `DrawMixerOverviewGrid()`).

**Rotate picks the channel, not the page** — unlike every other
Pad/Granular-style screen, `Screen::Mixer` has no sub-pages of its own;
the encoder's usual "cycle this screen's pages" job is repurposed to
step through one continuous, endlessly-wrapping sequence of 8 channel
stops plus a final Scope stop (a live oscilloscope of the actual
post-fader master mix, captured in `main.cpp` right after the master
filter/click/master-volume stage — the real final signal, not any one
instrument's own output), then wraps back to channel 0. This was a
deliberate design constraint, not an oversight: encoder rotate means
"next page" on every other screen in this project with zero exceptions,
so when Mixer needed a way to move between 8 channels *and* had no
"next page" job of its own left to give the encoder, repurposing rotate
for exactly this one screen was the least-surprising option, rather than
inventing a second, competing "rotate" gesture.

**Each of the 7 non-Master channels** shows a Detail page with real
Volume/Pan/Reverb-Send vertical bars: Button 1 tap maps the knobs to
Volume+Pan, Button 2 tap maps them to Reverb Send — the same
Button-1/Button-2-toggle-which-pair idiom already used elsewhere (Pad's
merged ADSR page, Grains' merged Grain/ADSR/Mix pages). **Master** (the
8th channel) has no toggle — Knob 1 is Volume, Knob 2 is Reverb Size,
always, since there's no per-channel Pan/Send concept for the master bus
itself.

**A genuine knob-pickup exception, and why**: `KnobContext::MixerVolPan`/
`MixerReverb` are each shared across all 7 non-Master channels (branching
internally on `mixer_position_`, the same "one context, branch on which
target" idiom `KnobContext::LayerStatus` already uses for
`cursor_layer_`). Unlike `LayerStatus`, though, `mixer_position_` can
change *while this exact KnobContext value stays the same* (rotating
between two Volume+Pan channels doesn't change which enum value
`CurrentKnobContext()` returns) — so the automatic "re-arm pickup on
context change" logic every other rotate-driven index in this project
relies on never fires here on its own. `HandleEncoder()`'s own rotate
handling explicitly re-runs the pickup reset/reseed by hand on every
channel change as a deliberate, documented exception to that otherwise-
universal rule.

## SD card management (Global:SdMgmt)

Lets you browse any of the 4 save categories — Performances, Pad
Presets, Fm Presets, Grains Presets — and Duplicate or Delete individual
files directly from the Pod, no computer needed. Reuses the exact same
cached slot-list arrays and dirty-flag/`Refresh*()` functions the 4 save
pages already scan and cache — no duplicate directory-scanning logic
anywhere.

- **Duplicate** (Button 1 held 800ms): byte-for-byte chunked copy into
  the next free slot in that category (`CopyFileChunked()`, shared by
  all 4 categories) — `FA_CREATE_NEW`, not `FA_CREATE_ALWAYS`, so an
  unexpected filename collision fails loudly instead of silently
  overwriting something; any failure partway through deletes the partial
  destination file rather than leaving a corrupt copy behind.
- **Delete** (Button 2 held `kSdMgmtDeleteHoldMs` = 1500ms — longer than
  every other hold-to-confirm gesture in this project, since unlike
  Overwrite/New (which can just be re-saved/re-loaded), delete has no
  undo): removes the file, then **closes the gap it leaves behind** —
  every higher-numbered file still on the card in that same category
  (and any other pre-existing gaps above it) shifts down to the lowest
  free number below it, converging back to contiguous numbering from 1
  (or from the first user slot, for Pad/Fm Presets' factory-reserved
  range) regardless of deletion order. Without this, `NextFree*Slot()`'s
  own "lowest free slot" scan would silently reuse the low number a
  delete just freed on the very next Save New, while other, higher-
  numbered saves sat stranded above it with a permanent gap in between.
  If the file that was deleted (or one that got shifted) happens to be
  the one currently loaded on its own save/load page, that page's own
  "Now: N" tracking is updated in the same pass to follow it to its new
  slot (or to "unsaved"/"custom" if it was the one actually deleted) —
  passed through as an in/out pointer to `DeleteSlot()`/
  `DeletePadPreset()`/`DeleteFmPreset()`/`DeleteGranularPreset()` rather
  than guessed at from the caller side, since only the delete function
  itself knows exactly how far each surviving file actually moved.

## The Files/New Load chooser

Save and Load use a deliberately identical two-step interaction on all 4
save pages (Global:File, Pad Preset, Fm Preset, Grains Preset), so the
convention never needs re-learning between them:

- **Save** (Button 1 tap from idle): reveals a two-line choice —
  Overwrite (only offered once something's actually loaded) vs. Save
  New — picked with Knob 1, confirmed with Button 2 held 800ms.
- **Load** (Button 2 tap from idle): reveals the *same shape* of
  two-line choice — **Files** vs. **Load New** — picked with Knob 1.
  Selecting Files and tapping Button 1 drills in: the chooser is
  replaced by the actual numbered file list (or, for Fm Preset only, a
  folder list one level above that — see *Preset folder browsing* under
  *Fm Synth* above), and Knob 1 now scrolls it directly. Button 1 is
  "Back" everywhere else inside Load (drilled-in → back to the chooser
  or folder list; chooser-with-New-highlighted → back to idle). Button 2
  held 800ms confirms whichever is actually selected — a specific
  browsed file, or New — and does nothing yet at the chooser with Files
  highlighted (or, for Fm, at its folder list), since there's no
  specific target picked at that level ("nothing if it does nothing",
  the same footer-label rule this project already applies everywhere
  else). Fm's own Preset page additionally lets a *short* Button 2 tap
  preview the highlighted file live without leaving the browser (see
  *Live preview while browsing* under *Fm Synth* above) — the one real
  difference from this otherwise-identical convention.
- **New**, reached this way instead of a separate hidden gesture, means:
  Global:File clears every layer's recorded audio (keeping every
  setting); Pad Preset and Fm Preset each reset to their own first
  factory patch; Grains Preset clears the captured audio and resets
  every parameter to the engine's own defaults — each the same "genuine
  fresh start" as the category's own factory-default state.

Both this chooser and Overwrite/Save New share the same forcing rule
when the "other" option isn't valid: Overwrite forces Save New when
nothing's loaded; the Load chooser forces New when there's nothing to
browse (an empty save list) — Knob 1 simply can't land on a choice that
doesn't exist.

## Save/load

`PerformanceStore` saves/loads a whole performance (all 4 layers' audio
plus tempo/global/per-layer settings) as one flat binary file per slot,
`PERF/PERF001.DAT` .. `PERF/PERF099.DAT`, via Global:File — kept in its
own subfolder for the same reason WAV exports get their own (see below):
a tidier SD root, and it keeps `ListSlots()`/`NextFreeSlot()`'s directory
scan scoped to just that folder. The on-disk layout has a
version tag (`kFileVersion` in `performance_store.cpp`, currently **10**)
that gets bumped whenever a field is added or removed — a save from an
older firmware version is rejected cleanly on load (shown as a short
error on the File page) rather than being misread, so a firmware update
can mean older saves need re-saving under the new version. Pad Presets
(`PADP/PRESnnn.DAT`), Fm Presets (`FMP/PRESnnn.DAT`), and Grains Presets
(`GRNP/PRESnnn.DAT`, audio-inclusive — see *Grains* above) are each their
own separate on-disk format with their own independent numbering, not
part of a performance file's own version tag.

### Save-file "bubbles"

A performance deals **only** with the looper (audio + tempo/global/
per-layer settings) — it does not embed Pad, Fm, or Grains' sound at all,
even though it once did (`kFileVersion` 7 added Pad's embedded sound,
version 10 removed it). Each instrument's presets live entirely in their
own independent save/load system instead, with no cross-embedding into a
performance file — every instrument's saves stay in its own separate
"bubble," performances included. This was a deliberate architecture
change partway through Fm Synth's development, not a Fm-specific
decision: the same directive applies equally to Pad and Grains, both of
which had their own preset systems already but (in Pad's case) were
*also* getting silently duplicated into every performance save. The
practical effect: switching or tweaking any instrument's loaded sound
never touches a saved performance, and loading an old performance never
drags along whatever synth patch happened to be active when it was
saved — the two are fully decoupled. `PerformanceStore::Save()`/`Load()`
no longer take or return any Pad/Fm/Grains data at all as a result; each
engine's own `TriggerSave*Preset()`/`TriggerLoad*Preset()` pair is
completely separate and unaffected by performance Save/Load.

"New" (reached via the Load chooser's own "New" pick on Global:File —
see *The Files/New Load chooser* above) only wipes each layer's recorded
*audio* (`LooperLayer::Clear()`) — every global and per-layer setting is
left exactly as it was. It's deliberately not the same as applying the
startup default (see below); those are two different, independent
actions. Delete/Duplicate for all 4 save categories live on
`Global:SdMgmt` instead (see *SD card management* above), not on these
individual save/load pages.

## Startup defaults

Global:Tempo's Button2, held 800ms (`Ui::TriggerSaveDefaults()`), saves
the current *global* settings only — BPM, Bars, Master Volume, Metronome
on/off + volume, Master Filter mode/cutoff/resonance, Reverb Size, and
Bypass — as a single small `PREFS.DAT` in the SD root. Deliberately no
per-layer settings (volume/pan/filter/effect/reverb send) — those
stay whatever they were, same as every other per-layer control.

`PREFS.DAT` reuses `PerformanceStore`'s existing `FileHeader` struct
as-is (same fields a full performance save already has, just written
without any layers/audio following it) rather than a second parallel
format, distinguished only by its own magic (`"PREF"` vs `"OURO"`) so it
can never be cross-loaded with a real performance save by mistake.

Applied once, automatically, at boot (`Ui::ApplyStartupDefaults()`,
called from `main()` right after `Ui::Init()`) — if no `PREFS.DAT`
exists yet (fresh SD card, or nothing's been saved), this silently
no-ops and the firmware's own hardcoded defaults stand; not an error
worth surfacing anywhere. It's *not* applied when starting a New
performance from Global:File — "New" there keeps every current setting
exactly as-is (see Save/load below), the startup default only ever
matters at power-on.

## WAV export

`PerformanceStore::ExportWav()` renders the current in-memory performance
to a standard stereo 16-bit PCM WAV file on the SD card. Two independent
output modes, both on Global:Export, both kept out of `PERF/` so they're
invisible to `ListSlots()`/`NextFreeSlot()`:

- **Button 1 ("Studio")** — full-quality native 48kHz (the Pod's actual
  audio rate), written to `WAV/EXPnnn.wav`. General purpose.
- **Button 2 ("CD")** — the same render, but resampled to 44100 Hz and
  written to `custom/EXPnnn.wav` instead. That folder name is required,
  not cosmetic: it's what a **MicroDexed Touch** (a Teensy 4.1-based FM
  synth with its own second SD card slot) scans for user sample content,
  at its native 44.1kHz — so the same card can go straight from this
  looper into MicroDexed's second slot and be picked up immediately, no
  copying needed. This mode exists because a straight 48kHz file plays
  back audibly slow and pitched down on that device (confirmed on real
  hardware): 44100/48000 reduces to an exact **147/160** ratio, so
  `Resampler48to44_1` (in `performance_store.cpp`, right after
  `kMaxExportLayers`) tracks phase with plain integer arithmetic instead
  of a float accumulator — zero long-term drift no matter how long the
  loop is, and the exact output frame count for a given input frame
  count (needed for the WAV header's `data_size`, written before the
  render loop even starts) is a closed-form calculation
  (`Resampler48to44_1::OutputFrames()`), not something requiring a dry
  run first. The DSP chain itself — master filter, shared reverb — always
  keeps running at the true native 48kHz regardless of mode; only the
  final quantized output samples are resampled, right after the master
  filter and before the makeup-gain/16-bit conversion step. Each mode has
  its own independent `EXPnnn` numbering sequence.

Unlike Save, neither mode ever overwrites anything (always the next free
number in its own folder), so there's no hold-to-confirm gesture on
either button.

- **Exactly one full loop length** (`TempoClock::GetLoopLengthSamples()`),
  always starting from the true downbeat regardless of where playback
  happened to be when Export was triggered — every layer's `play_pos_` is
  snapshotted, forced to 0 for the render, then restored afterward (even
  if the render fails partway through).
- **The real, live effects chain** — filter, character effect, and
  reverb per layer, plus the master filter — not a dry sum. This means
  the export calls each layer's actual `Process()` an extra time from the
  main loop (with `g_audio_suspended` held for the whole operation, same
  as `TriggerLoad()`, so it doesn't race the real audio ISR touching the
  same objects). Reverb is handled the same way live playback does it: a
  local `ReverbSc` mirroring `main.cpp`'s shared bus, fed by every
  layer's Send-scaled signal summed together, rather than a separate
  instance per layer.
- **Master volume and the metronome click are excluded.** Master volume
  is a monitor/output-level control, not mix content — including it would
  mean the exported file's loudness depended on wherever that knob
  happened to be sitting, including all the way down. The master filter
  *is* included (it's a real mix-shaping tool), via a fresh local `Svf`
  rather than main.cpp's live one, since a filter's only state is
  short-term signal history — primed with a throwaway first pass over the
  loop before the real render so it isn't starting cold. Project
  vari-speed is the one live-performance control treated the *opposite*
  way from master volume: it's deliberately baked into the export as-is
  (see *Vari-speed and scrub* above) rather than excluded.
- **Peak-normalized, not just clamped.** That same throwaway first pass
  doubles as a peak scan (it computes the exact same signal the real pass
  will write, so this is free); the real pass then applies a flat makeup
  gain so the loudest sample in the loop lands just under full scale,
  capped so near-silent content doesn't get boosted into audible noise.
  Without this, a loop that never got near clipping during normal
  playback would export using only a fraction of the 16-bit range and
  sound noticeably quiet no matter how loud it's played back.
- **Refuses to run** while any layer is `Recording` or `ArmedCountIn`
  (the Recording write path only checks `state_`, not real input, so
  running it against silent dummy input would overwrite an in-progress
  take), or if nothing's been recorded yet.

## Known limitations & assumptions

A few things that are intentional, not bugs:

- **No input routing selector** — see *Input front-end* above; per-layer
  Gain is the actual fix for a quiet source.
- **Bypass sums to mono**: the dry monitor mix (Home, Button 2) sums both
  input channels together and sends that to both outputs, rather than
  keeping them independent — the Pod has one physical stereo-TRS input
  jack, and a plain mono instrument cable into it only excites one ADC
  channel (the other's ring contact is unconnected), so a straight
  per-channel passthrough left the signal audible on only one side.
  Recording still captures each ADC channel independently, so a
  genuinely stereo source still records in true stereo.
- **No tempo/pitch-independent time-stretching**: changing BPM never
  retroactively affects already-recorded audio.
- **Waveform display is a downsampled cache, not the raw buffer**: each
  layer keeps 63 peak values (one per display column), updated
  incrementally per-sample while recording/overdubbing rather than
  rescanned from the (up to ~33s) audio buffer on every redraw. Peaks
  only ever rise within a take (max-hold), never fall, even if an
  overdub happens to reduce net amplitude somewhere — a deliberate
  simplification, not a bug.
- **Save files are version-locked**: see *Save/load* above.
- **Export is one stereo mixdown, not per-track stems**: all 4 layers'
  post-effects signal is summed into a single file (either export mode)
  — there's no way to export each layer as its own file.
- **No per-engine MIDI routing yet**: NoteOn/NoteOff (and, for Pad/Fm,
  pitch bend and mod wheel) drive every enabled engine from the same
  incoming MIDI stream (see *MIDI* above) — there's no way to play just
  Grains from a keyboard without it also reaching whichever of Pad/Fm
  is currently enabled.
- **Bypass's mix Volume/Pan don't persist**: like Master Volume and
  vari-speed, they're live-performance controls that always reset to
  their defaults on boot and after Load — not part of a saved
  performance, and not covered by Startup Defaults either.
- **No SD card hot-swapping while powered on**: the SD socket is
  hand-wired with no card-detect pin (see *Hardware* above), so the
  firmware has no way to know a card was physically pulled and
  reinserted while the Pod stays on — `PerformanceStore::Remount()`
  (triggered automatically on Home→Global, and manually via the "Retry"
  button when Global:File/Export show "No SD card") re-runs `f_mount()`,
  which recovers a merely-stale mount, but not a card that was actually
  swapped live: the low-level SDMMC peripheral can be left in a state a
  plain re-`Init()` doesn't cleanly recover from (a proper fix needs a
  full `HAL_SD_DeInit()`/re-`Init()` cycle, which broke the *boot-time*
  mount when tried and was reverted rather than risk it further without
  more careful testing). Power off before swapping cards, then power
  back on — every boot-time mount in testing has been reliable.

## Files

- `main.cpp` — hardware init, audio callback, main loop, MIDI polling
- `looper_layer.h/.cpp` — per-layer state machine, filter/effects/
  reverb, waveform cache, audio
- `tempo_clock.h/.cpp` — BPM/bars/metronome/count-in engine
- `pad_synth.h/.cpp` — the Pad Synth instrument (see *Pad Synth* above)
- `fast_fm_voice.h` — the lookup-table 4-operator FM oscillator core (see
  *Fm Synth* above) — no `.cpp`, everything's inline/force-inlined for
  ITCM residency
- `fm_synth.h/.cpp` — the Fm Synth instrument (see *Fm Synth* above)
- `granular_engine.h/.cpp` — the Grains instrument (see *Grains* above)
- `ui.h/.cpp` — encoder/button/knob handling + OLED menu rendering, for
  every screen (Home/Layer/Global/Fm/Pad/Granular/Mixer)
- `font_tomthumb.h/.cpp` — the tiny proportional font used in the
  footer rows (see `README.md`'s Thanks section for credit)
- `performance_store.h/.cpp` — SD card save/load (performances, Pad
  Presets, Fm Presets, Grains Presets), WAV export, and SD Mgmt's
  Duplicate/Delete. Performance on-disk format is at `kFileVersion = 10`
  (see *Save/load* above; bumped from 5→6 when Pitch's fields were
  removed — see *Pitch removal* below — up through Pad Synth's sound
  being embedded at version 7, then removed again at version 10 once the
  save-file "bubbles" principle was adopted — see *Save-file "bubbles"*
  above).
- `audio_engine.h` — `g_audio_suspended`, a flag that makes the audio
  callback output silence without touching any layer/tempo state, so
  `PerformanceStore::Load()` (which restores layers one at a time,
  streaming each from SD) can't leave layers starting at different
  sample offsets just because the audio ISR kept running mid-restore
- `itcm.h` — `DSY_ITCM_TEXT`, a project-local macro placing tagged
  functions in ITCM RAM — see *Boot process* below
- `STM32H750IB_qspi_custom.lds` — this project's linker script, a copy of
  libDaisy's own QSPI script plus the `.itcm_text` output section the
  macro above needs
- `Makefile` — source list for the build, `APP_TYPE`/`LDSCRIPT` overrides
  for the bootloader/QSPI build (see below)

## Boot process: bootloader + QSPI flash

Internal FLASH (128KB) was nearly full, with no room for future audio
features. As of v1.6.0 this firmware instead boots via the separate Daisy
bootloader and executes directly from the Pod's 8MB external QSPI flash
chip (`APP_TYPE = BOOT_QSPI` in `src/Makefile`) — genuine execute-in-place,
not a copy-to-RAM step. The bootloader itself (a prebuilt binary,
`libDaisy/core/dsy_bootloader_v6_4-intdfu-2000ms.bin`, installed once via
`make program-boot`) runs first on every power-on, checks briefly for a
pending update (a `.bin` on the SD card, or a short USB-DFU window), and
otherwise boots straight into the QSPI-resident app — no button-holding
for normal use, just a slightly longer boot. See `README.md`'s *Building
and flashing* for the practical steps, including the SD-card auto-flash
method (copy `main.bin` to the card's root, power-cycle — sidesteps the
bootloader's brief ~2 second DFU window entirely).

**Why this needed real measurement, not just flipping the flag**: QSPI
execute-in-place is more cache-dependent than internal flash (Electrosmith's
own stated caveat). Measured on real hardware with a DWT cycle counter at
this firmware's worst-case DSP load (all 4 layers active, a character
effect on one, shared reverb running), plain `BOOT_QSPI` raised the audio
callback's worst-case timing from under 50% of budget (the original
internal-flash build) to 69%. Rather than accept that regression, the
real-time call graph (`AudioCallback()`, `LooperLayer::Process()`,
`TempoClock::Process()`/`RenderClick()`, and the DSP `Process()` methods in
the vendored `DaisySP`/`DaisySP-LGPL` submodules) is hand-placed in
**ITCM** (a 64KB zero-contention instruction RAM every Daisy linker script
declares but none use) via the `DSY_ITCM_TEXT` macro (`itcm.h`) and a
custom `.itcm_text` section in `STM32H750IB_qspi_custom.lds`. This
recovered most, but not all, of the regression (64% worst-case) — the
remaining gap traced to `PitchShifter`'s own internal `DelayLine`/`Phasor`
helper calls, which weren't (and, being template/inline code, couldn't
cleanly be) pulled into the same ITCM placement.

Since the Pitch feature was removed for unrelated reasons (see below) and
took its cost with it, worst-case CPU under `BOOT_QSPI` + ITCM measured
**45%** for the 4-layer looper alone — better than the original
internal-flash baseline, with the QSPI chip's ~8MB now available for
future features (the original motivation for this migration).

That 45% predates Pad Synth and Grains; with both instruments added
(and voice counts tuned against real measurements rather than paper
estimates — see *Pad Synth*'s own CPU note above, including the
`StringVoice` architecture that was measured and ruled out), the
**genuine worst case with everything running at once** — full 4-layer
loop + reverb, 6-voice Pad Synth, and Grains, all simultaneously active
— measures **~94%**. The per-engine on/off toggles (see *Per-engine
on/off* above) are a real, immediate mitigation if a given performance's
actual worst case turns out tighter than that in practice.

Fm Synth (mutually exclusive with Pad — see *Per-engine on/off* above)
has its own separate worst-case story, told in full under *Fm Synth*'s
own CPU-measurement writeup above: two real ITCM/QSPI code-placement
bugs were found and fixed there (the same *class* of regression as
`PitchShifter`'s own gap just above — inline/header-defined functions
silently landing in slow QSPI instead of ITCM once they grew past
whatever threshold the compiler was inlining them below), plus a voice
count trade from 8 down to 6. With those fixes in, real hardware
measurement showed **~52-57% Fm-alone** (idle vs. actively playing) and
**~63% with 3 loop layers actively playing back alongside it** —
confirmed on hardware as "much better" after being genuinely sluggish
before the fix.

**A genuinely new linker-level detail worth knowing if extending this**:
the custom `.itcm_text` section has a load address (LMA) in QSPI flash but
a run address (VMA) in ITCM RAM, the same relationship `.data` has between
QSPI and SRAM — but the stock startup code's copy-down loop only knows
about `.data`'s symbols, not this new section. `main()` does its own
`memcpy()` from linker-provided `_sitcm_text`/`_eitcm_text`/`_siitcm_text`
symbols, as the very first statement, before anything ITCM-tagged can
possibly run.

## Pitch removal

The per-layer Pitch effect (±12 semitones, DaisySP's `PitchShifter`) was
removed in v1.6.0. It turned out to be the single most expensive effect in
the signal chain (see above) for a creative use case vari-speed already
covers more cheaply and, in most feedback, more usefully — tape-style
pitch/tempo change across the whole performance rather than a per-layer
delay-line shift with its own audible latency and warble trade-offs (the
old Fast/Med/Smooth delay presets existed specifically to manage that
latency). Removing it recovered real CPU headroom (see above) without
losing a capability nothing was actively relying on. `kFileVersion` bumped
5 → 6 for `LayerHeader`'s dropped fields — a save from v1.5.0 or earlier
fails to load cleanly under v1.6.0+ (shown as a version-mismatch error on
the File page) rather than being misread.
