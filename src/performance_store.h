#pragma once
#include <cstddef>
#include <cstdint>
#include "tempo_clock.h"
#include "looper_layer.h"

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

bool Save(int                slot,
          TempoClock&        tempo,
          LooperLayer*       layers,
          int                num_layers,
          float              master_volume01,
          bool               bypass,
          FilterMode         master_filter_mode,
          float              master_filter_cutoff01,
          float              master_filter_res01,
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
          ProgressFn   on_progress = nullptr);

// Renders the current in-memory performance (one full shared loop length,
// every layer's real filter/character-effect/pitch/reverb chain applied,
// same as live playback) to a new stereo 16-bit PCM WAV file under a
// "WAV/" subfolder on the SD card -- kept out of the root directory
// specifically so it never shows up as a load target in ListSlots().
// Master volume and the metronome click are deliberately NOT included
// (see performance_store.cpp); the master filter is. Blocking, like
// Save()/Load() -- call from main()'s loop, not the audio callback, and
// with g_audio_suspended held true for the whole call (see
// Ui::TriggerExport()) since this drives extra LooperLayer::Process()
// calls from outside the real ISR. Refuses to run while any layer is
// Recording or ArmedCountIn, or if nothing has been recorded yet.
bool ExportWav(TempoClock&  tempo,
               LooperLayer* layers,
               int          num_layers,
               FilterMode   master_filter_mode,
               float        master_filter_cutoff01,
               float        master_filter_res01,
               ProgressFn   on_progress = nullptr);

} // namespace PerformanceStore
