#include "performance_store.h"
#include <cstdio>
#include <cstring>
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

void SlotFilename(int slot, char* out, size_t out_size)
{
    snprintf(out, out_size, "PERF%03d.DAT", slot);
}

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
    float    reverb_send01;
    float    reverb_size01;
    uint8_t  pitch_enabled;
    float    pitch_amount01;
    float    pitch_fun01;
    int32_t  pitch_delay_preset;
};

// Bumped 1 -> 2 -> 3 as LayerHeader gained fields (most recently
// pitch_delay_preset) -- Load() already rejects a version mismatch
// cleanly (see below), so a performance saved under an older version
// will correctly fail to load rather than being misread.
constexpr uint32_t kFileVersion = 3;

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
        char    fname[16];
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
        char    fname[16];
        FILINFO fno;
        SlotFilename(slot, fname, sizeof(fname));
        if(f_stat(fname, &fno) != FR_OK)
            return slot;
    }
    return -1;
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
          ProgressFn         on_progress)
{
    if(!card_ready || slot < 1 || slot > kMaxSlots)
        return false;

    char fname[16];
    SlotFilename(slot, fname, sizeof(fname));

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
        lh.reverb_size01      = layer.GetReverbSize01();
        lh.pitch_enabled      = layer.GetPitchEnabled() ? 1 : 0;
        lh.pitch_amount01     = layer.GetPitchAmount01();
        lh.pitch_fun01        = layer.GetPitchFun01();
        lh.pitch_delay_preset = (int32_t)layer.GetPitchDelayPreset();

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
          ProgressFn   on_progress)
{
    if(!card_ready || slot < 1 || slot > kMaxSlots)
        return false;

    char fname[16];
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
        layer.SetReverbSize01(lh.reverb_size01);
        layer.SetPitchAmount01(lh.pitch_amount01);
        layer.SetPitchFun01(lh.pitch_fun01);
        layer.SetPitchDelayPreset((int)lh.pitch_delay_preset);
        layer.SetPitchEnabled(lh.pitch_enabled != 0);

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

const char* GetLastError()
{
    return last_error;
}

} // namespace PerformanceStore
