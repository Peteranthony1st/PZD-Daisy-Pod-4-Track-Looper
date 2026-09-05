#pragma once

// Places a function in ITCMRAM (the STM32H750's dedicated, zero-
// contention instruction memory) instead of wherever it would otherwise
// land -- QSPIFLASH under this project's BOOT_QSPI setup. Mirrors
// libDaisy's own DSY_QSPI_TEXT/DSY_SDRAM_BSS pattern (per/qspi.h,
// dev/sdram.h), but libDaisy has no ITCM equivalent of its own, so this
// is project-local. Only meaningful with the custom
// src/STM32H750IB_qspi_custom.lds, which is the only linker script with
// a .itcm_text output section wired up -- see that file's header comment
// for the full story (a real, measured audio-callback timing regression
// under plain BOOT_QSPI, not a preemptive optimization).
//
// Reserve this for the real-time call graph only (AudioCallback() and
// everything it calls) -- ITCMRAM is a fixed 64KB, shared by all of it.
#define DSY_ITCM_TEXT __attribute__((section(".itcm_text")))
