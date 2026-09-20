PZD-4LOOP-DEXED-GRAINS -- starter SD card
==========================================

Copy everything inside this "sdcard-template" folder onto the ROOT of a
FAT32-formatted SD card, then insert it into the Pod.

None of this is strictly required to boot the firmware -- most of these
folders get created automatically the first time you actually use the
matching feature (Save, Export, etc). This template exists for two
folders specifically that do NOT get created automatically, so the
matching Import feature would otherwise silently find nothing:

  IMPORT/     Grains WAV import
  DXIMPORT/   Dexed SysEx (.syx) import

The rest (PERF/, GRNP/, DEXP/, DEXP/IMPORTS/, WAV/, custom/) are
included too so a fresh card already looks like a populated one and you
can see at a glance what each folder is for -- see the README.txt
inside each one.

See this project's own README.md ("Building and flashing") for how to
get the firmware itself (main.bin) onto the card.
