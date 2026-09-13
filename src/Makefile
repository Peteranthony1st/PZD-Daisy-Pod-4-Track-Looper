TARGET = main

CPP_SOURCES = main.cpp looper_layer.cpp tempo_clock.cpp ui.cpp performance_store.cpp font_tomthumb.cpp pad_synth.cpp granular_engine.cpp fm_synth.cpp

LIBDAISY_DIR = libDaisy
DAISYSP_DIR = DaisySP
# Optimize our own sources for size, not speed -- the SD save/load feature
# pulls FatFS + the SDMMC driver out of libdaisy.a for the first time in
# this project, and flash is otherwise nearly full (see build stats printed
# at link time). None of this project's own code is performance-critical
# outside AudioCallback(), which -Os doesn't meaningfully hurt.
OPT = -Os
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
