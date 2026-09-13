#pragma once
#include <cstddef>
#include <cstdint>
#include "tempo_clock.h"
#include "looper_layer.h"
#include "pad_synth.h"
#include "granular_engine.h"
#include "fm_synth.h"

// SD card save/load for a whole "performance" (all layers' audio plus
// tempo/global/per-layer settings) as one flat binary file per slot.
//
// Slots are numbered 1..kMaxSlots and map to fixed 8.3 filenames
// ("PERF001.DAT" etc.) so listing/parsing never depends on long-filename
// support -- the UI is free to show a friendlier label ("1 - Performance")
// without that label ever touching the filesystem.
//
// This module owns the SD card hardware (SdmmcHandler + FatFSInterface)
// itself -- LooperLayer stays hardware-free (see its header comment) by
// only exposing plain getters/setters and raw buffer access; all file I/O
// lives here instead.
//
// Save()/Load() are blocking (SD I/O, one loop buffer can be many
// megabytes) -- call them from main()'s loop, never from the audio
// callback, same rule as Ui::Update()'s OLED writes. Both take an optional
// progress callback so the caller can redraw a progress bar during the
// (up to a couple of seconds) transfer.
namespace PerformanceStore
{
constexpr int kMaxSlots = 99;

// Mount the SD card. Call once from main(), after hw.Init().
void Init();
bool IsCardPresent();

// Re-mount the card (same f_mount() call Init() does). This hand-wired
// SD socket has no card-detect pin, so the firmware has no way to know a
// card was physically pulled/swapped/reinserted -- IsCardPresent()
// otherwise stays whatever it was at boot forever. Call this whenever
// it's reasonable to assume the card might have changed (see
// Ui::HandleEncoder()'s Home->Global transition) rather than waiting for
// a Save/Load/Export call to fail against a stale mount.
void Remount();

// Short diagnostic for the most recent Save()/Load() failure -- a step
// tag plus the FatFS FRESULT code ("hdr:3", "aud0:5", ...), e.g. for
// display on the File page. Empty string if the last call succeeded (or
// nothing has run yet). Overwritten by every Save()/Load() call.
const char* GetLastError();

// Fills out_slots ascending with existing slot numbers, returns how many
// were found (up to max_out).
int ListSlots(int* out_slots, int max_out);
// Lowest slot number with no saved file yet, or -1 if every slot is full
// or no card is present.
int NextFreeSlot();

using ProgressFn = void (*)(float progress01);

// Deletes an existing saved performance -- irreversible, same as deleting
// any other file; the caller (SD MGMT's own hold-to-confirm gesture) owns
// making that consequence clear before calling this. Also closes the gap
// this leaves behind: every higher-numbered performance still on the card
// (plus any other pre-existing gaps above it) shifts down to keep the
// numbering contiguous from 1, so a later Save New always continues right
// after the highest real performance instead of NextFreeSlot() picking
// the low number this just freed. If loaded_slot_inout is non-null and
// currently names a slot this shifts (or the one just deleted), it's
// updated in place to follow that content to its new slot (or to -1 if it
// was the slot just deleted).
bool DeleteSlot(int slot, int* loaded_slot_inout = nullptr);
// Byte-for-byte copy of an existing performance into the next free slot
// (see NextFreeSlot()) -- chunked, so this can take real time for a
// performance with a lot of recorded audio; pass on_progress the same way
// Save()/Load() do. *out_new_slot is set to the slot actually used.
bool DuplicateSlot(int slot, int* out_new_slot, ProgressFn on_progress = nullptr);

// Deals ONLY with the looper (all layers' audio plus tempo/global/per-
// layer settings) -- Pad/Fm/Grains each have their own fully independent
// save/load systems (SavePadPreset()/SaveFmPreset()/SaveGranularPreset()
// below), never embedded into a performance file. Keeps every
// instrument's saves in its own separate "bubble", performances included.
bool Save(int                slot,
          TempoClock&        tempo,
          LooperLayer*       layers,
          int                num_layers,
          float              master_volume01,
          bool               bypass,
          FilterMode         master_filter_mode,
          float              master_filter_cutoff01,
          float              master_filter_res01,
          float              reverb_size01,
          float              bypass_reverb_send01,
          ProgressFn         on_progress = nullptr);

bool Load(int          slot,
          TempoClock&  tempo,
          LooperLayer* layers,
          int          num_layers,
          float*       out_master_volume01,
          bool*        out_bypass,
          FilterMode*  out_master_filter_mode,
          float*       out_master_filter_cutoff01,
          float*       out_master_filter_res01,
          float*       out_reverb_size01,
          float*       out_bypass_reverb_send01,
          ProgressFn   on_progress = nullptr);

// Pad synth presets: parameters only, no audio -- a real snapshot (via
// PadSynth::CapturePreset()/ApplyPreset()), not a reference to whatever
// performance it originally came from, so a later Save/Load of a
// DIFFERENT performance can never invalidate an already-saved preset.
// Numbered 1..kMaxPadPresets; 1..PadSynth::kNumFactoryPresets are the
// permanently read-only, firmware-embedded factory presets (see
// PadSynth::GetFactoryPreset()) -- LoadPadPreset() serves those directly
// without touching the card, and SavePadPreset()/NextFreePadPresetSlot()
// never target them. User saves start right after that range.
constexpr int kMaxPadPresets = 99;

bool SavePadPreset(int slot, const PadSynth::PadPresetData& preset);
bool LoadPadPreset(int slot, PadSynth::PadPresetData* out_preset);
// Lists existing USER slots only (ascending) -- factory presets are
// always available and aren't part of this scan. Same shape as
// ListSlots().
int  ListPadPresets(int* out_numbers, int max_out);
// Lowest free USER slot, or -1 if the whole range is full/no card.
int  NextFreePadPresetSlot();
// Refuses factory slots (1..PadSynth::kNumFactoryPresets aren't files at
// all) same as SavePadPreset() -- see DeleteSlot()/DuplicateSlot() above
// for the general shape, including the same gap-closing renumbering and
// loaded_slot_inout follow-along.
bool DeletePadPreset(int slot, int* loaded_slot_inout = nullptr);
bool DuplicatePadPreset(int slot, int* out_new_slot, ProgressFn on_progress = nullptr);

// Fm synth presets: parameters only, no audio -- same shape as Pad
// presets above, including the same factory range: 1..FmSynth::
// kNumFactoryPresets are the permanently read-only, firmware-embedded
// factory presets (see FmSynth::GetFactoryPreset()) -- LoadFmPreset()
// serves those directly without touching the card, and SaveFmPreset()/
// NextFreeFmPresetSlot() never target them. User saves start right after
// that range.
constexpr int kMaxFmPresets = 99;

bool SaveFmPreset(int slot, const FmSynth::FmPresetData& preset);
bool LoadFmPreset(int slot, FmSynth::FmPresetData* out_preset);
int  ListFmPresets(int* out_numbers, int max_out);
int  NextFreeFmPresetSlot();
bool DeleteFmPreset(int slot, int* loaded_slot_inout = nullptr);
bool DuplicateFmPreset(int slot, int* out_new_slot, ProgressFn on_progress = nullptr);

// Grains (GranularEngine) presets: parameters AND the captured audio
// itself (unlike Pad presets above, which are parameters only) -- a
// Grains preset IS a specific captured sound plus how it's being played
// back, so loading one needs to restore both. No factory range (there's
// no hand-tuned-patch equivalent for a captured sample), so every slot
// 1..kMaxGranularPresets is a normal user save. Numbered/found the same
// way as PadPreset's own user range.
constexpr int kMaxGranularPresets = 99;

// audio_l/audio_r: whatever GranularEngine::GetSourceL()/R() currently
// point at; audio_len: GetSourceLen(). Blocking (chunked SD write, same
// kChunkSamples streaming as Save()'s own layer audio) -- call from the
// main loop with a progress callback, same rule as Save()/Load()/
// ExportWav() above, not the audio ISR.
bool SaveGranularPreset(int                                       slot,
                        const GranularEngine::GranularPresetData& preset,
                        const float*                              audio_l,
                        const float*                              audio_r,
                        size_t                                    audio_len,
                        ProgressFn                                on_progress = nullptr);
// out_audio_l/r must have room for at least audio_capacity samples each
// -- the file's own audio_len is clamped to that before reading, same
// "never write past what the caller actually owns" reasoning as every
// other buffer-filling call in this project. *out_audio_len is set to
// however many samples were actually read.
bool LoadGranularPreset(int                                  slot,
                        GranularEngine::GranularPresetData* out_preset,
                        float*                               out_audio_l,
                        float*                               out_audio_r,
                        size_t                                audio_capacity,
                        size_t*                               out_audio_len,
                        ProgressFn                            on_progress = nullptr);
int  ListGranularPresets(int* out_numbers, int max_out);
int  NextFreeGranularPresetSlot();
// No factory range to guard against here (see kMaxGranularPresets's own
// comment) -- see DeleteSlot()/DuplicateSlot() above for the general
// shape, including the same gap-closing renumbering and
// loaded_slot_inout follow-along. Duplicating copies the whole file
// (params + captured audio) in one chunked byte-for-byte pass, not
// through GranularEngine at all.
bool DeleteGranularPreset(int slot, int* loaded_slot_inout = nullptr);
bool DuplicateGranularPreset(int slot, int* out_new_slot, ProgressFn on_progress = nullptr);

// Importing a user-supplied WAV file (from a computer, dropped into
// IMPORT/ on the SD card) as Grains capture audio -- a third capture
// source alongside Direct Record and From Layer. Accepts 16-bit PCM,
// mono or stereo, 48000 or 44100 Hz; 44100 Hz files are resampled up to
// the native 48000 Hz engine rate with the same exact-ratio linear
// resampler ExportWav() already uses in the other direction. Anything
// else (24-bit, non-PCM, other sample rates) is cleanly refused
// (GetLastError() reports why) rather than misread -- there's no
// general resampler/format-conversion here, just these two exact rates.
constexpr int kMaxImportWavNameLen = 60;
// Fills out_names ascending (directory order) with .wav/.WAV filenames
// found in IMPORT/ on the SD root, returns how many were found (up to
// max_out). A name longer than kMaxImportWavNameLen is SKIPPED entirely
// rather than truncated -- an earlier version truncated it instead, which
// silently produced a listed name that didn't match any real file on
// disk (ImportWav() would then fail to open it, FR_NO_FILE) since
// truncating can chop off the ".wav" extension or land mid-name.
// Realistic sample filenames are well under this length in practice.
// Also skips macOS "._name.wav" AppleDouble sidecar files -- these get
// silently created alongside every real file when Finder (or many other
// macOS copy tools) writes to a non-HFS+ volume like a FAT32 SD card;
// they end in .wav and so pass the extension filter, but their content
// isn't a real WAV file (fails ImportWav()'s own RIFF magic check).
int  ListImportWavFiles(char out_names[][kMaxImportWavNameLen + 1], int max_out);
// out_l/out_r must have room for at least audio_capacity samples each --
// the file's own length (after any resampling) is truncated to that,
// same "never write past what the caller owns" rule as
// LoadGranularPreset(). *out_len is set to however many samples were
// actually produced.
bool ImportWav(const char* filename,
               float*      out_l,
               float*      out_r,
               size_t      audio_capacity,
               size_t*     out_len,
               ProgressFn  on_progress = nullptr);

// Renders the current in-memory performance (one full shared loop length,
// every layer's real filter/character-effect/reverb chain applied,
// same as live playback) to a new stereo 16-bit PCM WAV file. Two
// independent output modes, selected by for_microdexed:
//   false: full-quality native 48000 Hz, under "WAV/" -- general
//     purpose, kept out of the SD root so it never shows up as a load
//     target in ListSlots().
//   true: same performance/DSP chain (the chain itself always runs at
//     native 48kHz -- see ExportWav()'s sample_rate local), but the
//     final output is resampled to 44100 Hz (Resampler48to44_1, in the
//     .cpp) and written under "custom/" -- that folder name is
//     required, not cosmetic: MicroDexed Touch scans its SD card for a
//     folder literally named "custom" and expects 44100 Hz PCM.
// Each mode has its own independent EXPnnn numbering sequence.
// Master volume and the metronome click are deliberately NOT included
// (see performance_store.cpp); the master filter and the shared reverb
// bus (reverb_size01 -- see Ui::GetReverbSize01()) both are. project_speed
// is the live vari-speed multiplier (see Ui::GetProjectSpeed()) -- unlike
// master volume, this one IS captured into the export as-is, on purpose:
// vari-speed is something a user dials in deliberately as part of a
// performance (e.g. the classic "record fast, play back normal" tape
// trick), not an incidental monitor-level knob position, so baking in
// whatever it's currently set to when Export is pressed is the wanted
// behavior, not a leak of live-only state. Blocking, like Save()/Load()
// -- call from main()'s loop, not the audio callback, and with
// g_audio_suspended held true for the whole call (see Ui::TriggerExport())
// since this drives extra LooperLayer::Process() calls from outside the
// real ISR. Refuses to run while any layer is Recording or ArmedCountIn,
// or if nothing has been recorded yet.
bool ExportWav(TempoClock&  tempo,
               LooperLayer* layers,
               int          num_layers,
               FilterMode   master_filter_mode,
               float        master_filter_cutoff01,
               float        master_filter_res01,
               float        reverb_size01,
               bool         for_microdexed,
               float        project_speed,
               ProgressFn   on_progress = nullptr);

// User-settable startup defaults -- every *global* setting, deliberately
// no per-layer ones (see Global:Tempo's Button2 hold). Stored as a
// single small "PREFS.DAT" in the SD root, reusing the same on-disk
// header shape Save()/Load() already use for these exact fields (just
// without any layers/audio following it) rather than a second format.
//
// LoadPrefs() returns false (every output left untouched) if the card
// is missing, no PREFS.DAT exists yet, or it doesn't look like a prefs
// file (bad magic/version, e.g. left over from an older firmware) --
// none of those are errors worth surfacing; the caller's own hardcoded
// defaults should just stand.
bool SavePrefs(TempoClock&  tempo,
               float        master_volume01,
               bool         bypass,
               FilterMode   master_filter_mode,
               float        master_filter_cutoff01,
               float        master_filter_res01,
               float        reverb_size01,
               float        bypass_reverb_send01);

bool LoadPrefs(float*      out_bpm,
               int*        out_bars,
               float*      out_master_volume01,
               bool*       out_bypass,
               FilterMode* out_master_filter_mode,
               float*      out_master_filter_cutoff01,
               float*      out_master_filter_res01,
               float*      out_reverb_size01,
               float*      out_bypass_reverb_send01,
               bool*       out_metronome_enabled,
               float*      out_metronome_vol01);

} // namespace PerformanceStore
