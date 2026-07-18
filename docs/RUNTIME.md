# Katana Runtime

Die Katana-Runtime ist seit KR-2101 eine eigene statische Bibliothek. Generierter
C++-Code enthaelt keine Implementierung von Speicher, CPU-Zustand oder
ungeloesten Kontrollflusspfaden mehr.

## ABI

Die aktuelle Runtime-ABI ist Version `8`. Die typisierte Block-ABI ist seit
KR-4611 Version `2`.

Generierter Code enthaelt eine Compile-Time-Pruefung gegen diese Version. Eine
abweichende Runtime wird beim Kompilieren sichtbar abgelehnt. ABI-Version 3
kennzeichnete den Wechsel zum regionbasierten Bus, ABI-Version 4 fuehrte
breitenbewusste MMIO-Zugriffe ein. ABI-Version 5 erweitert `Memory` um
Ausrichtungsrichtlinie, strukturierte Zugriffsfehler, Trace-Handler und
Watchpoint-Zustand. ABI-Version 6 ergaenzt den zentralen CPU-Zustand um `TEA`,
strukturierte Exception-Ursachen und Delay-Slot-Kontext und bindet generierten
Code an den gemeinsamen Exception-Pfad. ABI-Version 7 zentralisiert die
`FPSCR`-Maskierung und die sichtbare FR-/XF-Bankumschaltung.

## CMake

Innerhalb des KatanaRecomp-Builds steht das Ziel `KatanaRecomp::runtime` zur
Verfuegung:

```cmake
target_link_libraries(mein_programm PRIVATE KatanaRecomp::runtime)
```

Der generierte C++-Code bindet automatisch `katana/runtime/runtime.hpp`,
`katana/runtime/exception.hpp` und `katana/runtime/fpu.hpp` ein.

## Zentraler CPU-Zustand

`katana::runtime::CpuState` enthaelt die Architektur- und Runtime-Daten an einer
Stelle:

- 16 allgemeine Register und acht banked Register
- getrennte 16er-Rohbitbaenke `FR` und `XF`
- `PC`, `SR`, `GBR`, `VBR`, `SSR`, `SPC`, `SGR` und `DBR`
- `MACH`, `MACL`, `PR`, `FPUL` und `FPSCR`
- `TRA`, `TEA`, `EXPEVT` und `INTEVT`
- explizite T-, S-, Q- und M-Zustandsbits
- sichtbare Exception-, Delay-Slot- und Schlafzustaende
- den zentralen regionbasierten Speicherbus

Die FPU-Baenke bewahren 32-Bit-Rohwerte, damit `FMOV` Bitmuster unveraendert
uebertragen kann. `FPSCR.FR` tauscht die sichtbaren FR- und XF-Baenke zentral.
Die FPU-Runtime interpretiert dieselben Bits fuer Single- oder gepaarte
Double-Precision-Operationen; `FPSCR.PR`, `SZ`, `FR` und `RM` steuern
Rechenpraezision, Transferbreite, Banksicht und Rundung. R0 bis R7 verwenden
Bank 1 nur im privilegierten Modus bei gleichzeitig gesetztem `SR.RB`; im
User-Modus bleibt unabhaengig von RB Bank 0 sichtbar.

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
- standardmaessig natuerliche 2- und 4-Byte-Ausrichtung
- strukturierte Fehler fuer Ausrichtung, Adressraum, Regionen und Schreibschutz
- optionalen Schreibschutz pro Region
- auslesbare Regionsmetadaten fuer Tests und Diagnose
- optionale globale Traces und gefilterte Watchpoints

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

Das v0.22-Release-Gate prueft `dreamcast_vram_32bit_to_linear_offset`
exhaustiv fuer alle 8.388.608 Eingabe-Offsets. Eine unabhaengige
Bank-/Wort-Berechnung und eine Bijektivitaetspruefung sichern den kompletten
8-MiB-Bereich ab. Zusaetzliche Buszugriffe testen alle Bytepositionen beider
Baenke, Halfword- und Word-Grenzen, den Wechsel bei `0x00400000`,
fortlaufende Wortfolgen und alle 28 direkten 32-Bit-Aliase.

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

## Ausrichtung, Fehler und Watchpoints

KR-2206 verwendet standardmaessig `MemoryAlignmentPolicy::Strict`. Bytezugriffe
sind immer gueltig; Halfwords muessen auf zwei und Words auf vier Byte
ausgerichtet sein. `MemoryAlignmentPolicy::Permissive` bleibt fuer explizite
Diagnose- und Kompatibilitaetsfaelle verfuegbar.

Der historische lineare 1-MiB-Speicher in `CpuState` wird fuer bestehende
synthetische Semantik-Harnesses weiterhin explizit permissiv initialisiert.
Echte oder explizit erzeugte `Memory`-Busse bleiben standardmaessig strikt.
Der Gast-Exception-Pfad ueberfuehrt fehlgeschlagene Operandenzugriffe in den
passenden SH-4-Lese- oder Schreib-Adressfehler. Die genauere Hostursache bleibt
im ausloesenden `MemoryAccessError` fuer Bus- und Runtime-Diagnosen erhalten,
wird aber nicht mehr mit einem widersprechenden EXPEVT kombiniert.

Busfehler werden als `MemoryAccessError` mit maschinenlesbaren Metadaten
gemeldet:

- `Misaligned`
- `Unmapped`
- `CrossRegion`
- `ReadOnly`
- `AddressOverflow`

Jeder Fehler enthaelt Operation, Adresse, Zugriffsbreite und, sofern vorhanden,
den Regionsnamen. Die Pruefung erfolgt vor dem Geraetezugriff und vor jeder
Beobachterbenachrichtigung.

Ein globaler Trace-Handler kann jeden erfolgreichen Read oder Write beobachten.
Watchpoints filtern zusaetzlich nach Adressbereich und Zugriffstyp. Ereignisse
enthalten Operation, absolute Adresse, Breite, Wert und den tatsaechlich
aufgeloesten Regionsnamen, sodass auch Aliase unterscheidbar bleiben.
Fehlgeschlagene Zugriffe erzeugen weder Trace- noch Watchpoint-Ereignisse.
Observer werden vor dem Aufruf kopiert, damit sie Watchpoints waehrend eines
Callbacks sicher entfernen oder veraendern koennen.

## Ausnahmen und Interrupts

KR-2301 bis KR-2305 fuehren einen gemeinsamen CPU-Exception-Pfad ein. Relevante
Statusregisterfelder besitzen zentrale Masken und strukturierte Zugriffe:

- `IMASK` fuer die vierstufige Interruptmaske
- `BL` zum Blockieren maskierbarer Interrupts
- `MD` fuer den privilegierten Modus
- `RB` fuer die aktive Registerbank
- `FD` fuer die FPU-Sperrsemantik

`enter_exception` sichert den bisherigen Zustand in `SSR`, `SPC` und `SGR`,
setzt `MD`, `RB` und `BL`, schreibt das Ereignis nach `EXPEVT` oder `INTEVT`
und springt ueber `VBR` zum allgemeinen Exception- oder Interruptvektor.
Speicherfehler hinterlegen die fehlerhafte Adresse zusaetzlich in `TEA`.
`return_from_exception` restauriert `SR`, Registerbank und `PC` zentral.

KR-4611 leitet Exceptionursache, Eventcode, Vektor und Interruptklasse aus
einer gemeinsamen Metadatentabelle ab. `trap_pending` bezeichnet einen aktiven
Gast-Exceptionhandler und keinen fatalen Hostabbruch. Block-ABI 2 trennt
`ExceptionReturn` und `Sleep` von normalen Returns: Der Portdispatcher setzt
Exceptions und Interrupts am durch VBR bestimmten Handler fort, dispatcht RTE
zum restaurierten SPC und fuehrt waehrend SLEEP erst nach einem akzeptierten
Interrupt wieder Gastcode aus.

Der `InterruptController` verwaltet Pending-Quellen nach Quell-ID, Prioritaet
und Eventcode. Angenommen wird deterministisch der hoechste Level oberhalb von
`IMASK`; gleich priorisierte Quellen werden nach kleinerer Quell-ID geordnet.
Gesetztes `BL` verhindert die Annahme. Ein angenommener Interrupt beendet einen
sichtbaren Schlafzustand und verwendet den Interruptvektor `VBR + 0x600`.

Generierte Speicherzugriffe fangen `MemoryAccessError` direkt an der
verursachenden IR-Instruktion ab und rufen `enter_memory_exception` auf. Im
Delay Slot wird `SPC` auf den Owner-PC gesetzt und der Slotkontext explizit
markiert. Der Zustand propagiert durch generierte Funktionsaufrufe; CPU-Fehler
verlassen den Ausfuehrungspfad nicht mehr als generische C++-Exception.

Ist `SR.FD` gesetzt, nehmen FPU-Instruktionen sowie FPUL-/FPSCR-Transfers vor
jeder Zustands- oder Speicheraenderung den strukturierten FPU-Disable-Pfad.
Dabei werden `EXPEVT = 0x800` beziehungsweise im Delay Slot `0x820`, der
korrekte Owner-PC in `SPC` und der Slotkontext gesichert.

## FPU-Grundoperationen

KR-2401 bis KR-2405 implementieren `FMOV`-Register- und Speicherformen,
Konstanten und FPUL-Transfers, `FADD`, `FSUB`, `FMUL`, `FDIV`, `FMAC`,
`FABS`, `FNEG`, `FSQRT`, `FCMP/EQ`, `FCMP/GT`, `FLOAT`, `FTRC`, `FCNVDS`,
`FCNVSD`, `FRCHG` und `FSCHG`. Single- und Double-Ergebnisse verwenden eine
strikte Host-Floating-Point-Umgebung; Round-to-Nearest und Round-to-Zero werden
aus `FPSCR.RM` abgeleitet. NaN-Ergebnisse werden auf die SH-4-Bitmuster
kanonisiert. `FPSCR.DN` behandelt denormalisierte Single- und Double-Operanden
und -Ergebnisse als vorzeichenbehaftete Null; Rohbittransfers, `FNEG` und
`FABS` bleiben unveraendert. Unzulaessige Register- oder PR/SZ-Kombinationen
sowie die reservierten RM-Werte 2 und 3 werden vor einer Teilwirkung als
strukturierte illegale Instruktion gemeldet.

Die vollstaendige FPU-Exception-Flag-Semantik sowie die Vektor- und
Spezialoperationen `FSCA`, `FSRRA`, `FIPR` und `FTRV` gehoeren zum folgenden
v0.25.0-Meilenstein.

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
- Runtime-Tests fuer CPU-Zustand, Reset, Speicherbus, Ausrichtung, strukturierte Fehler, Traces, Watchpoints, breitenbewusste MMIO-Handler sowie Dreamcast-RAM-, VRAM-, AICA-RAM-, BIOS- und Flash-Aliase

## Eigenstaendiger Disc-Boot fuer Portanwendungen

`load_dreamcast_runtime_boot` oeffnet eine GDI read-only, validiert die
Dreamcast-Bootmetadaten, liest die benannte ISO9660-Bootdatei zweimal ueber
dieselbe `DiscSource` und lehnt leere, zu grosse oder instabile Eingaben ab.
`initialize_dreamcast_runtime` ersetzt den Legacy-Testbus durch die definierten
Dreamcast-Aliase fuer Hauptspeicher, VRAM, AICA-RAM und Flash, kopiert die
Bootdatei nach `0x8C010000` und setzt PC und Stack deterministisch.

Das Portprojekt registriert jeden generierten Funktionseinstieg als statischen
Runtimeblock. Die eigenstaendige Hostanwendung loest den Programmeinstieg ueber
`dispatch_indirect`, protokolliert die Entscheidung und ruft erst danach die
registrierte Backendfunktion mit validierten Plattformdiensten auf. Fehlende
Bloecke, Speicherabbildungen oder Bootquellen propagieren als Nichtnull-Exitcode;
der Aufruferpfad wird vor der Ausgabe redigiert.
