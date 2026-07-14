# Katana Runtime

Die Katana-Runtime ist seit KR-2101 eine eigene statische Bibliothek. Generierter
C++-Code enthaelt keine Implementierung von Speicher, CPU-Zustand oder
ungeloesten Kontrollflusspfaden mehr.

## ABI

Die aktuelle Runtime-ABI ist Version `2`.

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

## Zentraler CPU-Zustand

`katana::runtime::CpuState` enthaelt die fuer v0.21 benoetigten
Architektur- und Runtime-Daten an einer Stelle:

- 16 allgemeine Register und acht banked Register
- getrennte 16er-Rohbitbaenke `FR` und `XF`
- `PC`, `SR`, `GBR`, `VBR`, `SSR`, `SPC`, `SGR` und `DBR`
- `MACH`, `MACL`, `PR`, `FPUL` und `FPSCR`
- `TRA`, `EXPEVT` und `INTEVT`
- explizite T-, S-, Q- und M-Zustandsbits
- sichtbare Trap- und Schlafzustaende
- den aktuellen Runtime-Speicher

Die FPU-Baenke speichern vorerst ausschliesslich unveraenderte 32-Bit-Rohwerte.
Arithmetik, Bankumschaltung und `FPSCR`-Modi folgen in der FPU-Phase.

## Deterministischer CPU-Reset

`katana::runtime::reset_cpu` stellt einen wiederholbaren CPU-Zustand her.

Der Standard-Reset setzt alle Registerbaenke, Systemregister, Ereignisregister,
Akkumulatoren, FPU-Rohregister und Runtime-Flags auf null. Eine optionale
`ResetState`-Konfiguration kann folgende Startwerte vorgeben:

- Programmzaehler `PC`
- Stackpointer `R15`
- Vektorbasis `VBR`
- Statusregister `SR`
- FPU-Statusregister `FPSCR`

`SR` wird dabei ueber dieselbe Maskierungs- und Registerbanklogik geschrieben wie
ein normaler Statusregistertransfer.

Ein CPU-Reset loescht den Runtime-Speicher absichtlich nicht. Speicherabbild,
Boot-ROM-Layout und Dreamcast-spezifische Startwerte gehoeren in die spaetere
Plattformkonfiguration.

## Weitere Runtime-Grundlage

- `katana::runtime::Memory`
- Little-Endian-Speicherzugriffe
- sichtbare Fehlerpfade fuer ungeloeste Calls und Spruenge

Der Dreamcast-Speicherbus und MMIO werden erst in v0.22 aufgebaut.