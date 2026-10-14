#pragma once

// Set true to make AudioCallback() (main.cpp) output silence and skip
// touching the tempo clock or any layer entirely, without altering their
// state -- a way to make a multi-step, blocking main-loop operation
// atomic with respect to real-time audio.
//
// Why this exists: PerformanceStore::Load() restores layers one at a
// time, and each one can take a while (streaming its recorded audio off
// the SD card). The audio ISR keeps running throughout a blocking main-
// loop call like that -- it isn't paused just because main() is busy --
// so without this, a layer already restored earlier in the loop starts
// playing (and the tempo clock keeps ticking) while later layers are
// still being read, and every layer ends up started at a different
// sample offset: audibly out of sync with each other, and with the
// tempo/metronome. Ui::TriggerLoad() sets this before calling Load() and
// clears it only after every layer AND the tempo's phase (see
// TempoClock::ResetPhase()) have been fully restored, so playback always
// resumes with everything aligned to sample 0 at once.
extern volatile bool g_audio_suspended;

// Live level meters + clip-indicator latches for the 4 Mixer channels
// that don't otherwise have anywhere to track this (the 4 loop layers
// already have their own, see LooperLayer::GetLevel()/IsClipping()) --
// Grains and Dexed read their own post-mixer-gain output (gran_l/
// dexed_l, the same signal that feeds both the live mix and a
// recording), Bypass reads its own post-gain/pre-pan monitor signal
// (byp_mono), and Master reads the final out[] after everything
// (volume, reverb, delay, filter, click) has been mixed in. All 4
// updated once per sample in AudioCallback() (main.cpp), same one-pole
// smoothing and clip-hold-latch convention as LooperLayer's own meter_/
// clip_hold_ -- see Ui::MixerGetLevel01()/MixerIsClipping().
extern float g_grains_level;
extern float g_dexed_level;
extern float g_bypass_level;
extern float g_master_level;
extern float g_grains_clip_hold;
extern float g_dexed_clip_hold;
extern float g_bypass_clip_hold;
extern float g_master_clip_hold;
