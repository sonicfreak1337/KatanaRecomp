# Katana Runtime

Die Katana-Runtime ist seit KR-2101 eine eigene statische Bibliothek. Generierter
C++-Code enthaelt keine Implementierung von Speicher, CPU-Zustand oder
ungeloesten Kontrollflusspfaden mehr.

## ABI

Die aktuelle Runtime-ABI ist Version `4`.

Generierter Code enthaelt eine Compile-Time-Pruefung gegen diese Version. Eine
abweichende Runtime wird beim Kompilieren sichtbar abgelehnt. ABI-Version 3
kennzeichnete den Wechsel vom flachen Runtime-Speicher zum regionbasierten Bus.
ABI-Version 4 erweitert die virtuelle Speichergeraete-API um breitenbewusste
16- und 32-Bit-Zugriffe fuer MMIO-Nebenwirkungen.

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
Der bisherige Konstruktor bleibt kompatibel: `Memory()` registriert eine lineare
1-MiB-Region ab Adresse null fuer synthetische und generierte Tests. Eine echte
Dreamcast-Konfiguration beginnt stattdessen mit `Memory(0u)`.

Der Bus garantiert zentral:

- keine leeren oder namenlosen Regionen
- keine Ueberlappungen
- keine Bereiche ausserhalb des 32-Bit-Adressraums
- ein Zugriff bleibt vollstaendig innerhalb genau einer Region
- Little-Endian-Verhalten fuer 16- und 32-Bit-Zugriffe
- sichtbare Fehler fuer nicht zugeordnete Adressen
- optionalen Schreibschutz pro Region
- auslesbare Regionsmetadaten fuer Tests und Diagnose

## Dreamcast-Haupt-RAM und Spiegelungen

KR-2202 fuehrt `map_dreamcast_main_ram` ein. Die Funktion erzeugt genau ein
nullinitialisiertes `LinearMemoryDevice` mit 16 MiB und registriert dasselbe
Backing in 28 direkten Aliasfenstern:

- vier physische Area-3-Fenster von `0x0C000000` bis `0x0FFFFFFF`
- die entsprechenden Wiederholungen in den U0/P0-Bereichen ab `0x2C000000`,
  `0x4C000000` und `0x6C000000`
- gecachte P1-Aliase von `0x8C000000` bis `0x8FFFFFFF`
- ungecachte P2-Aliase von `0xAC000000` bis `0xAFFFFFFF`
- den derzeit direkten P3-No-MMU-Pfad von `0xCC000000` bis `0xCFFFFFFF`

Der P4-Bereich ab `0xE0000000` bleibt fuer interne SH-4-Ressourcen reserviert
und wird nicht als Haupt-RAM abgebildet. Eine Konfiguration prueft zuerst alle
Aliasfenster gegen vorhandene Regionen. Bei einer Kollision bleibt der Bus
unveraendert.

Das beobachtbare Adresslayout wurde unabhaengig gegen Flycast
(`core/hw/sh4/sh4_mem.cpp`, `core/hw/mem/addrspace.cpp`) und dcrecomp
(`include/recompiler/sh4_cpu.h`, `src/recompiler/sh4_cpu.c`) gegengeprueft.
Aus den Referenzprojekten wurde kein Code uebernommen.

## Dreamcast-VRAM und AICA-RAM

KR-2203 fuehrt `map_dreamcast_vram` und `map_dreamcast_aica_ram` ein.
Beide Funktionen erzeugen ein eigenes nullinitialisiertes Backing und
registrieren alle direkten U0/P0-, P1-, P2- und derzeitigen P3-No-MMU-Aliase.
P4 bleibt ausgespart.

Das 8-MiB-VRAM besitzt zwei Sichten auf dasselbe Backing:

- 28 lineare 64-Bit-Pfad-Aliase auf Basis der physischen Fenster
  `0x04000000`, `0x04800000`, `0x06000000` und `0x06800000`
- 28 bankinterleavte 32-Bit-Pfad-Aliase auf Basis der physischen Fenster
  `0x05000000`, `0x05800000`, `0x07000000` und `0x07800000`

Die 32-Bit-Sicht ordnet aufeinanderfolgende 32-Bit-Woerter abwechselnd den
beiden 64-Bit-VRAM-Baenken zu. Bytepositionen innerhalb eines Wortes bleiben
erhalten, sodass die zentrale Little-Endian-Logik des Busses weiterhin gilt.

Das 2-MiB-AICA-RAM wird in den vier physischen Fenstern `0x00800000`,
`0x00A00000`, `0x00C00000` und `0x00E00000` gespiegelt. Zusammen mit den
sieben direkten SH-4-Segmenten entstehen 28 Aliase auf dasselbe Backing.

Alle vorgesehenen Fenster werden vor der ersten Registrierung gegen
bestehende Regionen und gegeneinander geprueft. Eine Kollision hinterlaesst
den Bus unveraendert. Adresslayout und beobachtbares VRAM-Bankverhalten
wurden unabhaengig gegen Flycast (`core/hw/sh4/sh4_mem.cpp` und
`core/hw/pvr/pvr_mem.cpp`) gegengeprueft; es wurde kein Referenzcode
uebernommen.

## Dreamcast-BIOS und Flash

KR-2204 fuehrt `map_dreamcast_bios` und `map_dreamcast_flash` ein. Beide
Funktionen akzeptieren optional ein exakt grosses Byte-Abbild, kopieren es in
ein eigenes Runtime-Backing und registrieren sieben direkte U0/P0-, P1-, P2-
und derzeitige P3-No-MMU-Aliase. P4 bleibt ausgespart.

Das BIOS umfasst 2 MiB ab physisch `0x00000000`. Alle Busregionen sind
read-only; Schreibversuche schlagen sichtbar fehl. Das Flash umfasst 128 KiB
ab physisch `0x00200000` und bleibt als getrenntes beschreibbares Backing
erhalten. Ohne bereitgestelltes Abbild werden beide Geraete deterministisch
mit `0xFF` initialisiert. Ein Abbild mit falscher Groesse wird abgelehnt, bevor
eine Region registriert wird.

Die Flash-Abstraktion modelliert in KR-2204 bewusst noch kein herstellerspezifisches
Programmier-, Loesch- oder Kommando-Protokoll. Buszugriffe schreiben direkt in
das Flash-Backing; eine spaetere Plattformintegration kann darauf einen
zustandsbehafteten Handler aufsetzen.

Adresslayout und Zugriffsrechte wurden unabhaengig gegen Flycast
(`core/hw/holly/sb_mem.cpp`) gegengeprueft. Es wurden weder Referenzcode noch
BIOS-, Flash- oder andere geschuetzte Binaerdaten uebernommen.

## Breitenbewusste MMIO-Handler

KR-2205 fuehrt `MmioMemoryDevice` ein. Ein MMIO-Geraet besitzt eine feste
Regionsgroesse und optionale Lese- und Schreibcallbacks. Jeder Callback erhaelt
den lokalen Geraeteoffset und `MemoryAccessWidth` fuer Byte, Halfword oder Word.
Schreibcallbacks erhalten zusaetzlich den auf die Zugriffsbreite maskierten Wert.

Der Speicherbus delegiert 16- und 32-Bit-Zugriffe jetzt direkt an das
Speichergeraet. Dadurch loest ein Registerzugriff genau einen MMIO-Callback aus
und wird nicht mehr in mehrere Bytezugriffe mit mehrfachen Nebenwirkungen
zerlegt. `MemoryDevice` stellt weiterhin Standardimplementierungen bereit, die
bytebasierte RAM-, VRAM- und Firmwaregeraete little-endian zusammensetzen.

Ein MMIO-Geraet darf reine Lese- oder Schreibhandler besitzen. Der jeweils
fehlende Zugriffspfad wird sichtbar als Laufzeitfehler gemeldet. Regionsschutz
wie `ReadOnly` greift weiterhin vor dem Geraetehandler. Bereichsueberschreitende
Zugriffe werden abgelehnt, bevor ein Callback ausgefuehrt wird.

KR-2205 stellt bewusst nur die generische Handler-Infrastruktur bereit.
Konkrete Dreamcast-System-, AICA-, GD-ROM-, Maple- oder PVR-Registermodelle
gehoeren zu den spaeteren Plattformkomponenten.

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
- Runtime-Tests fuer CPU-Zustand, Reset, Speicherbus, breitenbewusste MMIO-Handler sowie Dreamcast-RAM-, VRAM-, AICA-RAM-, BIOS- und Flash-Aliase
