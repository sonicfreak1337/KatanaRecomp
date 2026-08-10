# SkyEmu ARM7TDMI core

`arm7.h` is vendored from [SkyEmu](https://github.com/skylersaleh/SkyEmu) at
commit `01516d6798e3652b583e6a366085bb51c43b528d`. It is used only for the
Dreamcast AICA ARM7 execution domain. SkyEmu is distributed under the MIT
license reproduced in `LICENSE`.

The vendored header differs only by making the internal register-name pointer
`const`-correct so it compiles as C++ under the repository's warning profile.
Katana-specific bus, interrupt, scheduling, snapshot and fail-closed behavior
lives in `src/runtime/aica_arm7_core.cpp`.
