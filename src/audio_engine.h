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
