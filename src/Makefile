TARGET = main

CPP_SOURCES = main.cpp looper_layer.cpp tempo_clock.cpp ui.cpp performance_store.cpp font_tomthumb.cpp

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

SYSTEM_FILES_DIR = $(LIBDAISY_DIR)/core
include $(SYSTEM_FILES_DIR)/Makefile
