TARGET = main

CPP_SOURCES = main.cpp looper_layer.cpp tempo_clock.cpp ui.cpp performance_store.cpp font_tomthumb.cpp granular_engine.cpp dexed_synth.cpp dexed_sysex.cpp dexed_factory_data.cpp dattorro_reverb.cpp
# msfa (the real DX7 emulation core, vendored from Synth_Dexed -- see
# msfa/synth.h's own provenance comment).
CPP_SOURCES += msfa/exp2.cpp msfa/freqlut.cpp msfa/sin.cpp msfa/fm_op_kernel.cpp msfa/fm_core.cpp msfa/env.cpp msfa/pitchenv.cpp msfa/lfo.cpp msfa/porta.cpp msfa/dx7note.cpp msfa/EngineMsfa.cpp

LIBDAISY_DIR = libDaisy
DAISYSP_DIR = DaisySP
# Was -Os (optimize for size) -- that constraint is gone now: QSPI flash
# is at ~7.5% used (the migration to BOOT_QSPI/8MB external flash this
# comment used to justify -Os against happened after this was written),
# and -Os's own "AudioCallback() isn't hurt by it" claim was asserted,
# not measured against the alternative. -O2 was measured on real
# hardware (Global:Mixer's own CPU meter) and gave a real, clear win:
# idle 10%->8%, reverb active 36-37%->32%, reverb+delay 50%->44% --
# roughly 12-20% off across the board, including the always-on idle
# floor, not just reverb specifically. Trying -O3 next to see if
# there's more room past -O2's own gain.
OPT = -O3
# Needed for ReverbSc (shared reverb bus, see main.cpp). Comment must stay
# on its own line -- a trailing "= 1 # comment" leaves a trailing space in
# the value, which then fails the ifeq($(USE_DAISYSP_LGPL),1) check below.
USE_DAISYSP_LGPL = 1
# Boots via the separate Daisy bootloader (installed once with
# `make program-boot`) and executes directly from the 8MB external QSPI
# flash chip instead of the 128KB internal flash, which was nearly full.
# See DESIGN.md for the full story (staged migration, real-time budget
# re-measurement, and the ITCM fallback if QSPI's execute-in-place
# latency ever turns out to matter for the audio path).
APP_TYPE = BOOT_QSPI
# Custom copy of libDaisy/core/STM32H750IB_qspi.lds (see its own header
# comment) adding a .itcm_text output section -- real hardware
# measurement showed plain BOOT_QSPI raised the audio callback's
# worst-case timing from <50% to 69% of budget for the same DSP
# workload, so the real-time call graph is hand-placed in ITCMRAM
# instead (see itcm.h) to keep it exactly as fast as it was on internal
# flash, while everything else still gets QSPI's extra room.
LDSCRIPT = STM32H750IB_qspi_custom.lds

SYSTEM_FILES_DIR = $(LIBDAISY_DIR)/core
include $(SYSTEM_FILES_DIR)/Makefile

# -O2/-O3 (above) only change inlining/unrolling heuristics within strict
# IEEE float semantics -- this whole audio path is float-math-bound DSP
# (FM synthesis, granular, reverb, filters), so relaxing that (allowing
# reassociation, faster reciprocal/division, assuming no NaN/Inf) is a
# real, separate lever past -O3's own ceiling, not just more of the same
# knob. Appended here (after the include, not above) since CFLAGS is a
# plain recursively-expanded Make variable -- CPPFLAGS (what actually
# compiles every .cpp file) references $(CFLAGS) lazily, so appending
# after core/Makefile's own CFLAGS definition still reaches every file,
# C and C++ alike. Real trade-off, not a free win like -O2/-O3 were:
# this is no longer strict IEEE-compliant floating point (common and
# generally safe for audio DSP, but a genuine semantic change) --
# listen carefully for anything different, not just measure CPU.
CFLAGS += -ffast-math

# -O2/-O3/-ffast-math above all optimize each .cpp file in isolation --
# the compiler working on dexed_synth.cpp has zero visibility into
# dx7note.cpp/fm_core.cpp/env.cpp/etc even though it calls into all of
# them every single audio block, so those calls stay real function
# calls (no inlining, no cross-file constant propagation) no matter how
# aggressive the per-file flags get. LTO defers final optimization to
# the link step, once every object file is being combined, so the
# compiler can see and optimize across those boundaries too -- real
# potential upside specifically because this project's hot audio path
# (the whole msfa DX7 core alone is 11 separate .cpp files) is this
# fragmented. Needs the flag on both ends: CFLAGS so each object file
# carries LTO info, LDFLAGS so the final link actually performs the
# merge -- $(OPT) added to LDFLAGS too since GCC's LTO re-optimizes at
# link time using whatever -O level the link command itself gets, not
# whatever was used per-file originally. Real trade-off: noticeably
# longer build times, and whole-program visibility can occasionally
# surface latent bugs (aliasing/ODR assumptions) that happened to work
# under separate compilation -- watch the build for new warnings, not
# just CPU and sound.
CFLAGS  += -flto
LDFLAGS += -flto $(OPT)
