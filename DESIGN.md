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
  └──(long-press)──► Global ──(rotate)──► Tempo / Filter / Reverb / File /
                                             Export
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
| Layer: Reverb | Send (into the shared reverb bus, see below) | — | — | — |
| Layer: Gain | Input gain (1x-4x) | — | — | — |
| Global: Tempo | BPM (40-240) | Bars per loop (1-16) | Toggle metronome | Hold 800ms = save as startup default |
| Global: Filter | Cutoff (master bus) | Resonance | Cycle filter mode | — |
| Global: Reverb | Size/decay (shared bus, see below) | Bypass reverb send (independent of every layer's own Send) | — | — |
| Global: Speed | Project vari-speed (0.3x-2x, deadzone-centered on 1.0x, same curve as Layer:Speed) | — | Toggle scrub mode (see below) | Reset to 1.0x |
| Global: File | Browse save slots | — | Tap = Save, hold 400ms = New | Hold 800ms = Load |
| Global: Export | — | — | Tap = render to native 48kHz WAV ("Studio") | Tap = render to 44.1kHz WAV for MicroDexed ("CD") |

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
                  effect -> Pitch shift (independent on/off, can run
                  alongside the character effect) -> Volume/Pan ->
                  this layer's Reverb send -> ONE shared reverb bus,
                  Process()'d once per sample in main.cpp -> mixed to
                  output bus
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
  ~4x; measured with a cycle counter, even the heaviest combination this
  firmware can produce (all 4 layers with a character effect, one layer
  with pitch, reverb active) uses under half the audio callback's
  real-time budget.

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

`Phaser`, `PitchShifter` (both per-layer), and `ReverbSc` (the shared bus,
plus its counterpart in `PerformanceStore::ExportWav()`) all live in
SDRAM — libDaisy's `sdram.h` documents, and this project's linker script/
startup code confirm, that `.sdram_bss` is **not** zero-initialized at
boot, unlike ordinary SRAM `.bss`. `LooperLayer::Init()` and the
equivalent setup in `main.cpp`/`performance_store.cpp` now `memset()`
each of these objects to zero before calling their own `Init()`. This
isn't cosmetic: it was a real, reproducible bug — `PitchShifter::Init()`
doesn't touch several of its own internal fields, so without this they
started out holding raw leftover SDRAM contents, and on some boots that
decoded as NaN/Inf. Since NaN survives a plain min/max clamp unchanged
(NaN compares false against both bounds) and DaisySP's `ReverbSc` has no
NaN/Inf guard anywhere in its feedback path, one bad sample from a
pitch-enabled layer could permanently poison the shared reverb bus's
internal state — silencing the *entire* mix, not just that layer, until
a full power cycle. `LooperLayer::Process()` also clamps the pitch
stage's output to a finite value directly, as a second line of defense
against this exact failure shape regardless of cause.

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
`PERF/PERF001.DAT` .. `PERF/PERF099.DAT`, via Global:File — kept in its
own subfolder for the same reason WAV exports get their own (see below):
a tidier SD root, and it keeps `ListSlots()`/`NextFreeSlot()`'s directory
scan scoped to just that folder. The on-disk layout has a
version tag (`kFileVersion` in `performance_store.cpp`) that gets bumped
whenever a field is added — a save from an older firmware version is
rejected cleanly on load (shown as a short error on the File page)
rather than being misread, so a firmware update can mean older saves
need re-saving under the new version.

"New" (`Ui::TriggerNew()`, Button1 held 400ms on Global:File) only wipes
each layer's recorded *audio* (`LooperLayer::Clear()`) — every global
and per-layer setting is left exactly as it was. It's deliberately not
the same as applying the startup default (see below); those are two
different, independent actions.

## Startup defaults

Global:Tempo's Button2, held 800ms (`Ui::TriggerSaveDefaults()`), saves
the current *global* settings only — BPM, Bars, Master Volume, Metronome
on/off + volume, Master Filter mode/cutoff/resonance, Reverb Size, and
Bypass — as a single small `PREFS.DAT` in the SD root. Deliberately no
per-layer settings (volume/pan/filter/effect/pitch/reverb send) — those
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
- **The real, live effects chain** — filter, character effect, pitch, and
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
