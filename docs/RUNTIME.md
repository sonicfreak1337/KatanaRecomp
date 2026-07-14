# Katana Runtime

Die Katana-Runtime ist seit KR-2101 eine eigene statische Bibliothek. Generierter
C++-Code enthaelt keine Implementierung von Speicher, CPU-Zustand oder
ungeloesten Kontrollflusspfaden mehr.

## ABI

Die aktuelle Runtime-ABI ist Version `1`.

Generierter Code enthaelt eine Compile-Time-Pruefung gegen diese Version. Eine
abweichende Runtime wird beim Kompilieren sichtbar abgelehnt.

## CMake

Innerhalb des KatanaRecomp-Builds steht das Ziel `KatanaRecomp::runtime` zur
Verfuegung:

```cmake
target_link_libraries(mein_programm PRIVATE KatanaRecomp::runtime)
```

Der generierte C++-Code bindet automatisch
`katana/runtime/runtime.hpp` ein.

## Enthaltene Grundlage

- `katana::runtime::Memory`
- `katana::runtime::CpuState`
- allgemeine und banked Register
- SH-4-System- und Statusregister
- Little-Endian-Speicherzugriffe
- sichtbare Fehlerpfade fuer ungeloeste Calls und Spruenge

Der Dreamcast-Speicherbus und MMIO werden erst in v0.22 aufgebaut.