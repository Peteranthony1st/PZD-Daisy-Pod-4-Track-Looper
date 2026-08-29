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
everything else from scratch: per-layer filter/effect/reverb/pitch,
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
  screen, cycles through that screen's pages.
- **Click** the encoder: only meaningful on Home, where it drills into
  the layer under the cursor.
- **Long-press** the encoder (~600ms): from Home, opens Global settings;
  from anywhere else, goes back to Home.

```
Home ──(click layer)──► Layer[n] ──(rotate)──► Status / Speed / Filter /
  │                                             Effect / Pitch / Reverb / Gain
  │                                                     (long-press ⤴ back to Home)
  └──(long-press)──► Global ──(rotate)──► Tempo / Filter / File / Export
                                                (long-press ⤴ back to Home)
```

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
| Layer: Pitch | Amount (-12..+12 semitones, unison at center) | Fun (internal modulation amount) | Toggle Pitch on/off | Cycle delay-line preset (Fast/Med/Smooth — see below) |
| Layer: Reverb | Send | Size | — | — |
| Layer: Gain | Input gain (1x-4x) | — | — | — |
| Global: Tempo | BPM (40-240) | Bars per loop (1-16) | Toggle metronome | — |
| Global: Filter | Cutoff (master bus) | Resonance | Cycle filter mode | — |
| Global: File | Browse save slots | — | Tap = Save, hold 400ms = New | Hold 800ms = Load |
| Global: Export | — | — | Tap = render current performance to WAV | — |

BPM/Bars are locked once any layer holds a recording, and the knobs stop
affecting them, so you can't accidentally pull every track out of sync —
clear every layer to unlock and pick a new tempo.

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
                  effect -> Pitch shift (independent on/off, can run
                  alongside the character effect) -> Volume/Pan ->
                  this layer's own Reverb send -> mixed to output bus
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
- **Pitch**: independent of the character effect above, not
  mutually exclusive with it — a real on/off toggle (like `FilterMode::
  Off`, not a knob position), so it can run alongside a character effect
  or on its own. DaisySP's `PitchShifter` is a delay-line-based shifter
  with real, audible processing latency that scales with its internal
  buffer size — see the delay-preset note below.
- **Reverb**: independent per layer (each layer owns its own `ReverbSc`
  instance, not a shared bus) — Send is how much of this layer's signal
  feeds it, Size is that instance's own feedback/decay.

### Pitch delay presets

`PitchShifter`'s delay-line size trades sync latency against pitch-shift
smoothness — a smaller buffer means the pitched signal lags the actual
loop position less, but raises the internal crossfade rate for the same
pitch amount, which shows up as more audible warble on bigger shifts.
Three presets, cycled with Button 2 on the Pitch page:

| Preset | Delay size | Latency @48kHz |
|---|---|---|
| Fast (default) | ~2400 samples | ~50ms |
| Med | ~6000 samples | ~125ms |
| Smooth | 16384 samples (DaisySP's stock default) | ~341ms |

## Save/load

`PerformanceStore` saves/loads a whole performance (all 4 layers' audio
plus tempo/global/per-layer settings) as one flat binary file per slot,
`PERF001.DAT` .. `PERF099.DAT`, via Global:File. The on-disk layout has a
version tag (`kFileVersion` in `performance_store.cpp`) that gets bumped
whenever a field is added — a save from an older firmware version is
rejected cleanly on load (shown as a short error on the File page)
rather than being misread, so a firmware update can mean older saves
need re-saving under the new version.

## WAV export

`PerformanceStore::ExportWav()` (Global:Export, single tap on Button 1)
renders the current in-memory performance to a standard stereo 16-bit PCM
`WAV/EXPnnn.WAV` on the SD card — kept in its own subfolder specifically
so it's invisible to `ListSlots()`/`NextFreeSlot()`, which only ever look
at bare `PERFxxx.DAT` names in the root. Unlike Save, this never
overwrites anything (always the next free number), so there's no
hold-to-confirm gesture.

- **Exactly one full loop length** (`TempoClock::GetLoopLengthSamples()`),
  always starting from the true downbeat regardless of where playback
  happened to be when Export was triggered — every layer's `play_pos_` is
  snapshotted, forced to 0 for the render, then restored afterward (even
  if the render fails partway through).
- **The real, live effects chain** — filter, character effect, pitch,
  reverb, per layer, plus the master filter — not a dry sum. This means
  the export calls each layer's actual `Process()` an extra time from the
  main loop (with `g_audio_suspended` held for the whole operation, same
  as `TriggerLoad()`, so it doesn't race the real audio ISR touching the
  same objects).
- **Master volume and the metronome click are excluded.** Master volume
  is a monitor/output-level control, not mix content — including it would
  mean the exported file's loudness depended on wherever that knob
  happened to be sitting, including all the way down. The master filter
  *is* included (it's a real mix-shaping tool), via a fresh local `Svf`
  rather than main.cpp's live one, since a filter's only state is
  short-term signal history — primed with a throwaway first pass over the
  loop before the real render so it isn't starting cold.
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
  post-effects signal is summed into a single `WAV/EXPnnn.WAV` — there's
  no way to export each layer as its own file.

## Files

- `main.cpp` — hardware init, audio callback, main loop
- `looper_layer.h/.cpp` — per-layer state machine, filter/effects/pitch/
  reverb, waveform cache, audio
- `tempo_clock.h/.cpp` — BPM/bars/metronome/count-in engine
- `ui.h/.cpp` — encoder/button/knob handling + OLED menu rendering
- `font_tomthumb.h/.cpp` — the tiny proportional font used in the
  footer rows (see `README.md`'s Thanks section for credit)
- `performance_store.h/.cpp` — SD card save/load + WAV export
- `audio_engine.h` — `g_audio_suspended`, a flag that makes the audio
  callback output silence without touching any layer/tempo state, so
  `PerformanceStore::Load()` (which restores layers one at a time,
  streaming each from SD) can't leave layers starting at different
  sample offsets just because the audio ISR kept running mid-restore
- `Makefile` — source list for the build
