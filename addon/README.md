# Dexed factory patch source data

The raw 32-voice DX7 SysEx bank files (`.syx`) that `src/dexed_factory_data.cpp`'s
embedded factory presets were compiled from, kept here for reference and
attribution — not used by the firmware build itself (the actual patch data
is compiled directly into the binary as packed byte arrays; nothing here is
read at build or run time).

`SD/DEXED/<Category>/` mirrors the firmware's own 28 factory folder names
exactly, each holding the real bank file(s) that folder's presets were
unpacked from. Real, freely-distributed SysEx data drawn from the broader
Dexed/MicroDexed open-source ecosystem — not invented, and not traceable to
one single upstream repo (see `README.md`'s own Thanks section and
`DESIGN.md`'s *Dexed* section for the fuller story).
