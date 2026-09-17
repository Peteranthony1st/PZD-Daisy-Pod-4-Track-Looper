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

**Dexed** (see its own section below) is built around **msfa**, Google's
own real DX7 emulation core, released under the Apache License 2.0 —
vendored unmodified into `src/msfa/` except two small additive
introspection helpers. Its 961 factory presets are real, freely-
distributed SysEx bank dumps drawn from the broader Dexed/MicroDexed
open-source ecosystem that msfa's own lineage traces through, not one
single traceable upstream repo.

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
  On `Global:Dexed`/`Global:Granular`/`Global:Mixer`
  specifically, it's instead this Global page's own entry point into a
  real top-level screen (`Screen::Dexed`/`Screen::Granular`/
  `Screen::Mixer`) — the same encoder-click gesture, just repurposed
  per-page since Home's own drill-in doesn't apply outside Home. On
  `Screen::Dexed`/`Screen::Granular` themselves (and every
  other Global page), a click mutes/unmutes all 4 loop layers
  (`Ui::TogglePauseAll()`) — there's no drill-in job left for it on those
  screens, so it's free for the same mute Global pages already use.
  Unused on `Screen::Mixer` (rotate already picks the stop).
- **Long-press** the encoder (~600ms): from Home, opens Global settings;
  from anywhere else — including `Screen::Dexed`/
  `Screen::Granular`/`Screen::Mixer` — goes back to Home.

```
Home ──(click layer)──► Layer[n] ──(rotate)──► Status / Speed / Filter /
  │                                             Effect / Reverb / Gain
  │                                                     (long-press ⤴ back to Home)
  └──(long-press)──► Global ──(rotate)──► Tempo / Filter / Reverb / File /
       │                                    SdMgmt / Export / Dexed /
       │                                    Granular / Looper / Mixer
       │                                       (long-press ⤴ back to Home)
       ├──(click on Global:Dexed)──► Screen::Dexed ──(rotate)──► Algo / Feedback /
       │                                                   Vibrato / Brightness /
       │                                                   EnvSpeed / Filter /
       │                                                   Mix / Preset
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

Dexed and Grains are fully independent (see *Per-engine on/off* below) —
each has its own Global page, full screen, and Mixer channel, and either
can be switched on/off without affecting the other.

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
| Global: Dexed | — | — | Toggle Dexed on/off | — |
| Global: Granular | — | — | Toggle Grains on/off | — |
| Global: Looper | — | — | Toggle the whole 4-layer loop system on/off (also resets `TempoClock`'s phase) | — |
| Global: Mixer | — (entry point + at-a-glance overview grid, see *Screen::Mixer* below) | — | — | — |
| Dexed: Algo | Cycle algorithm (quantized bucket selection, all 32) | — | Cycle algorithm back | Cycle algorithm forward |
| Dexed: Feedback | Feedback amount (0-7) | — | — | — |
| Dexed: Vibrato | LFO speed | LFO pitch-mod depth | — | — |
| Dexed: Brightness | Modulator level scale (0x..2x, center = as saved) | — | — | — |
| Dexed: EnvSpeed | All-operator envelope rate scale (0x..2x, center = as saved) | — | — | — |
| Dexed: Filter | Cutoff | Resonance | Cycle filter mode | — |
| Dexed: Mix | Reverb send | Output level | — | — |
| Dexed: Preset | Folder list: scroll folders. Inside a folder: scroll presets | — | Save / Open folder (or Select at top chooser) / Back | Tap = preview live (stays in browser); Hold 800ms = confirm + exit |
| Granular: Grain | Size (or Gap) | Fill (or Scan) | Knobs → Size+Fill | Knobs → Gap+Scan |
| Granular: Position | Position (Grain layer's fixed anchor) | — | — | — |
| Granular: TuneDirection | Tune (grain pitch, ±24 semitones) | Direction (Forward/Reverse/Random) | Toggle Map-to-Note | — |
| Granular: ADSR | Attack (or Sustain) | Decay (or Release) | Knobs → Attack+Decay | Knobs → Sustain+Release |
| Granular: Filter | Cutoff | Resonance | Cycle filter mode | — |
| Granular: Mix | Grain volume (or Reverb send) | Scan volume | Knobs → Grain+Scan volume | Knobs → Reverb send |
| Granular: Capture | — (Import mode: browse .wav files) | — | Cycle source: Direct Record → Layer 1..N → Import | Hold = record (Direct) or capture/import (Layer/Import) |
| Granular: Trim | Trim start | Trim end | — | — |
| Granular: Preset | Same Files/New chooser and browse-list convention as Global:File | — | Same Save/Load/Back convention as Global:File | Same hold-to-confirm convention as Global:File (live progress bar — audio-inclusive) |
| Mixer: Layer/Grains/Bypass | Volume | Pan | Knobs → Volume+Pan | Knobs → Reverb send |
| Mixer: DXD (Dexed) | Volume | — (no Pan control yet) | Knobs → Volume (no Pan to toggle) | Knobs → Reverb send |
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

Two MIDI-played instruments — Dexed and Grains, both described below —
run alongside the looper, driven by the Pod's built-in MIDI IN jack
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

- **NoteOn/NoteOff** currently drive **both** Dexed and Grains from
  the same incoming stream — there's no per-engine MIDI channel or
  screen-based routing yet (flagged in `main.cpp` as a temporary,
  first-pass simplification). Playing a note plays it on whichever
  engines are enabled (see *Per-engine on/off* below).
- **Pitch bend** (±2 semitones, the MIDI standard default) feeds Dexed
  — Grains has no pitch bend input.
- **Mod wheel** (CC1) feeds Dexed's own vibrato (real DX7 LFO pitch-mod
  depth, gated by the wheel same as real DX7 hardware) — Grains has no
  mod wheel input.

## Per-engine on/off

`Global:Dexed`/`Global:Granular`/`Global:Looper` each carry a plain
Button-1-tap on/off toggle (`dexed_enabled_`/`granular_enabled_`/
`looper_enabled_`) — a real CPU lever, not just a mute:
`AudioCallback()` skips calling that engine's `Process()` entirely while
disabled and writes silence instead, so someone using only Grains (say)
genuinely gets Dexed's own CPU share back, not just its silence.
Switching the looper off also resets `TempoClock`'s phase (bar/beat
position, count-in state, click envelope) back to bar 1 beat 1, so
turning it back on always resumes cleanly rather than picking up
mid-phase from wherever it happened to be frozen. Both default **off** on
boot — a real toggle exists for each, so there's no reason for either to
be making sound before it's asked to.

Dexed and Grains are fully independent — no mutex, unlike the earlier
Pad Synth/Fm Synth pair they replaced (which shared one "melodic voice"
role and one Mixer channel, and were kept mutually exclusive as a
result). Dexed's own real 6-operator DX7 emulation made both of those
from-scratch approximations redundant at once rather than needing a
third alongside them, so there's no remaining case where two competing
melodic engines need to be kept from overlapping.

## Dexed (Screen::Dexed)

A MIDI-played, real 6-operator, 10-voice DX7 clone (`dexed_synth.h/.cpp`)
built around a vendored copy of Google's own **msfa** (`src/msfa/`,
Apache-2.0 — the same DX7 emulation core Google's official open-source
`dexed`-lineage synths trace back to), replacing both Pad Synth and Fm
Synth outright rather than sitting alongside either — a real DX7 clone
makes both of those earlier from-scratch approximations redundant at
once, and unlike them, Dexed and Grains are fully independent (no mutex
— Dexed's own on/off toggle doesn't touch Grains' or vice versa).

**Why msfa, not another from-scratch FM voice**: after building and
tuning two increasingly-elaborate custom FM engines in turn (Pad Synth's
oscillator-bank approach, then Fm Synth's own lookup-table 4-operator
voice — see project history for both), the real DX7's own operator
algorithms, envelope behavior, and parameter ranges turned out to be
worth having exactly right rather than approximated again a third time.
msfa is the genuine article: the same 32 real algorithms, the same
per-operator envelope/keyboard-scaling/velocity-sensitivity model, real
patch data ported directly from real DX7 SysEx dumps rather than
hand-guessed. No GPL'd decoder/UI code was ported alongside it — msfa's
own `synth.h` deliberately excludes the packed-SysEx conversion and
patch-editor logic that lived in the original (GPLv3) Dexed plugin, so
everything UI/preset/SysEx-adjacent in this project (`dexed_sysex.h/
.cpp`'s packed↔unpacked converter, the whole preset/macro/UI layer
below) is this project's own, written directly from the public 1993
Yamaha MIDI SysEx spec.

**Voice architecture**: `kMaxVoices = 10`, oldest-note-steal (matching
every other engine's own voice-stealing convention), each backed by
msfa's own `Dx7Note`. `patch_[156]` (msfa's unpacked patch layout) is
shared by every voice — there's no per-voice patch copy — so switching
or previewing a preset is a single write that every currently-held note
picks up on its next `update()` call, not a per-voice re-copy.

**A real cross-ISR race condition, found from real-hardware note
dropouts during fast playing**: `NoteOn()`/`NoteOff()` are called from
`ControlTimerCallback` (TIM5, a genuinely lower NVIC priority than the
audio DMA ISR), and perform real multi-step, non-atomic mutations to a
voice's `Dx7Note` state (`init()`/`keyup()`). `RenderQuantum()`, running
from the higher-priority audio ISR, reads that same voice state every 64-
sample quantum and can genuinely preempt a `NoteOn()`/`NoteOff()` call
mid-mutation — dropping or corrupting a note when playing fast enough
for the two to actually collide. Fixed with tight `__disable_irq()`/
`__enable_irq()` critical sections around just the actual voice-mutating
calls (not the surrounding voice-search logic) in `NoteOn()`, `NoteOff()`,
and `ApplyPatchToHeldVoices()` (also reachable from the equally-
preemptable main-loop context via the UI's own knob-driven setters).

**A follow-on regression from that same fix**: `Ui::KnobPickUp()` has no
"did this actually change" check of its own — once engaged, it
unconditionally re-applies the setter on every single main-loop tick,
harmless for every other engine's own cheap float-assignment setters, but
this meant `ApplyPatchToHeldVoices()`'s new critical section was firing
at the full main-loop tick rate for as long as any Dexed knob stayed
engaged, and with several notes held, the cumulative disabled-interrupt
time was enough to cause fresh dropouts. Fixed with explicit "value
actually changed" guards in `Ui::ApplyKnobs()` specifically for Dexed's
expensive setters (Feedback/Vibrato compare the resolved byte value;
Brightness/EnvSpeed use a small float epsilon) — Filter/Mix's own
setters are left unguarded, since those are cheap plain-float
assignments like every other engine's.

**Output level, headroom, and the post-mix filter**: `Process()` reads
msfa's own Q24 fixed-point render output (`kQ24Scale = 1/(1<<24)`), then
an empirically-measured `kHeadroomScale` correction — a single hand-tuned
worst-case test patch first suggested `1/4.41`, but real hardware testing
with actual factory patches (several using feedback, and real chords
beyond 3 notes) showed that margin was still too tight, engaging the
final `tanhf()` soft limiter hard enough to audibly compress even a
single patch played alone; doubled to `1/8.82`, confirmed clean up to
full master volume. A `daisysp::Svf` pair (mirroring `GranularEngine`'s
own bus-filter shape) sits post-mix, pre-limiter, so a resonant peak is
still caught by the safety net rather than clipping past it. Dexed has
no Pan control yet (see *Screen::Mixer* below for what that means for
its own Mixer channel) — a later increment.

**Macros — Brightness and Envelope Speed**: both bipolar, knob-center
(0.5) = "exactly as the loaded preset stored it," scaling relative to a
`patch_baseline_[156]` snapshot taken whenever a preset actually loads
(not relative to whatever the knob did last, so repeatedly nudging
Brightness back to center always returns to the *preset's own* sound,
not some drifted intermediate state). Both use
`FmCore::get_operator_routing()`/`get_carrier_operators()` to identify
which operators to scale generically for whichever of the 32 algorithms
is loaded, rather than 32 hand-authored per-algorithm tables:
- **Brightness** scales every non-carrier (modulator) operator's output-
  level byte together, carriers untouched — 0x at the bottom of its
  range, unchanged at center, 2x at the top.
- **Envelope Speed** scales all 6 operators' 4 rate bytes together, same
  0x/1x/2x curve.

Both call `Dx7Note::update()` (never `init()`) on every currently-held
voice after writing the scaled bytes into `patch_` — genuinely
click-free, confirmed on real hardware: `update()`'s own `Env::update()`
doesn't reset the envelope's `level_` or restart the attack, it only
retargets from wherever the envelope already is.

**A real, previously-latent bug found while building the Algo page's
diagram**: `FmCore::get_carrier_operators()` (msfa/fm_core.cpp) checked
only the `OUT_BUS_ADD` flag bit, which actually means "sum into my
target rather than overwrite it" — a flag also set on plain modulators
that sum into a shared bus (used by several converging algorithms, e.g.
16-18) — not "carrier" specifically. Decoding all 32 real algorithms
directly confirmed this over-reported carriers on 15 of the 32. Fixed to
check `outbus == 0` (writes straight to the final mix, exactly what
`render()` itself checks), which is what the Brightness macro above and
the Algo diagram below both now rely on.

**Algorithm diagram (`DexedParamPage::Algo`)**: a real, generic box-and-
arrow diagram of whichever of the 32 real algorithms is loaded, laid out
fresh from `FmCore::get_operator_routing()`'s per-operator bus data —
not 32 hand-drawn pictures. The routing graph (which operator feeds
which bus, and in what order, exactly matching msfa's own sequential
`render()` processing) is reconstructed into a column (independent
chain)/row (depth from that chain's root) layout; a parent feeding more
than one child branches the later child(ren) into a fresh column instead
of overlapping the first; a converging algorithm (multiple parents into
one child) centers that child between its parents' columns. Every
connector is strictly horizontal/vertical (a straight drop for a same-
column parent, an elbow — down to the child's own row height, then
sideways into the middle of its near side edge — for a cross-column one)
by explicit design constraint, not a limitation: it keeps the diagram
legible on a 128x64 monochrome display without needing a curve renderer.
Filled boxes are carriers, outline boxes are modulators (the operator
number punched through as "off" pixels on a filled box so it still
reads); a horizontal "OUT" bus line collects every carrier's drop; the
one operator (if any) with real self-feedback — both `FB_IN` and
`FB_OUT` set, confirmed from the routing data to always be a single
self-loop on one chain-root operator, never split across two — gets an
explicit "FB" text tag beside it (a small pixel bracket was tried first
and read poorly at this box size; feedback operators are always chain
roots with no incoming edges, so their side edges are always free for
this). Algorithm can be cycled three ways: Knob 1 (quantized bucket
selection across all 32, soft pickup), Button 1 (step back), Button 2
(step forward).

**Factory presets — 961 across 15 folders, real sourced data, never
invented**: real, freely-distributed SysEx bank dumps drawn from the
broader Dexed/MicroDexed open-source ecosystem this whole port's msfa
lineage already traces through (not one single traceable upstream
repo), each a real, standard 32-voice SysEx bulk-dump bank. 11 folders organized by real
sound type (Synth/Piano/E.Piano/Bass/Strings/Woodwind/Brass/Organ/
Perc/Choir/Bells — `kDexedFactoryCategories` in `dexed_factory_data.cpp`,
two source banks each, 64 voices, except Perc at 65 — see below), plus 4
more, **Rom 1** through **Rom 4**, each the real, unsorted 64-voice
contents of an actual Yamaha factory ROM cartridge pair (ROM1=1A+1B ...
ROM4=4A+4B) kept in their own original folders rather than re-sorted by
sound type. Real quality/popularity rankings for DX7 patches aren't
something that can be sourced reliably, so the ROM folders stand in as
an objective proxy instead — genuine factory data every real DX7 shipped
with (ROM1) or that Yamaha sold as official cartridges (ROM2-4). One
single voice (the classic ROM1A "MARIMBA") was hand-picked into the Perc
folder on top of its two source banks, after a user report that no
marimba sound existed anywhere in the set. Stored as raw 128-byte-per-
voice **packed** payloads (never pre-expanded and held in flash/RAM
unpacked) — `DexedSynth::GetFactoryPreset()` unpacks on demand via
`DexedSysex::UnpackVoice()`, verified byte-exact via a native (non-
embedded) round-trip test against real bank data before ever being
embedded.

**Duplicate factory names, and why some folders auto-number them**: real
factory banks occasionally have a long run of voices someone saved
without ever renaming from a generic default (the Bells folder's source
bank has ~30 voices literally all named bare "BELL") — indistinguishable
from each other while scrolling the preset browser, confirmed by a real
user report. `DexedSynth::GetFactoryPresetName()` disambiguates any name
that repeats anywhere within its own category with a running " N" suffix
(numbering every occurrence, including the first, so no two presets in
the same folder ever display identically) — a display-only fix; the
underlying patch data/sound is untouched.

**Preset data & save/load** (`DexedSynth::DexedPresetData`): patch bytes
plus `reverb_send01`/`output_level01`/filter mode+cutoff+resonance — a
flat snapshot via `ApplyPreset()`/`CapturePreset()`, the same shape every
other engine's own preset struct already uses. `output_level01` defaults
to **0.6** (not 1.0), and since factory presets never set this field
themselves, that's the level every one of the 961 factory presets
actually loads at. User presets save/load to `DEXP/PRESnnn.DAT` via
`PerformanceStore` (up to `kMaxDexedPresets = 1200` total slot numbers,
headroom above the 961 factory presets for up to ~239 of your own) — the
factory range is served straight from `GetFactoryPreset()` with zero card
I/O and, unlike the user range, is never subject to Delete/Duplicate's
own slot-renumbering (`CompactSlotsAfterDelete()` is fundamentally
incompatible with a fixed factory bank, the same reasoning Grains'
preset range already established).

**Preset folder browsing & live preview**: identical two-level
folder-then-preset browsing, and the identical "a short Button 2 tap
while a folder's open previews the highlighted preset live, without
resetting `save_load_mode_`" behavior, as the removed Fm Synth's own
preset browser — confirmed directly from that engine's own source
(`git show fm-synth-experiment:src/ui.cpp`) that live preview and commit
were always literally the same function call, just with or without also
exiting back to `Idle`; reused as-is here rather than redesigned.

**Filter/Mix**: one bus-level `Svf` pair (post-mix, pre-limiter, see
above) and Reverb Send/Output Level, both reachable identically from
Dexed's own Mix page and from `Screen::Mixer`'s DXD channel — same
underlying value either way.

**On by default? No** — `dexed_enabled_` defaults to **false**
(`Global:Dexed`'s own Button 1 toggle turns it on), matching Grains' own
default-off convention now that a real toggle exists — early in this
engine's development it defaulted on, back when no toggle existed yet to
turn it off.

**Recording Dexed (and Grains) into a loop layer**: `main.cpp`'s
`AudioCallback()` sums live input with Grains' and Dexed's own generated
audio into a `mixed_in_l/r` buffer, and each loop layer's `Process()`
call uses that mixed buffer instead of plain `in` only while that
specific layer is actually `Recording`/`Overdubbing`/`ArmedCountIn` —
layers just playing back always get plain `in`. This exact mechanism
existed before Pad/Fm's removal (as `mixed_in`, feeding Pad/Fm's own
output into recordings) and was deleted along with them under the
incorrect assumption that it existed only for those two instruments — it
also covered Grains (a real regression, since fixed) and needed
extending to cover Dexed too. Grains' own separate "Direct Record"
capture feature (its own dedicated live-input-into-Grains'-buffer path,
independent of the loop layers) was extended the same way, to also
include Dexed's generated audio (not Grains' own — that would be a
self-capture feedback loop).

**Grains' own output level, raised for the same underlying reason**:
real-hardware testing after the recording fix above surfaced that Grains
sounded much quieter than Dexed even at Grains' own Output Level knob
maxed — traced to two real, structural facts about `GranularEngine`'s own
gain chain that Dexed simply doesn't share: overlap-gain compensation
(dividing by however many grain slots are active, a deliberate,
documented anti-clipping tradeoff) and the shared linear center-pan law
(a real -6dB dip at center that Dexed currently skips entirely, having no
Pan control yet). `GranularEngine::SetOutputLevel01()`'s ceiling was
raised from 1.4x to 12x (in two real-hardware-driven steps, 4x first,
then further after that still measured too quiet) specifically to give
that knob enough real headroom to compensate — paired with a new
`tanhf()` soft limiter in `Process()` (same convention as Dexed's own)
so pushing the knob that hard drives it into deliberate, audible
saturation rather than harsh digital clipping.

## Grains (Screen::Granular / GranularEngine)

A MIDI-played, **monophonic** granular instrument (`granular_engine.h/
.cpp`) — deliberately much simpler than an earlier 8-voice granular
engine this project shelved on its own git branch: one held note at a
time, no oscillator layer (Dexed covers that role now), and two
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

**Filter/Mix/Reverb send/Pan/Output level**: one bus-level `Svf` pair
(same shape Dexed's own post-mix filter follows, see above), independent
Grain-layer and Scan-layer volumes summed before a single Output Level
stage — raised from a 1.4x to a 12x ceiling, plus a new `tanhf()` soft
limiter, after real-hardware testing showed Grains reading much quieter
than Dexed even maxed (see *Dexed*'s own note on this above for the full
story) — a continuous Reverb Send knob, and the same linear pan law
`LooperLayer` uses, applied to the already-stereo captured signal (Grains
reads real L/R from whatever was captured/imported, unlike a mono voice
sum).

**Presets** (`GranularEngine::GranularPresetData`): unlike Dexed,
Grains presets are **audio-inclusive** — a Grains preset IS a specific
captured sound plus how it's being played back, so saving/loading one
needs to restore both. `PerformanceStore::SaveGranularPreset()`/
`LoadGranularPreset()` stream the captured audio alongside the
parameters into ONE combined file per slot (`GRNP/PRESnnn.DAT`, up to 99
slots — no factory range, there's no hand-tuned-capture equivalent to a
hand-tuned patch), so this can take real SD-transfer time (up to ~1.9MB
of audio) and shows a live progress bar during the save/load, unlike
Dexed Presets' instant tiny-struct transfer. Output Level and Reverb Send are
deliberately excluded from the saved struct (same reasoning as Pan) —
session-level mixer settings, not part of the captured sound's own
identity.

## Screen::Mixer

One screen covering all 8 mixable channels — Loop Layers 1–4, Grains,
Dexed (DXD), Bypass, and Master — reached by clicking the encoder on
`Global:Mixer` (which itself shows a static at-a-glance overview grid:
all 8 channel names in a row, each with a real vertical Volume bar, no
numeric readout, via `DrawMixerOverviewGrid()`).

**Rotate picks the channel, not the page** — unlike every other
Dexed/Granular-style screen, `Screen::Mixer` has no sub-pages of its own;
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
Volume/Reverb-Send vertical bars: Button 1 tap maps the knobs to
Volume+Pan, Button 2 tap maps them to Reverb Send — the same
Button-1/Button-2-toggle-which-pair idiom already used elsewhere (Grains'
merged Grain/ADSR/Mix pages). **DXD (Dexed)** is the one exception among
those 7 — Dexed has no Pan control yet (see *Dexed* above), so its own
Pan bar/readout is simply omitted rather than showing a value that
wouldn't do anything; Volume and Reverb Send both still work normally.
**Master** (the 8th channel) has no toggle at all — Knob 1 is Volume,
Knob 2 is Reverb Size, always, since there's no per-channel Pan/Send
concept for the master bus itself.

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

Lets you browse any of the 3 save categories — Performances, Dexed
Presets, Grains Presets — and Duplicate or Delete individual
files directly from the Pod, no computer needed. Reuses the exact same
cached slot-list arrays and dirty-flag/`Refresh*()` functions the 3 save
pages already scan and cache — no duplicate directory-scanning logic
anywhere.

- **Duplicate** (Button 1 held 800ms): byte-for-byte chunked copy into
  the next free slot in that category (`CopyFileChunked()`, shared by
  all 3 categories) — `FA_CREATE_NEW`, not `FA_CREATE_ALWAYS`, so an
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
  (or from the first user slot, for Dexed Presets' factory-reserved
  range) regardless of deletion order. Without this, `NextFree*Slot()`'s
  own "lowest free slot" scan would silently reuse the low number a
  delete just freed on the very next Save New, while other, higher-
  numbered saves sat stranded above it with a permanent gap in between.
  If the file that was deleted (or one that got shifted) happens to be
  the one currently loaded on its own save/load page, that page's own
  "Now: N" tracking is updated in the same pass to follow it to its new
  slot (or to "unsaved"/"custom" if it was the one actually deleted) —
  passed through as an in/out pointer to `DeleteSlot()`/
  `DeleteDexedPreset()`/`DeleteGranularPreset()` rather
  than guessed at from the caller side, since only the delete function
  itself knows exactly how far each surviving file actually moved.

## The Files/New Load chooser

Save and Load use a deliberately identical two-step interaction on all 3
save pages (Global:File, Dexed Preset, Grains Preset), so the
convention never needs re-learning between them:

- **Save** (Button 1 tap from idle): reveals a two-line choice —
  Overwrite (only offered once something's actually loaded) vs. Save
  New — picked with Knob 1, confirmed with Button 2 held 800ms.
- **Load** (Button 2 tap from idle): reveals the *same shape* of
  two-line choice — **Files** vs. **Load New** — picked with Knob 1.
  Selecting Files and tapping Button 1 drills in: the chooser is
  replaced by the actual numbered file list (or, for Dexed Preset only, a
  folder list one level above that — see *Preset folder browsing & live
  preview* under *Dexed* above), and Knob 1 now scrolls it directly.
  Button 1 is "Back" everywhere else inside Load (drilled-in → back to
  the chooser or folder list; chooser-with-New-highlighted → back to
  idle). Button 2
  held 800ms confirms whichever is actually selected — a specific
  browsed file, or New — and does nothing yet at the chooser with Files
  highlighted (or, for Dexed, at its folder list), since there's no
  specific target picked at that level ("nothing if it does nothing",
  the same footer-label rule this project already applies everywhere
  else). Dexed's own Preset page additionally lets a *short* Button 2 tap
  preview the highlighted file live without leaving the browser (see
  *Preset folder browsing & live preview* under *Dexed* above) — the one
  real difference from this otherwise-identical convention.
- **New**, reached this way instead of a separate hidden gesture, means:
  Global:File clears every layer's recorded audio (keeping every
  setting); Dexed Preset resets to its own first factory patch; Grains
  Preset clears the captured audio and resets every parameter to the
  engine's own defaults — each the same "genuine fresh start" as the
  category's own factory-default state.

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
can mean older saves need re-saving under the new version. Dexed Presets
(`DEXP/PRESnnn.DAT`) and Grains Presets
(`GRNP/PRESnnn.DAT`, audio-inclusive — see *Grains* above) are each their
own separate on-disk format with their own independent numbering, not
part of a performance file's own version tag.

### Save-file "bubbles"

A performance deals **only** with the looper (audio + tempo/global/
per-layer settings) — it does not embed Dexed's or Grains' sound at all,
even though an earlier instrument (Pad Synth) once had its own sound
embedded this way (`kFileVersion` 7 added it, version 10 removed it
again). Each instrument's presets live entirely in their
own independent save/load system instead, with no cross-embedding into a
performance file — every instrument's saves stay in its own separate
"bubble," performances included. This was a deliberate architecture
change made partway through this project's history, not specific to any
one instrument: it applies equally to every instrument that's ever had
its own preset system. The
practical effect: switching or tweaking any instrument's loaded sound
never touches a saved performance, and loading an old performance never
drags along whatever synth patch happened to be active when it was
saved — the two are fully decoupled. `PerformanceStore::Save()`/`Load()`
no longer take or return any instrument data at all as a result; each
engine's own `TriggerSave*Preset()`/`TriggerLoad*Preset()` pair is
completely separate and unaffected by performance Save/Load.

"New" (reached via the Load chooser's own "New" pick on Global:File —
see *The Files/New Load chooser* above) only wipes each layer's recorded
*audio* (`LooperLayer::Clear()`) — every global and per-layer setting is
left exactly as it was. It's deliberately not the same as applying the
startup default (see below); those are two different, independent
actions. Delete/Duplicate for all 3 save categories live on
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
- **No per-engine MIDI routing yet**: NoteOn/NoteOff (and, for Dexed,
  pitch bend and mod wheel) drive every enabled engine from the same
  incoming MIDI stream (see *MIDI* above) — there's no way to play just
  Grains from a keyboard without it also reaching Dexed if it's enabled
  too.
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
- `msfa/` — the vendored real DX7 emulation core (Google, Apache-2.0),
  unmodified except two small, clearly-marked additive introspection
  helpers on `FmCore` (`get_carrier_operators()`'s outbus-based fix, and
  the new `get_operator_routing()` — see *Dexed* above)
- `dexed_synth.h/.cpp` — the Dexed instrument built around msfa (see
  *Dexed* above)
- `dexed_sysex.h/.cpp` — this project's own clean-room packed↔unpacked
  DX7 SysEx converter, written from the public 1993 Yamaha MIDI SysEx
  spec (see *Dexed* above for why msfa itself doesn't include one)
- `dexed_factory_data.h/.cpp` — the 961 embedded factory presets (see
  *Dexed* above), stored packed and unpacked on demand
- `granular_engine.h/.cpp` — the Grains instrument (see *Grains* above)
- `ui.h/.cpp` — encoder/button/knob handling + OLED menu rendering, for
  every screen (Home/Layer/Global/Dexed/Granular/Mixer)
- `font_tomthumb.h/.cpp` — the tiny proportional font used in the
  footer rows (see `README.md`'s Thanks section for credit)
- `performance_store.h/.cpp` — SD card save/load (performances, Dexed
  Presets, Grains Presets), WAV export, and SD Mgmt's
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

That 45% predates Pad Synth, Fm Synth, and Grains. The measurements below
(worst-case ~94% with Pad Synth, and Fm Synth's own ~52-63% story
including two real ITCM/QSPI placement bugs found and fixed) describe
**Pad Synth and Fm Synth specifically, both since removed** and replaced
outright by Dexed (see *Dexed* above for why a real DX7 clone made both
of those from-scratch approximations redundant at once) — kept here as
project history, since the *class* of bug they surfaced (inline/header-
defined functions silently landing in slow QSPI instead of ITCM once
they grow past whatever threshold the compiler stops inlining them
below, the same class of regression as `PitchShifter`'s own gap just
above) is a real, recurring risk worth remembering regardless of which
instrument next runs into it. Dexed's own worst-case CPU cost alongside
the looper and Grains has not yet been separately measured and recorded
here — a real gap in this document, not a claim that it's fine.

With both Pad Synth and Grains added (and voice counts tuned against
real measurements rather than paper estimates), the **genuine worst case
with everything running at once** — full 4-layer loop + reverb, 6-voice
Pad Synth, and Grains, all simultaneously active — measured **~94%**.
The per-engine on/off toggles (see *Per-engine on/off* above) are a
real, immediate mitigation if a given performance's actual worst case
turns out tighter than that in practice.

Fm Synth (mutually exclusive with Pad at the time — see *Per-engine
on/off* above) had its own separate worst-case story: two real ITCM/QSPI
code-placement bugs were found and fixed, plus a voice count trade from
8 down to 6. With those fixes in, real hardware measurement showed
**~52-57% Fm-alone** (idle vs. actively playing) and **~63% with 3 loop
layers actively playing back alongside it** — confirmed on hardware as
"much better" after being genuinely sluggish before the fix.

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
