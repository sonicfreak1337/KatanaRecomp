# Katana Runtime

Die Katana-Runtime ist seit KR-2101 eine eigene statische Bibliothek. Generierter
C++-Code enthaelt keine Implementierung von Speicher, CPU-Zustand oder
ungeloesten Kontrollflusspfaden mehr.

## ABI

Die aktuelle Runtime-ABI ist Version `3`.

Generierter Code enthaelt eine Compile-Time-Pruefung gegen diese Version. Eine
abweichende Runtime wird beim Kompilieren sichtbar abgelehnt. ABI-Version 3
kennzeichnet den Wechsel vom flachen Runtime-Speicher zum regionbasierten Bus.

## CMake

Innerhalb des KatanaRecomp-Builds steht das Ziel `KatanaRecomp::runtime` zur
Verfuegung:

```cmake
target_link_libraries(mein_programm PRIVATE KatanaRecomp::runtime)
```

Der generierte C++-Code bindet automatisch
`katana/runtime/runtime.hpp` ein.

## Zentraler CPU-Zustand

`katana::runtime::CpuState` enthaelt die Architektur- und Runtime-Daten an einer
Stelle:

- 16 allgemeine Register und acht banked Register
- getrennte 16er-Rohbitbaenke `FR` und `XF`
- `PC`, `SR`, `GBR`, `VBR`, `SSR`, `SPC`, `SGR` und `DBR`
- `MACH`, `MACL`, `PR`, `FPUL` und `FPSCR`
- `TRA`, `EXPEVT` und `INTEVT`
- explizite T-, S-, Q- und M-Zustandsbits
- sichtbare Trap- und Schlafzustaende
- den zentralen regionbasierten Speicherbus

Die FPU-Baenke speichern vorerst ausschliesslich unveraenderte 32-Bit-Rohwerte.
Arithmetik, Bankumschaltung und `FPSCR`-Modi folgen in der FPU-Phase.

## Regionbasierter Speicherbus

KR-2201 fuehrt die zentrale Adressdekodierung in `katana::runtime::Memory` ein.
Der Bus bindet benannte `MemoryDevice`-Instanzen an 32-Bit-Adressbereiche.
`LinearMemoryDevice` stellt dafuer einen einfachen Byte-Speicher bereit.

Eine leere Buskonfiguration entsteht mit `Memory(0u)`. Regionen werden mit
`map_region` registriert und anhand ihrer Basisadresse deterministisch sortiert.
Der bisherige Konstruktor bleibt kompatibel: `Memory()` registriert vorerst eine
lineare 1-MiB-Region ab Adresse null, bis KR-2202 das echte Dreamcast-RAM-Layout
einfuehrt.

Der Bus garantiert zentral:

- keine leeren oder namenlosen Regionen
- keine Ueberlappungen
- keine Bereiche ausserhalb des 32-Bit-Adressraums
- ein Zugriff bleibt vollstaendig innerhalb genau einer Region
- Little-Endian-Verhalten fuer 16- und 32-Bit-Zugriffe
- sichtbare Fehler fuer nicht zugeordnete Adressen
- optionalen Schreibschutz pro Region
- auslesbare Regionsmetadaten fuer Tests und Diagnose

RAM-Spiegelungen, VRAM, AICA-RAM, BIOS, Flash und MMIO bleiben getrennte
Folgetasks der v0.22-Roadmap.

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

Ein CPU-Reset loescht oder ersetzt den Speicherbus und seine registrierten
Geraete absichtlich nicht. Dreamcast-spezifische Startwerte gehoeren in die
spaetere Plattformkonfiguration.

## Weitere Runtime-Grundlage

- sichtbare Fehlerpfade fuer ungeloeste Calls und Spruenge
- Runtime-Tests fuer CPU-Zustand, Reset und Speicherbus