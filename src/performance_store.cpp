#include "performance_store.h"
#include <cstdio>
#include <cstring>
#include <cctype>
#include <math.h>
#include "per/sdmmc.h"
#include "sys/fatfs.h"

using namespace daisy;

// ff.c (compiled into libdaisy.a with _USE_LFN enabled) calls these for
// every file operation, not just actual long-filename ones, and they're
// not provided by the prebuilt library -- normally they'd come from
// FatFs's option/unicode.c, but that's a large Unicode case-folding
// table we don't want to spend flash on. Every filename this firmware
// ever opens is a fixed, plain-ASCII 8.3 name ("PERF001.DAT", see
// SlotFilename() below), so a minimal ASCII-only stand-in is all that's
// ever actually exercised.
extern "C" WCHAR ff_convert(WCHAR chr, UINT dir)
{
    (void)dir;
    return chr < 0x80 ? chr : 0; // 0 = "unconvertible", fine for pure ASCII
}
extern "C" WCHAR ff_wtoupper(WCHAR chr)
{
    return (chr >= 'a' && chr <= 'z') ? (WCHAR)(chr - 0x20) : chr;
}

namespace PerformanceStore
{
namespace
{
daisy::SdmmcHandler   sdmmc;
daisy::FatFSInterface fsi;
bool                  card_ready = false;

char last_error[24] = {};
void SetError(const char* step, FRESULT fr)
{
    snprintf(last_error, sizeof(last_error), "%s:%d", step, (int)fr);
}
void ClearError()
{
    last_error[0] = '\0';
}

// Chunk size for streaming loop audio to/from the SD card -- small enough
// to keep progress updates responsive, large enough to not be dominated
// by per-call SD overhead.
constexpr UINT kChunkSamples = 4096;

// Kept inside its own "PERF/" subfolder, same reasoning as "custom/"
// below -- tidier SD root, and it's still a plain PERFxxx.DAT name
// underneath so nothing else about the format changes.
void SlotFilename(int slot, char* out, size_t out_size)
{
    snprintf(out, out_size, "PERF/PERF%03d.DAT", slot);
}

// Own subfolder/numbering, same reasoning as SlotFilename()'s own split
// from the SD root -- a Grains preset is never targeted by ListSlots()
// this way either.
void GranularPresetFilename(int slot, char* out, size_t out_size)
{
    snprintf(out, out_size, "GRNP/PRES%03d.DAT", slot);
}

// Own subfolder/numbering, same reasoning as GranularPresetFilename()
// above. Only ever called for USER slots (see SaveDexedPreset() and
// friends' own factory-range guard) -- factory slots are never files.
void DexedPresetFilename(int slot, char* out, size_t out_size)
{
    snprintf(out, out_size, "DEXP/PRES%03d.DAT", slot);
}

// Own subfolder for user-supplied WAV files to import as Grains capture
// audio -- unlike every other subfolder here, filenames underneath this
// one are arbitrary (whatever the user named the file on their
// computer), not a fixed 8.3 numbered pattern, so this takes a name
// rather than a slot number.
void ImportWavFilename(const char* filename, char* out, size_t out_size)
{
    snprintf(out, out_size, "IMPORT/%s", filename);
}

// Bare filename in the SD root (unlike PERFxxx.DAT/EXPnnn.wav, this one's
// not in its own subfolder -- it's a single file, not something that
// clutters a listing) -- see SavePrefs()/LoadPrefs().
constexpr const char* kPrefsFilename = "PREFS.DAT";

inline float Clampf(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

// Each export mode is kept in its own subfolder with its own independent
// EXPnnn numbering sequence (see ExportWav()'s for_microdexed param) --
// "WAV/" for full-quality native exports, "custom/" for MicroDexed-ready
// 44.1kHz ones. "custom" is deliberate, not cosmetic -- that's the exact
// folder name MicroDexed Touch scans its SD slots for, so that card,
// moved straight into MicroDexed's second slot, is already laid out the
// way it expects with no copy step needed at all. Neither ever collides
// with ListSlots()/NextFreeSlot(), which only look inside "PERF/" (see
// SlotFilename() above).
void ExportFilename(bool for_microdexed, int n, char* out, size_t out_size)
{
    if(for_microdexed)
        snprintf(out, out_size, "custom/EXP%03d.wav", n);
    else
        snprintf(out, out_size, "WAV/EXP%03d.wav", n);
}

// Lowest-numbered free EXPnnn file in the given mode's folder -- same
// f_stat-scan pattern as NextFreeSlot(), just scoped per-mode. -1 if that
// mode's whole 001..999 range is somehow full.
int NextFreeExportNumber(bool for_microdexed)
{
    for(int n = 1; n <= 999; n++)
    {
        char    fname[24];
        FILINFO fno;
        ExportFilename(for_microdexed, n, fname, sizeof(fname));
        if(f_stat(fname, &fno) != FR_OK)
            return n;
    }
    return -1;
}

// Canonical 44-byte RIFF/WAVE PCM header. Every field lands on its own
// natural alignment boundary in this exact order (verified by the
// static_assert below), so this is portable across compilers without
// needing a packed attribute -- same "plain fields, stable layout"
// reasoning as FileHeader/LayerHeader above.
struct WavHeader
{
    char     riff[4];         // "RIFF"
    uint32_t riff_size;       // 36 + data_size
    char     wave[4];         // "WAVE"
    char     fmt[4];          // "fmt "
    uint32_t fmt_size;        // 16 for PCM
    uint16_t audio_format;    // 1 = PCM
    uint16_t num_channels;    // 2
    uint32_t sample_rate;
    uint32_t byte_rate;       // sample_rate * block_align
    uint16_t block_align;     // num_channels * bytes_per_sample
    uint16_t bits_per_sample; // 16
    char     data[4];         // "data"
    uint32_t data_size;       // total_samples * num_channels * bytes_per_sample
};
static_assert(sizeof(WavHeader) == 44, "WavHeader must be the canonical 44-byte layout");

// Upper bound on num_layers for the small play_pos_ snapshot/restore
// array below -- this project only ever has 4 (kNumLayers in main.cpp);
// generous headroom, not a hard architectural limit.
constexpr int kMaxExportLayers = 8;

// Exact-ratio linear-interpolation downsampler, 48000 -> 44100 Hz
// (44100/48000 == 147/160 exactly, gcd 300). Integer phase tracking, not
// a float accumulator, so there's zero long-term drift over an
// arbitrarily long loop. Streaming by design -- Push() only ever looks
// at the two most-recently-pushed samples, so its state survives being
// called across ExportWav()'s chunked I/O loop with no discontinuity at
// chunk boundaries; the chunking is purely an I/O detail this never
// sees. Only ever driven during ExportWav()'s pass 1 (the real write
// pass) when for_microdexed is true.
struct Resampler48to44_1
{
    static constexpr uint64_t kInRate  = 160;
    static constexpr uint64_t kOutRate = 147;

    bool     have_prev = false;
    float    prev_l = 0.f, prev_r = 0.f;
    uint64_t in_count  = 0;
    uint64_t out_count = 0;

    // Push one native-rate sample. Returns true if an output sample was
    // produced (written to *out_l/*out_r) -- most calls return false,
    // since kOutRate < kInRate. Never more than one emission per call
    // (kInRate/kOutRate < 2, so consecutive output positions are always
    // further apart than one input-sample interval).
    bool Push(float in_l, float in_r, float* out_l, float* out_r)
    {
        bool emitted = false;
        if(have_prev)
        {
            uint64_t n = in_count + 1;
            if(out_count * kInRate < n * kOutRate)
            {
                uint64_t base = in_count * kOutRate;
                float    frac = (float)(out_count * kInRate - base) / (float)kOutRate;
                *out_l  = prev_l + (in_l - prev_l) * frac;
                *out_r  = prev_r + (in_r - prev_r) * frac;
                out_count++;
                emitted = true;
            }
        }
        prev_l = in_l;
        prev_r = in_r;
        have_prev = true;
        in_count++;
        return emitted;
    }

    // Exact output-frame count for `n` native input frames -- needed to
    // size the WAV header's data_size before the render loop runs.
    static uint32_t OutputFrames(size_t n)
    {
        return (uint32_t)(((uint64_t)n * kOutRate + (kInRate - 1)) / kInRate);
    }
};

// Mirror image of Resampler48to44_1 above for ImportWav()'s own 44100 ->
// 48000 Hz upsampling -- same exact-ratio linear interpolation, just
// kInRate/kOutRate swapped (48000/44100 reduces to the same 160/147 as
// the other direction, just inverted).
struct Resampler44_1to48
{
    static constexpr uint64_t kInRate  = 147;
    static constexpr uint64_t kOutRate = 160;

    // 4-sample rolling history (s0 oldest .. s3 newest) for Catmull-Rom
    // cubic interpolation, not the 2-point linear interpolation an
    // earlier version used. Linear interpolation between just two
    // samples is a known source of audible aliasing/imaging artifacts on
    // bright, dense program material -- fine for a quick draft, but
    // exactly what was left over (heard as "poppy"/crackly", worse under
    // Grains' own grain-boundary windowing) once the separate dropped-
    // sample bug above was fixed. Cubic uses the two neighboring samples
    // either side of the segment being interpolated to fit a smooth
    // curve instead of a straight line, which is a large, well-
    // established quality step up for a modest cost (one extra sample of
    // input latency, a few more FLOPs per output sample) -- not a full
    // windowed-sinc filter (a bigger, higher-risk change: a filter
    // kernel table plus real DSP tuning that's hard to verify without
    // being able to listen on hardware directly), which would be the
    // next step up if this still isn't enough.
    float    s0_l = 0.f, s1_l = 0.f, s2_l = 0.f, s3_l = 0.f;
    float    s0_r = 0.f, s1_r = 0.f, s2_r = 0.f, s3_r = 0.f;
    int      real_count = 0; // total real samples pushed so far (saturating checks only need <3)
    uint64_t in_count    = 0; // input-sample index of s1/s2's segment (see Push())
    uint64_t out_count  = 0;

    static float CatmullRom(float p0, float p1, float p2, float p3, float t)
    {
        float t2 = t * t;
        float t3 = t2 * t;
        return 0.5f
               * (2.f * p1 + (p2 - p0) * t + (2.f * p0 - 5.f * p1 + 4.f * p2 - p3) * t2
                  + (3.f * p1 - p0 - 3.f * p2 + p3) * t3);
    }

    // Push one 44100 Hz sample. Writes up to 2 output (48000 Hz) samples
    // into out_l/out_r (each must have room for 2) and returns how many
    // were actually produced -- never more than 2 since kOutRate/kInRate
    // (160/147) is < 2 (see the dropped-sample comment this replaced).
    //
    // in_count/out_count start at 0 and only ever advance from inside the
    // real-processing branch below, in lockstep with the (already
    // verified correct) linear resampler's own timing -- an earlier
    // version instead jumped in_count straight to 1 once history was
    // primed while leaving out_count at 0, which desynced the two
    // permanently (every frac computed from then on used a mismatched
    // pair of counters, extrapolating wildly instead of interpolating)
    // and produced silence/garbage for the whole file instead of the
    // first few samples it meant to skip.
    int Push(float in_l, float in_r, float* out_l, float* out_r)
    {
        s0_l = s1_l; s1_l = s2_l; s2_l = s3_l; s3_l = in_l;
        s0_r = s1_r; s1_r = s2_r; s2_r = s3_r; s3_r = in_r;
        real_count++;

        // The very first segment [x[0],x[1]] finalizes once this call's
        // new sample (x[2]) is available as its s3 lookahead -- i.e. on
        // the 3rd real push, at which point s1==x[0]/s2==x[1]/s3==x[2]
        // but s0 is still the initial 0 (there's no real x[-1]) --
        // clamped to s1 (duplicate the edge sample) rather than left as
        // a stray 0, same idea as any edge-clamped interpolation.
        if(real_count < 3)
            return 0;
        if(real_count == 3)
        {
            s0_l = s1_l;
            s0_r = s1_r;
        }

        int      emitted = 0;
        uint64_t n       = in_count + 1;
        while(emitted < 2 && out_count * kInRate < n * kOutRate)
        {
            uint64_t base = in_count * kOutRate;
            float    frac = (float)(out_count * kInRate - base) / (float)kOutRate;
            out_l[emitted] = CatmullRom(s0_l, s1_l, s2_l, s3_l, frac);
            out_r[emitted] = CatmullRom(s0_r, s1_r, s2_r, s3_r, frac);
            out_count++;
            emitted++;
        }
        in_count++;
        return emitted;
    }
};

// Fixed-layout header, written/read as one raw block. Every field is a
// plain float/int32/uint8 (no bitfields, no padding-sensitive types) so
// the layout is stable across compilers -- this firmware is both the only
// writer and the only reader, so exact binary compatibility with anything
// else was never a goal.
struct FileHeader
{
    char     magic[4]; // "OURO"
    uint32_t version;
    uint32_t num_layers;
    float    bpm;
    int32_t  bars;
    uint8_t  metronome_enabled;
    uint8_t  bypass;
    float    metronome_vol01;
    float    master_volume01;
    int32_t  master_filter_mode;
    float    master_filter_cutoff01;
    float    master_filter_res01;
    // Shared reverb bus's Size/decay (see main.cpp's fx_reverb_shared) --
    // global now, not per-layer any more (see LayerHeader below).
    float    reverb_size01;
    // How much of the Bypass live-monitor signal feeds that same shared
    // reverb bus (see Ui::GetBypassReverbSend01()) -- independent of
    // every layer's own Send.
    float    bypass_reverb_send01;

    // Pad synth settings (added in version 7, removed in version 10 when
    // Pad synth itself was removed from the project) -- a performance
    // deals ONLY with the looper (audio + tempo/global/per-layer
    // settings); Grains owns its own fully independent save/load system
    // instead (see SaveGranularPreset()), with no cross-embedding into a
    // performance file.
};

struct LayerHeader
{
    uint32_t record_len; // samples (0 = empty layer, no audio follows)
    float    volume01;
    float    pan01;
    float    speed01;
    float    input_gain01;
    int32_t  filter_mode;
    float    filter_cutoff01;
    float    filter_res01;
    int32_t  effect;
    float    effect_param_a01;
    float    effect_param_b01;
    float    reverb_send01; // Size is now a global FileHeader field, not per-layer
};

// Bumped 1 -> 2 -> ... -> 9 -> 10 as FileHeader/LayerHeader's layout
// changed (most recently: removed every pad_* field -- a performance now
// deals only with the looper, see FileHeader's own comment) -- Load()
// already rejects a version mismatch cleanly (see below), so a
// performance saved under an older version will correctly fail to load
// rather than being misread. Same accepted tradeoff as every past bump.
constexpr uint32_t kFileVersion = 10;

} // namespace

void Init()
{
    SdmmcHandler::Config sd_cfg;
    // Dropped from STANDARD (25MHz) to MEDIUM_SLOW (12.5MHz) -- this is a
    // hand-wired SD socket, not a factory module, and an intermittent
    // FR_INVALID_OBJECT specifically at f_close() (after writes had
    // already succeeded earlier in the same call) is a classic symptom
    // of marginal signal integrity at higher SDMMC clock rates rather
    // than a logic bug. Trades some transfer speed for a lot more
    // margin, which matters more than raw throughput here.
    sd_cfg.speed = SdmmcHandler::Speed::MEDIUM_SLOW;
    sdmmc.Init(sd_cfg);

    FatFSInterface::Config fsi_cfg;
    fsi_cfg.media = FatFSInterface::Config::MEDIA_SD;
    fsi.Init(fsi_cfg);

    FATFS& fs = fsi.GetSDFileSystem();
    // opt=1 forces the mount to happen now (rather than lazily on first
    // access), so IsCardPresent() actually reflects whether a card is in
    // the slot right after Init() instead of only finding out on first use.
    card_ready = f_mount(&fs, fsi.GetSDPath(), 1) == FR_OK;
}

void Remount()
{
    FATFS& fs = fsi.GetSDFileSystem();
    card_ready = f_mount(&fs, fsi.GetSDPath(), 1) == FR_OK;
}

bool IsCardPresent()
{
    return card_ready;
}

int ListSlots(int* out_slots, int max_out)
{
    if(!card_ready)
        return 0;
    int count = 0;
    for(int slot = 1; slot <= kMaxSlots && count < max_out; slot++)
    {
        char    fname[20];
        FILINFO fno;
        SlotFilename(slot, fname, sizeof(fname));
        if(f_stat(fname, &fno) == FR_OK)
            out_slots[count++] = slot;
    }
    return count;
}

int NextFreeSlot()
{
    if(!card_ready)
        return -1;
    for(int slot = 1; slot <= kMaxSlots; slot++)
    {
        char    fname[20];
        FILINFO fno;
        SlotFilename(slot, fname, sizeof(fname));
        if(f_stat(fname, &fno) != FR_OK)
            return slot;
    }
    return -1;
}

// -- SD MGMT: generic delete/duplicate, shared by every category below --

bool DeleteFileGeneric(const char* path)
{
    ClearError();
    FRESULT fr = f_unlink(path);
    if(fr != FR_OK)
    {
        SetError("delete", fr);
        return false;
    }
    return true;
}

// Byte-for-byte, chunked (same reasoning as every audio-streaming loop
// elsewhere in this file: keeps progress responsive, doesn't need a
// buffer anywhere near the size of a whole performance/preset). Refuses
// to leave a partial file behind on any failure -- f_unlink()s dst_path
// rather than risking a later Load() silently misreading a half-written
// duplicate as real data.
bool CopyFileChunked(const char* src_path, const char* dst_path, ProgressFn on_progress)
{
    ClearError();
    static FIL src, dst;
    FRESULT    fr = f_open(&src, src_path, FA_READ);
    if(fr != FR_OK)
    {
        SetError("dupopen", fr);
        return false;
    }
    // FA_CREATE_NEW, not FA_CREATE_ALWAYS -- dst_path came from a
    // NextFree*Slot() scan moments ago, so this failing at all would mean
    // something raced/changed underneath us; better to fail loudly than
    // silently overwrite whatever's actually there.
    fr = f_open(&dst, dst_path, FA_CREATE_NEW | FA_WRITE);
    if(fr != FR_OK)
    {
        SetError("dupcreate", fr);
        f_close(&src);
        return false;
    }

    FSIZE_t       total = f_size(&src);
    FSIZE_t       done  = 0;
    static uint8_t buf[4096]; // see the DTCMRAM/DMA comment on Save() above
    bool          ok    = true;
    while(ok && done < total)
    {
        UINT to_read = (UINT)((total - done) < sizeof(buf) ? (total - done) : sizeof(buf));
        UINT br, bw;
        fr = f_read(&src, buf, to_read, &br);
        ok = fr == FR_OK && br == to_read;
        if(!ok)
        {
            SetError("dupread", fr);
            break;
        }
        fr = f_write(&dst, buf, br, &bw);
        ok = fr == FR_OK && bw == br;
        if(!ok)
        {
            SetError("dupwrite", fr);
            break;
        }
        done += br;
        if(on_progress && total > 0)
            on_progress((float)done / (float)total);
    }

    f_close(&src);
    FRESULT close_dst = f_close(&dst);
    if(ok && close_dst != FR_OK)
    {
        SetError("dupclose", close_dst);
        ok = false;
    }
    if(!ok)
        f_unlink(dst_path);
    return ok;
}

// After a delete, closes every gap from just above the deleted slot up to
// max_slot -- not just the one slot that was just freed -- shifting each
// surviving file down to the lowest free number below it. This is what
// keeps the numbering contiguous from 1 (or from the first user slot, for
// the two ranges that reserve low numbers for something else) so that
// NextFree*Slot()'s own "lowest free slot" scan naturally continues right
// after the last real save instead of reusing a hole a delete left behind
// while other, higher-numbered saves sit stranded above it. Scans past
// any gap rather than stopping at the first one, so it also repairs any
// non-contiguous numbering left over from before this existed.
//
// filename_fn must match SlotFilename()/GranularPresetFilename()'s own
// signature. If loaded_slot_inout is
// non-null and currently names a slot this moves, it's updated in place
// to follow that file to wherever it actually ends up.
void CompactSlotsAfterDelete(int  deleted_slot,
                              int  max_slot,
                              void (*filename_fn)(int, char*, size_t),
                              int* loaded_slot_inout)
{
    int target = deleted_slot;
    for(int src = deleted_slot + 1; src <= max_slot; src++)
    {
        char    src_name[24];
        FILINFO fno;
        filename_fn(src, src_name, sizeof(src_name));
        if(f_stat(src_name, &fno) != FR_OK)
            continue; // gap here -- nothing to shift down (yet)
        char dst_name[24];
        filename_fn(target, dst_name, sizeof(dst_name));
        f_rename(src_name, dst_name);
        if(loaded_slot_inout && *loaded_slot_inout == src)
            *loaded_slot_inout = target;
        target++;
    }
}

bool DeleteSlot(int slot, int* loaded_slot_inout)
{
    if(!card_ready || slot < 1 || slot > kMaxSlots)
        return false;
    char fname[20];
    SlotFilename(slot, fname, sizeof(fname));
    if(!DeleteFileGeneric(fname))
        return false;
    if(loaded_slot_inout && *loaded_slot_inout == slot)
        *loaded_slot_inout = -1;
    CompactSlotsAfterDelete(slot, kMaxSlots, SlotFilename, loaded_slot_inout);
    return true;
}

bool DuplicateSlot(int slot, int* out_new_slot, ProgressFn on_progress)
{
    if(!out_new_slot)
        return false;
    *out_new_slot = -1;
    if(!card_ready || slot < 1 || slot > kMaxSlots)
        return false;
    char    src[20];
    FILINFO fno;
    SlotFilename(slot, src, sizeof(src));
    if(f_stat(src, &fno) != FR_OK)
    {
        SetError("dupsrc", FR_NO_FILE);
        return false;
    }
    int new_slot = NextFreeSlot();
    if(new_slot < 0)
    {
        SetError("dupslot", FR_OK);
        return false;
    }
    char dst[20];
    SlotFilename(new_slot, dst, sizeof(dst));
    if(!CopyFileChunked(src, dst, on_progress))
        return false;
    *out_new_slot = new_slot;
    return true;
}

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
          ProgressFn         on_progress)
{
    if(!card_ready || slot < 1 || slot > kMaxSlots)
        return false;

    char fname[20];
    SlotFilename(slot, fname, sizeof(fname));

    FRESULT mkdir_res = f_mkdir("PERF");
    if(mkdir_res != FR_OK && mkdir_res != FR_EXIST)
    {
        SetError("mkdir", mkdir_res);
        return false;
    }

    // FIL/FileHeader/LayerHeader are `static` here rather than ordinary
    // locals on purpose: this project's stack lives in DTCMRAM (see
    // STM32H750IB_flash.lds's _estack, right at the top of the 128K
    // DTCMRAM region), and the SDMMC peripheral's DMA cannot reach
    // DTCMRAM at all (see fatfs.h's FatFSInterface doc comment). Handing
    // f_write()/f_read() a plain stack-local buffer as the source/
    // destination silently fails the transfer -- that's exactly what was
    // happening here: f_open() alone (no DMA, just a directory entry)
    // succeeded, so a 0-byte file would appear on the card, but the very
    // first f_write() of the header (a stack local) failed, so Save()
    // always bailed out before writing anything past that. `static`
    // places these in ordinary .bss instead, which -- like every other
    // plain global in this project -- lands in AXI SRAM, which SDMMC's
    // DMA can reach. Safe as `static` (not re-entrancy-safe) because
    // Save()/Load() are only ever called synchronously, one at a time,
    // from the main loop (see Ui::TriggerSave()/TriggerLoad()).
    ClearError();
    static FIL file;
    FRESULT    fr = f_open(&file, fname, FA_CREATE_ALWAYS | FA_WRITE);
    if(fr != FR_OK)
    {
        SetError("open", fr);
        return false;
    }

    static FileHeader hdr;
    hdr = FileHeader{};
    memcpy(hdr.magic, "OURO", 4);
    hdr.version                = kFileVersion;
    hdr.num_layers              = (uint32_t)num_layers;
    hdr.bpm                     = tempo.GetBpm();
    hdr.bars                    = tempo.GetBars();
    hdr.metronome_enabled       = tempo.IsMetronomeEnabled() ? 1 : 0;
    hdr.bypass                  = bypass ? 1 : 0;
    hdr.metronome_vol01         = tempo.GetMetronomeVolume01();
    hdr.master_volume01         = master_volume01;
    hdr.master_filter_mode      = (int32_t)master_filter_mode;
    hdr.master_filter_cutoff01  = master_filter_cutoff01;
    hdr.master_filter_res01     = master_filter_res01;
    hdr.reverb_size01           = reverb_size01;
    hdr.bypass_reverb_send01    = bypass_reverb_send01;

    UINT bw;
    fr        = f_write(&file, &hdr, sizeof(hdr), &bw);
    bool ok   = fr == FR_OK && bw == sizeof(hdr);
    if(!ok)
        SetError("hdr", fr);

    // Total sample-halves (L+R across all layers) for progress reporting.
    size_t total_units = 0;
    for(int i = 0; i < num_layers; i++)
        total_units += layers[i].GetRecordedLength() * 2;
    size_t done_units = 0;

    for(int i = 0; ok && i < num_layers; i++)
    {
        LooperLayer& layer = layers[i];
        size_t       len   = layer.GetRecordedLength();

        static LayerHeader lh; // see the FIL/FileHeader comment above
        lh = LayerHeader{};
        lh.record_len       = (uint32_t)len;
        lh.volume01          = layer.GetVolume01();
        lh.pan01              = layer.GetPan01();
        lh.speed01            = layer.GetSpeed01();
        lh.input_gain01       = layer.GetInputGain01();
        lh.filter_mode        = (int32_t)layer.GetFilterMode();
        lh.filter_cutoff01    = layer.GetFilterCutoff01();
        lh.filter_res01       = layer.GetFilterResonance01();
        lh.effect              = (int32_t)layer.GetEffect();
        lh.effect_param_a01   = layer.GetEffectParamA01();
        lh.effect_param_b01   = layer.GetEffectParamB01();
        lh.reverb_send01      = layer.GetReverbSend01();

        fr = f_write(&file, &lh, sizeof(lh), &bw);
        ok = fr == FR_OK && bw == sizeof(lh);
        if(!ok)
        {
            char step[8];
            snprintf(step, sizeof(step), "lhdr%d", i);
            SetError(step, fr);
            continue;
        }
        if(len == 0)
            continue;

        float* srcs[2] = {layer.GetBufferL(), layer.GetBufferR()};
        for(int ch = 0; ok && ch < 2; ch++)
        {
            size_t written = 0;
            while(ok && written < len)
            {
                UINT n = (UINT)((len - written) < kChunkSamples ? (len - written)
                                                                  : kChunkSamples);
                UINT bytes = n * sizeof(float);
                fr = f_write(&file, srcs[ch] + written, bytes, &bw);
                ok = fr == FR_OK && bw == bytes;
                if(!ok)
                {
                    char step[8];
                    snprintf(step, sizeof(step), "aud%d%d", i, ch);
                    SetError(step, fr);
                    break;
                }
                written += n;
                done_units += n;
                if(on_progress && total_units > 0)
                    on_progress((float)done_units / (float)total_units);
            }
        }
    }

    FRESULT close_res = f_close(&file);
    if(close_res != FR_OK)
        SetError("close", close_res);
    return ok && close_res == FR_OK;
}

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
          ProgressFn   on_progress)
{
    if(!card_ready || slot < 1 || slot > kMaxSlots)
        return false;

    char fname[20];
    SlotFilename(slot, fname, sizeof(fname));

    ClearError();
    static FIL file; // see the DTCMRAM/DMA comment in Save() above
    FRESULT    fr = f_open(&file, fname, FA_READ);
    if(fr != FR_OK)
    {
        SetError("open", fr);
        return false;
    }

    static FileHeader hdr;
    hdr = FileHeader{};
    UINT br;
    fr        = f_read(&file, &hdr, sizeof(hdr), &br);
    bool ok   = fr == FR_OK && br == sizeof(hdr);
    if(ok && (memcmp(hdr.magic, "OURO", 4) != 0 || hdr.version != kFileVersion))
    {
        ok = false;
        SetError("magic", FR_OK); // not a FatFS error -- file content itself is wrong
    }
    else if(!ok)
    {
        SetError("hdr", fr);
    }

    if(ok)
    {
        // Layers must be cleared (and the tempo unlocked) before BPM/Bars
        // can change -- see TempoClock::SetBpm()/SetBars(). Clearing here
        // also discards whatever was in memory before the load, which is
        // the intended "Load replaces the current in-memory performance"
        // behaviour (the file on disk for whatever was loaded/saved
        // before is untouched either way).
        tempo.Unlock();
        for(int i = 0; i < num_layers; i++)
            layers[i].Clear();

        tempo.SetBpm(hdr.bpm);
        tempo.SetBars(hdr.bars);
        tempo.SetMetronomeEnabled(hdr.metronome_enabled != 0);
        tempo.SetMetronomeVolume01(hdr.metronome_vol01);
        // Every restored layer's play_pos_ starts at 0 (see
        // RestoreRecordedLength() below) -- the tempo's own phase needs
        // the same "sample 0 of bar 1" reset, or the metronome/beat
        // indicator resumes wherever it happened to be instead of
        // aligned with the newly loaded audio. Caller (Ui::TriggerLoad())
        // keeps the audio engine suspended across this whole call so
        // nothing advances either phase in between.
        tempo.ResetPhase();

        *out_master_volume01        = hdr.master_volume01;
        *out_bypass                  = hdr.bypass != 0;
        *out_master_filter_mode      = (FilterMode)hdr.master_filter_mode;
        *out_master_filter_cutoff01  = hdr.master_filter_cutoff01;
        *out_master_filter_res01     = hdr.master_filter_res01;
        *out_reverb_size01           = hdr.reverb_size01;
        *out_bypass_reverb_send01    = hdr.bypass_reverb_send01;
    }

    int n = ok ? (int)hdr.num_layers : 0;
    if(n > num_layers)
        n = num_layers; // tolerate a saved file with fewer/more layers than we have now

    // Total sample-halves across all layers actually being restored, read
    // up front from each layer's header for progress reporting.
    size_t total_units = 0;
    size_t done_units  = 0;

    for(int i = 0; ok && i < n; i++)
    {
        static LayerHeader lh; // see the DTCMRAM/DMA comment in Save() above
        lh = LayerHeader{};
        fr = f_read(&file, &lh, sizeof(lh), &br);
        ok = fr == FR_OK && br == sizeof(lh);
        if(!ok)
        {
            char step[8];
            snprintf(step, sizeof(step), "lhdr%d", i);
            SetError(step, fr);
            break;
        }

        LooperLayer& layer = layers[i];
        layer.SetVolume01(lh.volume01);
        layer.SetPan01(lh.pan01);
        layer.SetSpeed01(lh.speed01);
        layer.SetInputGain01(lh.input_gain01);
        layer.SetFilterMode((FilterMode)lh.filter_mode);
        layer.SetFilterCutoff01(lh.filter_cutoff01);
        layer.SetFilterResonance01(lh.filter_res01);
        layer.SetEffect((LayerEffect)lh.effect);
        layer.SetEffectParamA01(lh.effect_param_a01);
        layer.SetEffectParamB01(lh.effect_param_b01);
        layer.SetReverbSend01(lh.reverb_send01);

        size_t len = lh.record_len;
        if(len > layer.GetBufferSize())
            len = layer.GetBufferSize(); // guard against a corrupt/foreign file

        total_units += len * 2;

        if(len > 0)
        {
            float* dsts[2] = {layer.GetBufferL(), layer.GetBufferR()};
            for(int ch = 0; ok && ch < 2; ch++)
            {
                size_t got = 0;
                while(ok && got < len)
                {
                    UINT n_samp = (UINT)((len - got) < kChunkSamples ? (len - got)
                                                                       : kChunkSamples);
                    UINT bytes = n_samp * sizeof(float);
                    fr = f_read(&file, dsts[ch] + got, bytes, &br);
                    ok = fr == FR_OK && br == bytes;
                    if(!ok)
                    {
                        char step[8];
                        snprintf(step, sizeof(step), "aud%d%d", i, ch);
                        SetError(step, fr);
                        break;
                    }
                    got += n_samp;
                    done_units += n_samp;
                    if(on_progress && total_units > 0)
                        on_progress((float)done_units / (float)total_units);
                }
            }
        }
        layer.RestoreRecordedLength(len);
    }

    f_close(&file);
    return ok;
}

namespace
{
// Header + one raw GranularPresetData block + audio_len samples each of
// L then R (float32, chunked -- see kChunkSamples), same shape as Save()'s
// own per-layer audio. audio_len lives in the header (not implied by file
// size alone) so a short/empty capture doesn't need padding to a fixed
// size.
struct GranularPresetFileHeader
{
    char     magic[4]; // "GRNP"
    uint32_t version;
    uint32_t audio_len;
};
constexpr uint32_t kGranularPresetFileVersion = 3; // bumped again: Scan split into 2 fields

// Same shape as GranularPresetFileHeader above, own magic/version. No
// audio, so no audio_len field -- just the header plus one raw
// DexedPresetData block, a single small f_write()/f_read() like the
// removed Fm/Pad presets used.
struct DexedPresetFileHeader
{
    char     magic[4]; // "DEXP"
    uint32_t version;
};
constexpr uint32_t kDexedPresetFileVersion = 1;
} // namespace

bool SaveGranularPreset(int                                       slot,
                        const GranularEngine::GranularPresetData& preset,
                        const float*                              audio_l,
                        const float*                              audio_r,
                        size_t                                    audio_len,
                        ProgressFn                                on_progress)
{
    if(!card_ready || slot < 1 || slot > kMaxGranularPresets)
        return false;

    char fname[24];
    GranularPresetFilename(slot, fname, sizeof(fname));

    FRESULT mkdir_res = f_mkdir("GRNP");
    if(mkdir_res != FR_OK && mkdir_res != FR_EXIST)
    {
        SetError("mkdir", mkdir_res);
        return false;
    }

    ClearError();
    static FIL file; // see the DTCMRAM/DMA comment on Save() above
    FRESULT    fr = f_open(&file, fname, FA_CREATE_ALWAYS | FA_WRITE);
    if(fr != FR_OK)
    {
        SetError("open", fr);
        return false;
    }

    static GranularPresetFileHeader hdr;
    hdr = GranularPresetFileHeader{};
    memcpy(hdr.magic, "GRNP", 4);
    hdr.version   = kGranularPresetFileVersion;
    hdr.audio_len = (uint32_t)audio_len;

    static GranularEngine::GranularPresetData data;
    data = preset;

    UINT bw;
    fr      = f_write(&file, &hdr, sizeof(hdr), &bw);
    bool ok = fr == FR_OK && bw == sizeof(hdr);
    if(ok)
    {
        fr = f_write(&file, &data, sizeof(data), &bw);
        ok = fr == FR_OK && bw == sizeof(data);
    }
    if(!ok)
        SetError("hdr", fr);

    size_t total_units = audio_len * 2;
    size_t done_units  = 0;
    const float* srcs[2] = {audio_l, audio_r};
    for(int ch = 0; ok && ch < 2; ch++)
    {
        size_t written = 0;
        while(ok && written < audio_len)
        {
            UINT n = (UINT)((audio_len - written) < kChunkSamples ? (audio_len - written)
                                                                    : kChunkSamples);
            UINT bytes = n * sizeof(float);
            fr = f_write(&file, srcs[ch] + written, bytes, &bw);
            ok = fr == FR_OK && bw == bytes;
            if(!ok)
            {
                char step[8];
                snprintf(step, sizeof(step), "aud%d", ch);
                SetError(step, fr);
                break;
            }
            written += n;
            done_units += n;
            if(on_progress && total_units > 0)
                on_progress((float)done_units / (float)total_units);
        }
    }

    FRESULT close_res = f_close(&file);
    if(close_res != FR_OK)
        SetError("close", close_res);
    return ok && close_res == FR_OK;
}

bool LoadGranularPreset(int                                  slot,
                        GranularEngine::GranularPresetData* out_preset,
                        float*                               out_audio_l,
                        float*                               out_audio_r,
                        size_t                                audio_capacity,
                        size_t*                               out_audio_len,
                        ProgressFn                            on_progress)
{
    if(!card_ready || slot < 1 || slot > kMaxGranularPresets || !out_preset || !out_audio_len)
        return false;

    char fname[24];
    GranularPresetFilename(slot, fname, sizeof(fname));

    ClearError();
    static FIL file; // see the DTCMRAM/DMA comment on Save() above
    FRESULT    fr = f_open(&file, fname, FA_READ);
    if(fr != FR_OK)
    {
        SetError("open", fr);
        return false;
    }

    static GranularPresetFileHeader hdr;
    hdr = GranularPresetFileHeader{};
    UINT br;
    fr      = f_read(&file, &hdr, sizeof(hdr), &br);
    bool ok = fr == FR_OK && br == sizeof(hdr);
    if(ok && (memcmp(hdr.magic, "GRNP", 4) != 0 || hdr.version != kGranularPresetFileVersion))
    {
        ok = false;
        SetError("magic", FR_OK); // not a FatFS error -- file content itself is wrong
    }
    else if(!ok)
    {
        SetError("hdr", fr);
    }

    static GranularEngine::GranularPresetData data;
    data = GranularEngine::GranularPresetData{};
    if(ok)
    {
        fr = f_read(&file, &data, sizeof(data), &br);
        ok = fr == FR_OK && br == sizeof(data);
        if(!ok)
            SetError("data", fr);
    }
    if(ok)
        *out_preset = data;

    size_t len = ok ? (size_t)hdr.audio_len : 0;
    if(len > audio_capacity)
        len = audio_capacity; // never write past what the caller actually owns
    *out_audio_len = len;

    size_t total_units = len * 2;
    size_t done_units  = 0;
    float* dsts[2]       = {out_audio_l, out_audio_r};
    for(int ch = 0; ok && ch < 2; ch++)
    {
        size_t got = 0;
        while(ok && got < len)
        {
            UINT n_samp = (UINT)((len - got) < kChunkSamples ? (len - got) : kChunkSamples);
            UINT bytes  = n_samp * sizeof(float);
            fr = f_read(&file, dsts[ch] + got, bytes, &br);
            ok = fr == FR_OK && br == bytes;
            if(!ok)
            {
                char step[8];
                snprintf(step, sizeof(step), "aud%d", ch);
                SetError(step, fr);
                break;
            }
            got += n_samp;
            done_units += n_samp;
            if(on_progress && total_units > 0)
                on_progress((float)done_units / (float)total_units);
        }
    }

    FRESULT close_res = f_close(&file);
    if(close_res != FR_OK)
        SetError("close", close_res);
    return ok && close_res == FR_OK;
}

int ListGranularPresets(int* out_numbers, int max_out)
{
    if(!card_ready)
        return 0;
    int count = 0;
    for(int slot = 1; slot <= kMaxGranularPresets && count < max_out; slot++)
    {
        char    fname[24];
        FILINFO fno;
        GranularPresetFilename(slot, fname, sizeof(fname));
        if(f_stat(fname, &fno) == FR_OK)
            out_numbers[count++] = slot;
    }
    return count;
}

int NextFreeGranularPresetSlot()
{
    if(!card_ready)
        return -1;
    for(int slot = 1; slot <= kMaxGranularPresets; slot++)
    {
        char    fname[24];
        FILINFO fno;
        GranularPresetFilename(slot, fname, sizeof(fname));
        if(f_stat(fname, &fno) != FR_OK)
            return slot;
    }
    return -1;
}

bool DeleteGranularPreset(int slot, int* loaded_slot_inout)
{
    if(!card_ready || slot < 1 || slot > kMaxGranularPresets)
        return false;
    char fname[24];
    GranularPresetFilename(slot, fname, sizeof(fname));
    if(!DeleteFileGeneric(fname))
        return false;
    if(loaded_slot_inout && *loaded_slot_inout == slot)
        *loaded_slot_inout = 0;
    CompactSlotsAfterDelete(slot, kMaxGranularPresets, GranularPresetFilename, loaded_slot_inout);
    return true;
}

bool DuplicateGranularPreset(int slot, int* out_new_slot, ProgressFn on_progress)
{
    if(!out_new_slot)
        return false;
    *out_new_slot = -1;
    if(!card_ready || slot < 1 || slot > kMaxGranularPresets)
        return false;
    char    src[24];
    FILINFO fno;
    GranularPresetFilename(slot, src, sizeof(src));
    if(f_stat(src, &fno) != FR_OK)
    {
        SetError("dupsrc", FR_NO_FILE);
        return false;
    }
    int new_slot = NextFreeGranularPresetSlot();
    if(new_slot < 0)
    {
        SetError("dupslot", FR_OK);
        return false;
    }
    char dst[24];
    GranularPresetFilename(new_slot, dst, sizeof(dst));
    if(!CopyFileChunked(src, dst, on_progress))
        return false;
    *out_new_slot = new_slot;
    return true;
}

bool SaveDexedPreset(int slot, const DexedSynth::DexedPresetData& preset)
{
    if(!card_ready || slot <= DexedSynth::GetNumFactoryPresets() || slot > kMaxDexedPresets)
        return false;

    char fname[24];
    DexedPresetFilename(slot, fname, sizeof(fname));

    FRESULT mkdir_res = f_mkdir("DEXP");
    if(mkdir_res != FR_OK && mkdir_res != FR_EXIST)
    {
        SetError("mkdir", mkdir_res);
        return false;
    }

    ClearError();
    static FIL file; // see the DTCMRAM/DMA comment on Save() above
    FRESULT    fr = f_open(&file, fname, FA_CREATE_ALWAYS | FA_WRITE);
    if(fr != FR_OK)
    {
        SetError("open", fr);
        return false;
    }

    static DexedPresetFileHeader hdr;
    hdr = DexedPresetFileHeader{};
    memcpy(hdr.magic, "DEXP", 4);
    hdr.version = kDexedPresetFileVersion;

    static DexedSynth::DexedPresetData data;
    data = preset;

    UINT bw;
    fr      = f_write(&file, &hdr, sizeof(hdr), &bw);
    bool ok = fr == FR_OK && bw == sizeof(hdr);
    if(ok)
    {
        fr = f_write(&file, &data, sizeof(data), &bw);
        ok = fr == FR_OK && bw == sizeof(data);
    }
    if(!ok)
        SetError("data", fr);

    FRESULT close_res = f_close(&file);
    if(close_res != FR_OK)
        SetError("close", close_res);
    return ok && close_res == FR_OK;
}

bool LoadDexedPreset(int slot, DexedSynth::DexedPresetData* out_preset)
{
    if(slot < 1 || slot > kMaxDexedPresets || !out_preset)
        return false;
    if(slot <= DexedSynth::GetNumFactoryPresets())
    {
        *out_preset = DexedSynth::GetFactoryPreset(slot - 1);
        return true;
    }
    if(!card_ready)
        return false;

    char fname[24];
    DexedPresetFilename(slot, fname, sizeof(fname));

    ClearError();
    static FIL file;
    FRESULT    fr = f_open(&file, fname, FA_READ);
    if(fr != FR_OK)
    {
        SetError("open", fr);
        return false;
    }

    static DexedPresetFileHeader hdr;
    hdr = DexedPresetFileHeader{};
    UINT br;
    fr      = f_read(&file, &hdr, sizeof(hdr), &br);
    bool ok = fr == FR_OK && br == sizeof(hdr);
    if(ok && (memcmp(hdr.magic, "DEXP", 4) != 0 || hdr.version != kDexedPresetFileVersion))
    {
        ok = false;
        SetError("magic", FR_OK); // not a FatFS error -- file content itself is wrong
    }
    else if(!ok)
    {
        SetError("hdr", fr);
    }

    static DexedSynth::DexedPresetData data;
    data = DexedSynth::DexedPresetData{};
    if(ok)
    {
        fr = f_read(&file, &data, sizeof(data), &br);
        ok = fr == FR_OK && br == sizeof(data);
        if(!ok)
            SetError("data", fr);
    }
    if(ok)
        *out_preset = data;

    FRESULT close_res = f_close(&file);
    if(close_res != FR_OK)
        SetError("close", close_res);
    return ok && close_res == FR_OK;
}

int ListDexedPresets(int* out_numbers, int max_out)
{
    if(!card_ready)
        return 0;
    int count = 0;
    for(int slot = DexedSynth::GetNumFactoryPresets() + 1;
        slot <= kMaxDexedPresets && count < max_out; slot++)
    {
        char    fname[24];
        FILINFO fno;
        DexedPresetFilename(slot, fname, sizeof(fname));
        if(f_stat(fname, &fno) == FR_OK)
            out_numbers[count++] = slot;
    }
    return count;
}

int NextFreeDexedPresetSlot()
{
    if(!card_ready)
        return -1;
    for(int slot = DexedSynth::GetNumFactoryPresets() + 1; slot <= kMaxDexedPresets; slot++)
    {
        char    fname[24];
        FILINFO fno;
        DexedPresetFilename(slot, fname, sizeof(fname));
        if(f_stat(fname, &fno) != FR_OK)
            return slot;
    }
    return -1;
}

bool DeleteDexedPreset(int slot, int* loaded_slot_inout)
{
    if(!card_ready || slot <= DexedSynth::GetNumFactoryPresets() || slot > kMaxDexedPresets)
        return false;
    char fname[24];
    DexedPresetFilename(slot, fname, sizeof(fname));
    if(!DeleteFileGeneric(fname))
        return false;
    if(loaded_slot_inout && *loaded_slot_inout == slot)
        *loaded_slot_inout = 0;
    CompactSlotsAfterDelete(slot, kMaxDexedPresets, DexedPresetFilename, loaded_slot_inout);
    return true;
}

bool DuplicateDexedPreset(int slot, int* out_new_slot, ProgressFn on_progress)
{
    if(!out_new_slot)
        return false;
    *out_new_slot = -1;
    if(!card_ready || slot <= DexedSynth::GetNumFactoryPresets() || slot > kMaxDexedPresets)
        return false;
    char    src[24];
    FILINFO fno;
    DexedPresetFilename(slot, src, sizeof(src));
    if(f_stat(src, &fno) != FR_OK)
    {
        SetError("dupsrc", FR_NO_FILE);
        return false;
    }
    int new_slot = NextFreeDexedPresetSlot();
    if(new_slot < 0)
    {
        SetError("dupslot", FR_OK);
        return false;
    }
    char dst[24];
    DexedPresetFilename(new_slot, dst, sizeof(dst));
    if(!CopyFileChunked(src, dst, on_progress))
        return false;
    *out_new_slot = new_slot;
    return true;
}

int ListImportWavFiles(char out_names[][kMaxImportWavNameLen + 1], int max_out)
{
    if(!card_ready)
        return 0;
    static DIR dir; // see the DTCMRAM/DMA comment on Save() above
    if(f_opendir(&dir, "IMPORT") != FR_OK)
        return 0; // no IMPORT/ folder yet -- not an error, just nothing to list

    int            count = 0;
    static FILINFO fno; // see the DTCMRAM/DMA comment on Save() above
    while(count < max_out)
    {
        if(f_readdir(&dir, &fno) != FR_OK || fno.fname[0] == 0)
            break; // error or end of directory
        if(fno.fattrib & AM_DIR)
            continue;
        if(fno.fname[0] == '.' && fno.fname[1] == '_')
            continue; // macOS AppleDouble sidecar file, not a real WAV -- see this function's doc comment
        size_t len = strlen(fno.fname);
        if(len < 4)
            continue; // too short for a ".wav" extension
        char ext[5];
        for(int i = 0; i < 4; i++)
            ext[i] = (char)tolower((unsigned char)fno.fname[len - 4 + i]);
        ext[4] = '\0';
        if(strcmp(ext, ".wav") != 0)
            continue;
        if(len > kMaxImportWavNameLen)
            continue; // skip, don't truncate -- see this function's doc comment
        snprintf(out_names[count], kMaxImportWavNameLen + 1, "%s", fno.fname);
        count++;
    }
    f_closedir(&dir);
    return count;
}

bool ImportWav(const char* filename, float* out_l, float* out_r, size_t audio_capacity,
               size_t* out_len, ProgressFn on_progress)
{
    if(!card_ready || !filename || !out_l || !out_r || !out_len)
        return false;
    *out_len = 0;

    char fname[64];
    ImportWavFilename(filename, fname, sizeof(fname));

    ClearError();
    static FIL file; // see the DTCMRAM/DMA comment on Save() above
    FRESULT    fr = f_open(&file, fname, FA_READ);
    if(fr != FR_OK)
    {
        SetError("open", fr);
        return false;
    }

    // -- RIFF/WAVE header -------------------------------------------------
    // All of these are `static` (see the DTCMRAM/DMA comment on Save()
    // above) -- f_read() into a plain stack-local here silently fails
    // the DMA transfer (stack lives in DTCMRAM, unreachable by SDMMC's
    // DMA), leaving the "destination" full of stale/garbage stack
    // contents while FRESULT still reports FR_OK -- exactly what was
    // happening here before this fix (a real file always failed the
    // very first magic-byte check with a misleadingly benign "riff:0").
    UINT br;
    static char     riff_id[4], wave_id[4];
    static uint32_t riff_size_unused;
    bool ok = true;
    fr = f_read(&file, riff_id, 4, &br);
    ok = fr == FR_OK && br == 4 && memcmp(riff_id, "RIFF", 4) == 0;
    if(ok)
    {
        fr = f_read(&file, &riff_size_unused, 4, &br);
        ok = fr == FR_OK && br == 4;
    }
    if(ok)
    {
        fr = f_read(&file, wave_id, 4, &br);
        ok = fr == FR_OK && br == 4 && memcmp(wave_id, "WAVE", 4) == 0;
    }
    if(!ok)
    {
        SetError("riff", fr);
        f_close(&file);
        return false;
    }

    // -- Walk chunks until fmt and data are both found -- unlike this
    // firmware's own fixed 44-byte WavHeader (used for ExportWav(), which
    // controls the exact layout it writes), a file from an arbitrary
    // computer/DAW can have extra metadata chunks (LIST/INFO/etc.) or the
    // chunks in a different order, so this walks generically instead of
    // assuming a fixed layout. --------------------------------------------
    bool     have_fmt = false, have_data = false;
    uint16_t audio_format = 0, num_channels = 0, bits_per_sample = 0;
    uint32_t sample_rate = 0, data_size = 0, data_start_offset = 0;
    static char     chunk_id[4]; // static -- see the DTCMRAM/DMA comment above
    static uint32_t chunk_size;
    while(ok && !(have_fmt && have_data))
    {
        fr = f_read(&file, chunk_id, 4, &br);
        if(fr != FR_OK || br != 4)
            break;
        fr = f_read(&file, &chunk_size, 4, &br);
        if(fr != FR_OK || br != 4)
            break;

        if(memcmp(chunk_id, "fmt ", 4) == 0)
        {
            static uint8_t fmt_buf[16]; // static -- see the DTCMRAM/DMA comment above
            if(chunk_size < 16)
            {
                ok = false;
                break;
            }
            fr = f_read(&file, fmt_buf, sizeof(fmt_buf), &br);
            if(fr != FR_OK || br != sizeof(fmt_buf))
            {
                ok = false;
                break;
            }
            memcpy(&audio_format, fmt_buf + 0, 2);
            memcpy(&num_channels, fmt_buf + 2, 2);
            memcpy(&sample_rate, fmt_buf + 4, 4);
            memcpy(&bits_per_sample, fmt_buf + 14, 2);
            // Skip any remaining bytes of an extended fmt chunk (word-
            // aligned, same as every other chunk here).
            uint32_t remaining = chunk_size - 16 + ((chunk_size - 16) & 1);
            if(remaining > 0)
                f_lseek(&file, f_tell(&file) + remaining);
            have_fmt = true;
        }
        else if(memcmp(chunk_id, "data", 4) == 0)
        {
            data_size         = chunk_size;
            data_start_offset = (uint32_t)f_tell(&file);
            have_data         = true;
            // Skipped like any other chunk (unlike an earlier version of
            // this function, which left the file pointer sitting here
            // assuming fmt always comes first) -- data_start_offset is
            // what the streaming loop below actually seeks back to once
            // the walk finishes, so this works whether fmt comes before
            // or after data in the file.
            uint32_t skip = chunk_size + (chunk_size & 1);
            fr             = f_lseek(&file, f_tell(&file) + skip);
            if(fr != FR_OK)
                break;
        }
        else
        {
            uint32_t skip = chunk_size + (chunk_size & 1); // word-aligned
            fr             = f_lseek(&file, f_tell(&file) + skip);
            if(fr != FR_OK)
                break;
        }
    }

    if(!ok || !have_fmt || !have_data)
    {
        SetError("chunks", fr);
        f_close(&file);
        return false;
    }
    // PCM at 16/24/32-bit, or 32-bit IEEE float (audio_format 3 -- what
    // several DAWs/export tools default to) -- widened from 16-bit-only
    // after a real user report: a commercial sample-pack loop (24-bit,
    // otherwise perfectly ordinary stereo/44.1kHz) failed to import with
    // no path to fix it short of re-encoding on a computer first. Real
    // sample rate conversion beyond the existing 44.1->48kHz case isn't
    // attempted -- that's a separate, much bigger problem (arbitrary
    // resampling) than "this bit depth/format isn't decoded yet".
    bool format_ok = (audio_format == 1 && (bits_per_sample == 16 || bits_per_sample == 24
                                              || bits_per_sample == 32))
                      || (audio_format == 3 && bits_per_sample == 32);
    if(!format_ok || (num_channels != 1 && num_channels != 2)
       || (sample_rate != 48000 && sample_rate != 44100))
    {
        // Not a FatFS error -- the file itself is a format this importer
        // doesn't handle (see ImportWav()'s own doc comment for exactly
        // which formats are accepted).
        SetError("format", FR_OK);
        f_close(&file);
        return false;
    }

    // -- Stream-convert the data chunk -------------------------------
    // Seek back to the data payload's own start -- the chunk walk above
    // always skips past it (see the "data" branch's own comment), so
    // this is needed regardless of whether fmt came before or after it.
    fr = f_lseek(&file, data_start_offset);
    if(fr != FR_OK)
    {
        SetError("seek", fr);
        f_close(&file);
        return false;
    }

    // Some tools (streamed/live-bounced exports in particular) never go
    // back and patch the "data" chunk's declared size, leaving it as a
    // placeholder that's larger than what's actually there -- trusting
    // it as-is would read straight past the real audio into whatever
    // bytes happen to follow on disk (another chunk's raw data, or
    // undefined trailing bytes) as if they were more PCM, which plays
    // back as noise/crackle right where the real audio ends rather than
    // a clean stop. Clamp to what the file can actually provide instead.
    uint32_t bytes_left_in_file = (uint32_t)f_size(&file) - data_start_offset;
    if(data_size > bytes_left_in_file)
        data_size = bytes_left_in_file;

    bool   need_resample = sample_rate == 44100;
    static Resampler44_1to48 resampler; // see the DTCMRAM/DMA comment on Save() above
    resampler = Resampler44_1to48{};

    // Raw bytes, not int16_t -- unlike the 16-bit-only version of this
    // function, the sample width varies by file (2/3/4 bytes), so this
    // reads and decodes generically instead of assuming int16_t layout.
    // Sized for the worst case (stereo, 4 bytes/sample).
    static uint8_t raw_buf[kChunkSamples * 2 * 4];
    uint32_t       bytes_per_sample = (uint32_t)bits_per_sample / 8;
    uint32_t       bytes_per_frame  = (uint32_t)num_channels * bytes_per_sample;
    uint32_t       total_frames     = data_size / bytes_per_frame;
    uint32_t       frames_done      = 0;
    size_t         written          = 0;

    while(ok && frames_done < total_frames && written < audio_capacity)
    {
        UINT frames_this_chunk = (UINT)((total_frames - frames_done) < kChunkSamples
                                             ? (total_frames - frames_done)
                                             : kChunkSamples);
        UINT bytes_this_chunk  = frames_this_chunk * bytes_per_frame;
        fr = f_read(&file, raw_buf, bytes_this_chunk, &br);
        ok = fr == FR_OK && br == bytes_this_chunk;
        if(!ok)
        {
            SetError("data", fr);
            break;
        }

        // Decodes one little-endian sample starting at byte offset
        // `off` in raw_buf, to a -1..1 float, based on this stream's own
        // (constant for the whole file) audio_format/bits_per_sample.
        // 24-bit has no native integer type, so it's reconstructed by
        // hand: shift the 3 bytes into the TOP 24 bits of a 32-bit int,
        // then arithmetic-shift back down by 8 -- sign-extends and
        // rescales into the same numeric range a native 24-bit type
        // would have, in one step.
        auto decode_sample = [&](UINT off) -> float {
            if(audio_format == 3) // 32-bit IEEE float
            {
                float f;
                memcpy(&f, raw_buf + off, 4);
                return f;
            }
            if(bits_per_sample == 24)
            {
                int32_t v = (int32_t)((uint32_t)raw_buf[off] << 8
                                        | (uint32_t)raw_buf[off + 1] << 16
                                        | (uint32_t)raw_buf[off + 2] << 24);
                return (float)(v >> 8) / 8388608.f; // 2^23
            }
            if(bits_per_sample == 32) // 32-bit PCM int
            {
                int32_t v;
                memcpy(&v, raw_buf + off, 4);
                return (float)v / 2147483648.f; // 2^31
            }
            int16_t v; // 16-bit PCM int
            memcpy(&v, raw_buf + off, 2);
            return (float)v / 32768.f; // 2^15
        };

        for(UINT i = 0; i < frames_this_chunk && written < audio_capacity; i++)
        {
            float l, r;
            if(num_channels == 1)
            {
                l = r = decode_sample(i * bytes_per_sample);
            }
            else
            {
                l = decode_sample(i * bytes_per_frame);
                r = decode_sample(i * bytes_per_frame + bytes_per_sample);
            }

            if(!need_resample)
            {
                out_l[written] = l;
                out_r[written] = r;
                written++;
            }
            else
            {
                float rl[2], rr[2];
                int   n = resampler.Push(l, r, rl, rr);
                for(int k = 0; k < n && written < audio_capacity; k++)
                {
                    out_l[written] = rl[k];
                    out_r[written] = rr[k];
                    written++;
                }
            }
        }

        frames_done += frames_this_chunk;
        if(on_progress && total_frames > 0)
            on_progress((float)frames_done / (float)total_frames);
    }

    FRESULT close_res = f_close(&file);
    if(close_res != FR_OK && ok)
        SetError("close", close_res);
    if(ok)
    {
        // Downward-only peak normalization -- a WAV authored/mastered on a
        // computer is often close to 0dBFS, whereas nothing else Grains
        // can capture (Direct Record off the pod's own audio-in, or a
        // loop layer's already-mixed audio) tends to sit anywhere near
        // that hot, so an unscaled import plays back jarringly loud next
        // to those. Only ever scales down (never boosts a quiet file) --
        // this is loudness-matching against the other capture sources,
        // not a general "make everything as loud as possible" normalizer.
        constexpr float kTargetPeak = 0.9f;
        float           peak        = 0.f;
        for(size_t i = 0; i < written; i++)
        {
            float a = fabsf(out_l[i]);
            if(a > peak)
                peak = a;
            a = fabsf(out_r[i]);
            if(a > peak)
                peak = a;
        }
        if(peak > kTargetPeak)
        {
            float scale = kTargetPeak / peak;
            for(size_t i = 0; i < written; i++)
            {
                out_l[i] *= scale;
                out_r[i] *= scale;
            }
        }
        *out_len = written;
    }
    return ok && close_res == FR_OK;
}

bool ExportWav(TempoClock&  tempo,
              LooperLayer* layers,
              int          num_layers,
              FilterMode   master_filter_mode,
              float        master_filter_cutoff01,
              float        master_filter_res01,
              float        reverb_size01,
              bool         for_microdexed,
              float        project_speed,
              ProgressFn   on_progress)
{
    if(!card_ready)
        return false;

    ClearError();

    // Refuse mid-take: the Recording branch in LooperLayer::Process() is
    // gated only on state_, not on the (all-zero, silent) ticks this
    // offline render feeds it, so calling Process() on a Recording layer
    // here would overwrite real in-progress audio with silence. Also
    // refuse ArmedCountIn (nothing useful to export yet from that layer)
    // and an entirely-empty performance.
    bool any_content = false;
    for(int i = 0; i < num_layers; i++)
    {
        LayerState s = layers[i].GetState();
        if(s == LayerState::Recording || s == LayerState::ArmedCountIn)
        {
            SetError("busy", FR_OK);
            return false;
        }
        if(layers[i].HasContent())
            any_content = true;
    }
    if(!any_content)
    {
        SetError("empty", FR_OK);
        return false;
    }

    FRESULT mkdir_res = f_mkdir(for_microdexed ? "custom" : "WAV");
    if(mkdir_res != FR_OK && mkdir_res != FR_EXIST)
    {
        SetError("mkdir", mkdir_res);
        return false;
    }

    int num = NextFreeExportNumber(for_microdexed);
    if(num < 0)
    {
        SetError("full", FR_OK);
        return false;
    }
    char fname[24];
    ExportFilename(for_microdexed, num, fname, sizeof(fname));

    static FIL file; // see the DTCMRAM/DMA comment in Save() above
    FRESULT    fr = f_open(&file, fname, FA_CREATE_ALWAYS | FA_WRITE);
    if(fr != FR_OK)
    {
        SetError("open", fr);
        return false;
    }

    // sample_rate/total_samples stay at the true native rate -- they
    // drive mfilt_l/r.Init(), export_reverb.Init(), and nyquist_guard
    // below, all of which must keep running at the Pod's real 48kHz
    // regardless of export mode or vari-speed (only the read position and
    // the final written samples are affected by either -- see below).
    // render_len is the number of per-sample render ticks needed for one
    // full pass through the loop AT project_speed (unlike master volume,
    // vari-speed IS captured into exports, on purpose -- see this
    // function's header comment): at project_speed>1 a full pass
    // completes in fewer real-time-domain ticks (faster, shorter, higher
    // pitched), at <1 it takes more (slower, longer, lower). out_frames
    // is what actually goes in the WAV header/gets written -- render_len
    // for the Studio path, or that same count further resampled to
    // 44.1kHz for the CD path.
    const uint32_t sample_rate     = (uint32_t)tempo.GetSampleRate();
    const size_t   total_samples   = tempo.GetLoopLengthSamples();
    const size_t   render_len      = project_speed > 0.0001f
                                          ? (size_t)((float)total_samples / project_speed + 0.5f)
                                          : total_samples;
    const uint32_t out_sample_rate = for_microdexed ? 44100u : sample_rate;
    const uint32_t out_frames      = for_microdexed
                                          ? Resampler48to44_1::OutputFrames(render_len)
                                          : (uint32_t)render_len;
    const uint32_t data_size       = out_frames * 2 * (uint32_t)sizeof(int16_t);

    static WavHeader hdr;
    memcpy(hdr.riff, "RIFF", 4);
    hdr.riff_size = 36 + data_size;
    memcpy(hdr.wave, "WAVE", 4);
    memcpy(hdr.fmt, "fmt ", 4);
    hdr.fmt_size        = 16;
    hdr.audio_format    = 1; // PCM
    hdr.num_channels    = 2;
    hdr.sample_rate     = out_sample_rate;
    hdr.bits_per_sample = 16;
    hdr.block_align     = (uint16_t)(hdr.num_channels * (hdr.bits_per_sample / 8));
    hdr.byte_rate       = hdr.sample_rate * hdr.block_align;
    memcpy(hdr.data, "data", 4);
    hdr.data_size = data_size;

    UINT bw;
    fr      = f_write(&file, &hdr, sizeof(hdr), &bw);
    bool ok = fr == FR_OK && bw == sizeof(hdr);
    if(!ok)
        SetError("hdr", fr);

    // Snapshot every layer's real play_pos_ so live playback can resume
    // exactly where it was after this function returns (success or not
    // -- see the restore at the bottom, which always runs).
    int   n_snap = num_layers < kMaxExportLayers ? num_layers : kMaxExportLayers;
    static float saved_pos[kMaxExportLayers];
    for(int i = 0; i < n_snap; i++)
        saved_pos[i] = layers[i].GetPlayPosRaw();

    // Local master filter -- a fresh instance rather than main.cpp's live
    // fx_master_filter_l/r, since its only state is short-term signal
    // history (not a delay line/reverb tail like the per-layer effects
    // reused below), so a locally primed instance sounds the same as the
    // live one without plumbing a new cross-module pointer through Ui.
    // Same cutoff curve main.cpp already applies to the live master
    // filter (kFilterMinHz/kFilterMaxHz from looper_layer.h).
    daisysp::Svf mfilt_l, mfilt_r;
    mfilt_l.Init((float)sample_rate);
    mfilt_r.Init((float)sample_rate);
    float mfilt_cutoff
        = kFilterMinHz * powf(kFilterMaxHz / kFilterMinHz, master_filter_cutoff01);
    float nyquist_guard = (float)sample_rate / 3.f - 1.f;
    mfilt_cutoff         = Clampf(mfilt_cutoff, kFilterMinHz, nyquist_guard);
    float mfilt_res      = Clampf(master_filter_res01, 0.f, 1.f) * 0.9f;
    mfilt_l.SetFreq(mfilt_cutoff);
    mfilt_l.SetRes(mfilt_res);
    mfilt_r.SetFreq(mfilt_cutoff);
    mfilt_r.SetRes(mfilt_res);

    // Local shared reverb bus, mirroring main.cpp's fx_reverb_shared --
    // every layer's Send-scaled signal (see LooperLayer::Process()'s
    // reverb_send_out parameter) is summed and run through this ONE
    // instance, same as live playback. `static` (not stack-local) AND
    // SDRAM-placed like main.cpp's, since it's the same ~386KB object --
    // this project's stack lives in DTCMRAM (128K total), nowhere near
    // enough room. Primed by pass 0 below same as the master filter.
    // Zeroed before every Init() call, same reasoning as main.cpp's
    // fx_reverb_shared and LooperLayer::Init()'s fx_phaser:
    // .sdram_bss isn't zero-initialized by startup code, so this object's
    // memory is raw leftover contents the first time this runs, and
    // (being `static`) whatever a previous export call left behind on
    // every call after that -- re-zeroing each time keeps every export
    // starting from the same clean state.
    static daisysp::ReverbSc DSY_SDRAM_BSS export_reverb;
    memset(&export_reverb, 0, sizeof(export_reverb));
    export_reverb.Init((float)sample_rate);
    export_reverb.SetLpFreq(9000.f); // fixed damping, matches the live default
    export_reverb.SetFeedback(Clampf(reverb_size01, 0.f, 1.f));

    // Dummy input/ticks for the offline Process() calls below -- a
    // Playing-state layer never reads either (confirmed against
    // LooperLayer::Process()'s Playing/Paused/Overdubbing block), so an
    // all-zero, never-written static array is safe to reuse every chunk.
    static float                 dummy_in_l[kChunkSamples] = {};
    static float                 dummy_in_r[kChunkSamples] = {};
    const float*                 in_ptrs[2] = {dummy_in_l, dummy_in_r};
    static TempoClock::TempoTick dummy_ticks[kChunkSamples] = {};

    static float   chunk_l[kChunkSamples];
    static float   chunk_r[kChunkSamples];
    static float   reverb_send_l[kChunkSamples];
    static float   reverb_send_r[kChunkSamples];
    static int16_t pcm_chunk[kChunkSamples * 2];

    // Two full passes over the loop: pass 0 is a throwaway priming pass
    // (runs every layer's real effects chain plus the master filter, but
    // writes nothing) so the master filter isn't starting cold at sample
    // 0 of the real pass -- a cold Svf at low cutoff/high resonance has a
    // real audible startup thump, not just a brief settle. Because the
    // render always starts at play_pos_==0 and runs exactly one loop
    // length, pass 0 naturally wraps play_pos_ back to (very near) 0 via
    // the normal modulo wraparound already in Process() -- forcing it
    // again below is just cheap insurance against float drift. Both
    // passes count toward on_progress so the bar moves smoothly across
    // the whole operation instead of sitting at 0% through all of pass 0.
    //
    // Pass 0 doubles as a peak scan: nothing here normalizes or limits
    // the live signal path, so a loop that never gets near clipping would
    // otherwise export using only a fraction of the 16-bit range and play
    // back noticeably quiet. Since pass 0 already computes the exact same
    // post-filter signal pass 1 will write, tracking its peak is free --
    // pass 1 then applies a flat makeup gain so the loudest sample lands
    // just under full scale instead of wherever it naturally fell.
    float             peak   = 0.f;
    float             makeup = 1.f;
    Resampler48to44_1 resampler; // only driven below when for_microdexed and write_this_pass
    for(int pass = 0; pass < 2 && ok; pass++)
    {
        bool write_this_pass = pass == 1;
        if(write_this_pass)
        {
            const float kExportTarget = 0.98f; // just under full scale, avoids quantizing right at the edge
            const float kMaxMakeup    = 8.f;   // don't blow up near-silent content into hiss/noise
            makeup = peak > 0.0001f ? Clampf(kExportTarget / peak, 0.f, kMaxMakeup) : 1.f;
        }

        for(int i = 0; i < n_snap; i++)
            layers[i].SetPlayPosRaw(0.f);

        size_t remaining = render_len;
        while(remaining > 0 && ok)
        {
            UINT n      = (UINT)(remaining < kChunkSamples ? remaining : kChunkSamples);
            UINT wcount = 0; // frames actually written into pcm_chunk this chunk (== n
                              // unless for_microdexed, since resampling emits 0 or 1
                              // output frame per native frame, not always 1)

            for(UINT i = 0; i < n; i++)
            {
                chunk_l[i]        = 0.f;
                chunk_r[i]        = 0.f;
                reverb_send_l[i]  = 0.f;
                reverb_send_r[i]  = 0.f;
            }
            float* out_ptrs[2]         = {chunk_l, chunk_r};
            float* reverb_send_ptrs[2] = {reverb_send_l, reverb_send_r};

            for(int L = 0; L < num_layers; L++)
                layers[L].Process(in_ptrs, out_ptrs, reverb_send_ptrs, n, dummy_ticks, tempo,
                                    project_speed);

            for(UINT i = 0; i < n; i++)
            {
                float rev_wet_l, rev_wet_r;
                export_reverb.Process(reverb_send_l[i], reverb_send_r[i], &rev_wet_l, &rev_wet_r);
                chunk_l[i] += rev_wet_l;
                chunk_r[i] += rev_wet_r;

                float l = chunk_l[i];
                float r = chunk_r[i];
                if(master_filter_mode != FilterMode::Off)
                {
                    mfilt_l.Process(l);
                    mfilt_r.Process(r);
                    switch(master_filter_mode)
                    {
                        case FilterMode::LowPass:
                            l = mfilt_l.Low();
                            r = mfilt_r.Low();
                            break;
                        case FilterMode::HighPass:
                            l = mfilt_l.High();
                            r = mfilt_r.High();
                            break;
                        case FilterMode::BandPass:
                            l = mfilt_l.Band();
                            r = mfilt_r.Band();
                            break;
                        default: break;
                    }
                }
                if(write_this_pass)
                {
                    if(for_microdexed)
                    {
                        float rl, rr;
                        if(resampler.Push(l, r, &rl, &rr))
                        {
                            rl = Clampf(rl * makeup, -1.f, 1.f);
                            rr = Clampf(rr * makeup, -1.f, 1.f);
                            pcm_chunk[2 * wcount]     = (int16_t)(rl * 32767.f);
                            pcm_chunk[2 * wcount + 1] = (int16_t)(rr * 32767.f);
                            wcount++;
                        }
                    }
                    else
                    {
                        l = Clampf(l * makeup, -1.f, 1.f);
                        r = Clampf(r * makeup, -1.f, 1.f);
                        pcm_chunk[2 * wcount]     = (int16_t)(l * 32767.f);
                        pcm_chunk[2 * wcount + 1] = (int16_t)(r * 32767.f);
                        wcount++;
                    }
                }
                else
                {
                    float a = fabsf(l) > fabsf(r) ? fabsf(l) : fabsf(r);
                    if(a > peak)
                        peak = a;
                }
            }

            if(write_this_pass)
            {
                UINT bytes = wcount * 2 * (UINT)sizeof(int16_t);
                UINT written;
                fr = f_write(&file, pcm_chunk, bytes, &written);
                ok = fr == FR_OK && written == bytes;
                if(!ok)
                    SetError("data", fr);
            }

            remaining -= n;
            if(on_progress)
            {
                size_t done  = (size_t)pass * render_len + (render_len - remaining);
                size_t total = render_len * 2;
                on_progress((float)done / (float)total);
            }
        }
    }

    // Always restore -- runs whether the render above succeeded or bailed
    // out partway through, so a failed export never leaves live playback
    // resuming from the wrong phase.
    for(int i = 0; i < n_snap; i++)
        layers[i].SetPlayPosRaw(saved_pos[i]);

    FRESULT close_res = f_close(&file);
    if(close_res != FR_OK && ok)
        SetError("close", close_res);

    return ok && close_res == FR_OK;
}

bool SavePrefs(TempoClock&  tempo,
              float        master_volume01,
              bool         bypass,
              FilterMode   master_filter_mode,
              float        master_filter_cutoff01,
              float        master_filter_res01,
              float        reverb_size01,
              float        bypass_reverb_send01)
{
    if(!card_ready)
        return false;

    ClearError();
    static FIL file; // see the DTCMRAM/DMA comment in Save() above
    FRESULT    fr = f_open(&file, kPrefsFilename, FA_CREATE_ALWAYS | FA_WRITE);
    if(fr != FR_OK)
    {
        SetError("open", fr);
        return false;
    }

    static FileHeader hdr;
    hdr = FileHeader{};
    memcpy(hdr.magic, "PREF", 4); // distinct from "OURO" -- never cross-loadable with Load()
    hdr.version                = kFileVersion;
    hdr.num_layers              = 0; // no layers/audio follow -- header only
    hdr.bpm                     = tempo.GetBpm();
    hdr.bars                    = tempo.GetBars();
    hdr.metronome_enabled       = tempo.IsMetronomeEnabled() ? 1 : 0;
    hdr.bypass                  = bypass ? 1 : 0;
    hdr.metronome_vol01         = tempo.GetMetronomeVolume01();
    hdr.master_volume01         = master_volume01;
    hdr.master_filter_mode      = (int32_t)master_filter_mode;
    hdr.master_filter_cutoff01  = master_filter_cutoff01;
    hdr.master_filter_res01     = master_filter_res01;
    hdr.reverb_size01           = reverb_size01;
    hdr.bypass_reverb_send01    = bypass_reverb_send01;

    UINT bw;
    fr      = f_write(&file, &hdr, sizeof(hdr), &bw);
    bool ok = fr == FR_OK && bw == sizeof(hdr);
    if(!ok)
        SetError("hdr", fr);

    FRESULT close_res = f_close(&file);
    if(close_res != FR_OK && ok)
        SetError("close", close_res);

    return ok && close_res == FR_OK;
}

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
              float*      out_metronome_vol01)
{
    // No card, no file, or wrong shape -- all silently "nothing saved
    // yet", not an error (see the header comment: the caller's own
    // hardcoded defaults should just stand in every one of these cases).
    if(!card_ready)
        return false;

    static FIL file; // see the DTCMRAM/DMA comment in Save() above
    FRESULT    fr = f_open(&file, kPrefsFilename, FA_READ);
    if(fr != FR_OK)
        return false;

    static FileHeader hdr;
    hdr = FileHeader{};
    UINT br;
    fr      = f_read(&file, &hdr, sizeof(hdr), &br);
    bool ok = fr == FR_OK && br == sizeof(hdr);
    f_close(&file);
    if(!ok || memcmp(hdr.magic, "PREF", 4) != 0 || hdr.version != kFileVersion)
        return false;

    *out_bpm                    = hdr.bpm;
    *out_bars                   = hdr.bars;
    *out_master_volume01        = hdr.master_volume01;
    *out_bypass                 = hdr.bypass != 0;
    *out_master_filter_mode     = (FilterMode)hdr.master_filter_mode;
    *out_master_filter_cutoff01 = hdr.master_filter_cutoff01;
    *out_master_filter_res01    = hdr.master_filter_res01;
    *out_reverb_size01          = hdr.reverb_size01;
    *out_bypass_reverb_send01   = hdr.bypass_reverb_send01;
    *out_metronome_enabled      = hdr.metronome_enabled != 0;
    *out_metronome_vol01        = hdr.metronome_vol01;
    return true;
}

const char* GetLastError()
{
    return last_error;
}

} // namespace PerformanceStore
