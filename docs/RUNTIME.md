# Katana Runtime

Die Katana-Runtime ist seit KR-2101 eine eigene statische Bibliothek. Generierter
C++-Code enthaelt keine Implementierung von Speicher, CPU-Zustand oder
ungeloesten Kontrollflusspfaden mehr.

## ABI

Die aktuelle Runtime-ABI ist Version `41`. Die typisierte Block-ABI ist
Version `3`; die Backend-Interface-ABI ist Version `3`.

Generierter Code enthaelt eine Compile-Time-Pruefung gegen diese Version. Eine
abweichende Runtime wird beim Kompilieren sichtbar abgelehnt. ABI-Version 3
kennzeichnete den Wechsel zum regionbasierten Bus, ABI-Version 4 fuehrte
breitenbewusste MMIO-Zugriffe ein. ABI-Version 5 erweitert `Memory` um
Ausrichtungsrichtlinie, strukturierte Zugriffsfehler, Trace-Handler und
Watchpoint-Zustand. ABI-Version 6 ergaenzt den zentralen CPU-Zustand um `TEA`,
strukturierte Exception-Ursachen und Delay-Slot-Kontext und bindet generierten
Code an den gemeinsamen Exception-Pfad. ABI-Version 7 zentralisiert die
`FPSCR`-Maskierung und die sichtbare FR-/XF-Bankumschaltung.
ABI-Version 8 ergaenzte den versionierten Plattform- und Portvertrag.
ABI-Version 9 macht Gastwrites samt Herkunft, Byteidentitaet und
Codeinvalidierung zu einem beobachtbaren `Memory`-Vertrag. ABI-Version 10
fuehrt generationsgesicherte Blockhandles und getrennte Registryindizes ein.
ABI-Version 11 bindet den gemeinsamen Gastzyklusvertrag ein. ABI-Version 12
versioniert Runtime-only-Dispatchklassen, Zielvalidierung und Maschinenmetriken.
ABI-Version 13 bindet projektbezogene Flash-/VMU-Arbeitskopien, geordnetes
Shutdown-Speichern und Host-Pacing ein. ABI-Version 14 fuehrt
generationsgebundene Runtime-Module und kontrollierte Materialisierung ein.
ABI-Version 15 erweitert den SH-4-Zustand um LDTLB-, Cache- und MMU-Vertraege.
ABI-Version 16 bindet den gastzeitgebundenen AICA-/Sound-RAM-Pfad ein.
ABI-Version 17 versioniert die gemeinsame Produktdiagnostik und den
erweiterten MMIO-/Runtimezustand. ABI-Version 18 bindet die bytegenaue
Runtime-Write-Provenienz und kontrollierte Haupt-RAM-Codepromotion in den
Modulkatalog ein. ABI-Version 19 erweitert den oeffentlichen DMAC-Zustand um
`DMAOR.DDT`, begrenzte On-Demand-Requestqueues und den TR-only-Wiederholpfad.
ABI-Version 20 trennt PVR-Core-, TA- und SDRAM-Softreset vom Power-on-Reset
und stellt den gastzeitgebundenen Scanout nach Runtime-/Schedulerreset wieder her.
Die kumulativen Versionen 21 bis 32 binden die anschliessenden SH-4-, MMU-,
DMA-, PVR-, Boot-, Modul- und AOT-Vertraege; ihre einzelnen Aenderungen sind im
Changelog nachvollziehbar. ABI-Version 33 erweitert den oeffentlichen
GD-ROM-Produktzustand und macht ausstehende Async-Reads explizit abbrechbar.
ABI-Version 34 versioniert die erweiterten GD-ROM-Streaming- und
Callbackgrenzen. ABI-Version 35 bindet aktive Modulextents sowie den
scanoutgebundenen PVR-Framebeweis in den oeffentlichen Runtimevertrag ein.
ABI-Version 36 ergaenzt den oeffentlichen DMAC-Pruefvertrag um die erwartete
Requestquelle und bindet Copy-/DMA-Loadbeobachtung an die atomare
Veroeffentlichung geladener ausfuehrbarer Bereiche.
ABI-Version 37 bindet die exakte externe Dispatchfortsetzung an den letzten
tatsaechlich ausgefuehrten Block einer lokalen AOT-Kette. Terminatorquelle,
Callsite, Call-/Tail-Jump-Art und statische, tabellenbasierte, Runtime-only-
oder Fallback-Siteklasse bleiben dadurch bis zum folgenden Lookup und seiner
Diagnostik erhalten.
ABI-Version 38 bindet den typisierten G1-DMA-Fault mit Phase, Fehleradresse,
committed Praefix und Residue an den GD-ROM-Requestzustand. Sie versioniert
ausserdem die MMU-abhaengige Store-Queue-`PREF`-Uebersetzung zwischen QACR und
UTLB. Der generierte native Code behandelt `PREF` unabhaengig vom
adressabhaengigen IR-Speichereffekt als moegliche `MemoryAccessError`-Quelle,
sodass Alignment-, SQMD- und TLB-Fehler ueber `enter_memory_exception` laufen.
ABI-Version 39 bindet source-relativierte native AOT-Templates und den
scanoutgebundenen Direct-Framebuffer-Beweis in den kumulativen
Runtimevertrag. Block-ABI 3 versioniert die zugehoerige virtuelle
Quell-/Laufzeitadressabbildung.
ABI-Version 40 versioniert den oeffentlichen begrenzten und standardmaessig
redigierten Systemreplay-Vertrag. Kapazitaet, Dropstatus und Redaktionsmodus
sind damit Bestandteil der Runtime-Schnittstelle.
ABI-Version 41 bindet MMU-bewusste lineare Gastpeeks, allokationsfreies
Last-MMIO-Tracking und nicht mutierende PVR-/Systembus-Snapshots in den
oeffentlichen Diagnosevertrag. Der Portprojektvertrag 25 verwendet diese
Schnittstellen im generierten terminalen Fortschrittsbericht.
ABI-Version 42 bindet den seiteneffektfreien POD-Zugriffssink,
Quell-/Laufzeit-PC-Provenienz und `RuntimeWaitLoopTrace` Version 1. AOT und der
begrenzte Diagnoseinterpreter teilen den Herkunftsvertrag; Store-Queue-`PREF`,
PVR-Render und PVR-YUV bleiben unterscheidbare Writer, waehrend VRAM32 auf das
gemeinsame lineare Backing projiziert wird. PlatformServices-ABI 10 reicht die
exakte `PREF`-Instruktionsherkunft bis zur Store Queue. Portprojektvertrag 26
versioniert die generischen Auditdeskriptoren und das explizite Trace-Opt-in.

## CMake

Innerhalb des KatanaRecomp-Builds steht das Ziel `KatanaRecomp::runtime` zur
Verfuegung:

```cmake
target_link_libraries(mein_programm PRIVATE KatanaRecomp::runtime)
```

Der generierte C++-Code bindet automatisch `katana/runtime/runtime.hpp`,
`katana/runtime/exception.hpp` und `katana/runtime/fpu.hpp` ein.

Ein statisches Runtime-SDK aus einem Sanitizer-Build exportiert sein
Sanitizerprofil als `INTERFACE`-Usage-Requirement von
`KatanaRecomp::runtime`. Ein installierter Out-of-Tree-Consumer erbt dadurch
insbesondere unter MSVC dieselben ASan-STL-Annotationen; GNU- und
Clang-Consumer erben die passenden ASan-/UBSan-Compile- und Linkoptionen.
Der Paketvertrag ist sowohl fuer das sanitizte als auch fuer das
nicht-sanitizte Profil mit einem konfigurierten, kompilierten und gelinkten
Consumer geprueft.

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
- einen gemeinsamen Gastwrite-Observer mit Byteidentitaet und Herkunft

Der vollstaendige Write- und Invalidierungsvertrag steht in
[`GUEST_WRITES.md`](GUEST_WRITES.md).

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

Die P4-Cachefenster bilden die SH-4-IC-/OC-Adressarrays bei `0xF0000000` und
`0xF4000000` sowie die Datenarrays bei `0xF1000000` und `0xF5000000` ab.
Adresswrites verwenden die dokumentierten Entry-, Association-, Tag-, U- und
V-Felder; Datenzugriffe und alle vier Aperturen akzeptieren ausschliesslich
ausgerichtete 32-Bit-Zugriffe. `CCR.ICI` loescht die IC-Validbits und bleibt
selbstloeschend.

## Store-Queue-PREF und MMU

Der produktive Store-Queue-Pfad bindet `PREF` an den aktuellen
`RuntimeAddressSpace`. Bei `MMUCR.AT=0` entsteht das Ziel wie bisher aus QACR;
bei `AT=1` liefert die passende UTLB-Abbildung die physische Zieladresse. Die
32 Queuebytes bleiben in beiden Modi unveraendert.

Vor jeder Sinkwirkung werden SQ-Fenster, Longwordausrichtung,
`MMUCR.SQMD`, Privileg, ASID/SV, Schreibberechtigung und Dirty-Bit geprueft.
Fehlende oder mehrdeutige UTLB-Eintraege werden als typisierte Write-TLB-
Exception mit korrektem `TEA`, `PTEH` und Miss- beziehungsweise Resetvektor
weitergereicht. Kein Fehler darf TA-FIFO, RAM oder Transferzaehler teilweise
veraendern. Derselbe Vertrag gilt auch dann, wenn ein synthetischer CPU-Zustand
noch keinen expliziten `RuntimeAddressSpace` besitzt; aktive MMU faellt dort
nicht still auf QACR zurueck.

## Seiteneffektfreier Runtime-Wait-Loop-Trace

`Memory` kann einen rohen POD-Sink fuer den bereits ausgefuehrten Zugriff
benachrichtigen. Das Ereignis enthaelt Operation, virtuelle und physische
Adresse, Breite, skalaren Wert, Quell- und Laufzeit-PC, Retirementstand,
Writequelle und, wo vorhanden, die Projektion auf ein lineares Backing. Der
Sink fragt einen beobachteten Readwert nicht erneut ab und ruft keinen
zusaetzlichen MMIO-Handler auf. Das gilt auch fuer MMIO: Der Sink uebernimmt
den bereits beobachteten Wert des ausgefuehrten Zugriffs. Nur fuer die
No-op-Klassifikation eines Writes durch einen nichtlinearen Wrapper darf der
aktivierte Trace vor dem Write die seiteneffektfreie lineare Projektion lesen.
Der Produkt-`GuestWriteObserver` und damit der Scanout-Dirty-Vertrag bleiben
fuer solche Wrapper bewusst konservativ und bei Trace aus/an identisch.
Store-Queue-`PREF` reicht seine
Instruktionsherkunft ueber
PlatformServices-ABI 10 weiter. PVR-Render- und YUV-Writes melden getrennte
Writer-Urspruenge. Die logische VRAM32-Abbildung projiziert ihre einzelnen
Bytes auf dasselbe VRAM-Backing und bleibt dadurch mit RAM-/VRAM64-Writes
vergleichbar.

`RuntimeWaitLoopTrace` v1 verknuepft tatsaechlich ausgefuehrte Read-Sites mit
begrenzten Wertlaeufen und ueberlappenden Writern. Der Portexport berechnet den
Hardwareaudit genau einmal und leitet daraus deterministisch deduplizierte
`ProvenGuard`-, `UnresolvedGuard`- und konservative Kandidatendeskriptoren ab;
reine Counterloops werden nicht instrumentiert. Ein vorab sortierter Index
ordnet ausgefuehrte Read-Sites ihren Deskriptoren zu, ohne pro Zugriff alle
Deskriptoren linear zu durchsuchen. Aktive lineare Locations sind ausserdem
nach Backing indiziert, sodass ein Write auf ein unbeteiligtes Backing ohne
Location-Vollscan verworfen wird. Der aktive Trace berechnet seine
Writer-Aenderung fuer skalare und Range-Wrapperwrites bytegenau, fasst nur
Abschnitte mit gleichem Aenderungszustand zusammen und verwirft No-op-Writer.
Diese Trace-Evidenz veraendert die konservative Produkt-/Scanout-Evidenz
nicht. `guest_memory_access_change_tracking_limit` begrenzt die
Trace-Aenderungsmap auf 1 MiB; bis 256 Byte liegen inline, erst darueber ist
innerhalb dieses Limits eine Allokation noetig. Scheitert sie oder ist der
Range groesser, wird der Gastwrite trotzdem vollstaendig nach seinem normalen
Vertrag ausgefuehrt. Der Sink erhaelt stattdessen ein ungueltiges Event und
markiert den Trace ueber `invalid_access_events` und `complete:false` als
unvollstaendig. Der PVR-Renderer prueft die Trace-Aktivitaet einmal am
Renderanfang und nicht pro Pixelstore.

Strukturell ungueltige Access-Events, darunter lineare Projektionen mit
mehr als den vier im Ereignis abbildbaren Byteoffsets, erhoehen
`invalid_access_events` und erzwingen `complete:false`; sie zaehlen nicht als
bloss ignorierte gueltige Events. Scheitert beim Anlegen oder Indizieren einer
aktiven Location eine Allokation, faengt der `noexcept`-Callback sie ab,
erhoeht `dropped_locations` und markiert die Ausgabe damit ebenfalls als
unvollstaendig, statt den Prozess zu beenden.

Ein Wertlauf kennzeichnet die Writerkorrelation als
`writer_link_kind:"exact-backing-bytes"`, wenn sich die nachgewiesenen
linearen Backing-Bytes ueberschneiden. Nichtlineare Zugriffe wie MMIO koennen
nur ueber ihren physischen Bereich korreliert werden und tragen deshalb
`writer_link_kind:"physical-range-candidate"` statt eines Beweises. Es gibt
weder Titeladressen noch private Inhalte im Vertrag. Jeder serialisierte
Writer traegt zusaetzlich `instruction_valid`; `source_pc` und `runtime_pc`
sind nur bei gesetztem Marker als Instruktionsherkunft gueltig.

Nur der exakte Wert `KATANA_PORT_WAIT_LOOP_TRACE=1` konstruiert diesen
Rohwertrecorder, unabhaengig vom breiteren Schalter
`KATANA_PORT_DIAGNOSTICS`. Bei einer leeren Deskriptorliste werden weder
Recorder noch Sink erzeugt. Bei tatsaechlicher Aktivierung warnt der Port
einmalig auf `stderr`: Die Ausgabe ist nur lokal bestimmt, enthaelt rohe
Gastwerte und darf nicht ungeprueft geteilt werden. Das JSON deklariert
`contains_raw_guest_values:true` und
`writer_scope:"since-previous-sample"`; fuer Range-Writes ohne gueltigen
Skalarwert stehen `scalar_value_valid:false` und `value:null`. Der
`invalid_access_events`-Zaehler ist Teil desselben terminalen Schemas. RAII
entfernt den Sink vor jeder terminalen `KATANA_WAIT_LOOP_TRACE`-JSON-Ausgabe, auch beim
kontrollierten Lifecycle- oder Fehler-Unwind. Ohne dieses Trace-Opt-in bleiben
Recorder und Speicherprojektion aus dem Fastpath.

Der begrenzte Interpreter deckt die Registervarianten von `PREF`, `OCBI`,
`OCBP`, `OCBWB` und `TAS.B` ab. Doppelte `FMOV`-Speicherzugriffe erfolgen in
der Reihenfolge low nach high. Diese Diagnoseunterstuetzung aendert nicht die
Produktgrenze: Der normale Port fuehrt weiterhin ausschliesslich AOT-Code aus.

## G1-/GD-ROM-DMA-Faultvertrag

G1-DMA prueft vor dem ersten Schedulerevent die kodierte Laenge, Ausrichtung,
Richtung, den 32-Bit-Endadressueberlauf, das GDAPRO-Schutzfenster und die
vollstaendige beschreibbare Gastspanne. Ein Fehlerobjekt enthaelt Start- oder
Chunkphase, exakte Fehleradresse, bereits committed Bytezahl und Residue. Vor
seiner Beobachtung werden das geplante Event, `active` und alle spaeten
Fortsetzungen geloescht.

Der gebundene GD-ROM-Controller uebersetzt diesen Zustand in seinen
Taskfile- oder BIOS-Requestvertrag: CHECK/ABRT beziehungsweise stabiler
Sensezustand, exakte uebertragene Bytezahl, kein ausstehender Gastcallback und
keine falsche DMA-Completion. Nur echte Hardwarevertragsfehler duerfen das
passende ASIC-Fehlerereignis ausloesen; interne Backend-, Scheduler- oder
Observerfehler bleiben typisierte Host-/Runtimefehler und simulieren kein
Hardwareereignis.

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

Der produktive SH-4-Bus spiegelt `TRA`, `EXPEVT` und `INTEVT` als
32-Bit-R/W-Register bei `0xFF000020..0xFF000028` sowie unter ihren
Area-7-Aliasadressen. Beide Sichten verwenden direkt denselben `CpuState` wie
`TRAPA`, Exception- und Interruptannahme. `TRA` behaelt Bits 9:2, die beiden
Eventregister ihren 12-Bit-Code; andere Bits lesen null. Schmalere oder
unausgerichtete Zugriffe werden abgewiesen. Grundlage ist der in
[REFERENCE_PROVENANCE.md](REFERENCE_PROVENANCE.md) ausgewiesene
Renesas-SH7750-Hardwarevertrag.

KR-4611 leitet Exceptionursache, Eventcode, Vektor und Interruptklasse aus
einer gemeinsamen Metadatentabelle ab. `trap_pending` bezeichnet einen aktiven
Gast-Exceptionhandler und keinen fatalen Hostabbruch. Der mit Block-ABI 2
eingefuehrte und in Block-ABI 3 fortgefuehrte Vertrag trennt
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
- optionales leichtgewichtiges Last-MMIO-Tracking fuer begrenzte Produktprobes;
  bei Aktivierung speichert der Gast-Hotpath nur Operation, Adresse, Breite,
  Wert und Regionsbasis in einem allokationsfreien POD. Der owning
  Regionsstring entsteht erst beim terminalen Abruf; ohne Aktivierung bleibt
  der normale Speicherhotpath frei von Trace-Callbacks
- MMU-bewusste freie 32-Bit-Probes ausschliesslich auf echten linearen
  Haupt-RAM-, VRAM- und AICA-RAM-Backings; `peek_guest_u32` uebersetzt ueber
  die aktuelle Gast-MMU ohne Exception- oder CPU-Mutation. `Memory::peek_u32`
  lehnt Flash und MMIO auch bei einer fehlerhaften expliziten Whitelist vor
  jedem Geraetehandler ab und beruehrt weder Observer, Watchpoints noch
  Lookup-/Referenzzaehler
- strukturierte `PvrRegisterFile::snapshot()`- und
  `DreamcastSystemBusControl::snapshot()`-Zustaende fuer Produktdiagnostik;
  auch pending Rendercompletions und aktive Channel-2-Transfers bleiben
  unveraendert und werden weder gepumpt noch quittiert
- deterministische Systemreplays unter Schema 2 mit standardmaessig 4.096 und
  maximal 65.536 Ereignissen sowie hoechstens 64 Zeichen langen Ereigniscodes;
  eine Saettigung zaehlt genau einen Drop je abgewiesenem Ereignis und
  verhindert Versiegelung und Replay. Ereigniswerte bleiben intern exakt,
  waehrend Standard-JSON Code, Adresse, Wert, numerische Nutzlasten,
  Ereignishash und Endzustandshash ohne ausdrueckliches lokales Opt-in
  redigiert

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

`ExecutableLoadWriteTracker` beobachtet unmittelbare Copy-/DMA-Gastwrites eines
BIOS-/GD-Reloads ueber deren kanonische physische Aliase. Ein anschliessend
publizierter, byteidentischer Bereich invalidiert keine registrierten nativen
AOT-Bloecke. Bereits bewiesene Modulabdeckung wird nicht dupliziert; ein
frischer, zufaellig schon bytegleicher Bereich erhaelt dagegen seinen ersten
Provenienznachweis. Haben sich Bytes geaendert, hat der gemeinsame Gastwrite-
Pfad die betroffenen Bloecke bereits exakt einmal invalidiert; die
Modulveroeffentlichung wiederholt diesen Schritt nicht. Ein nicht beobachteter
Load behaelt die konservative vollstaendige Invalidierung.

BIOS-PIO loest die komplette virtuelle Zielspanne vor dem ersten Write ueber
die aktive Gast-MMU auf und meldet nur eine durchgehend lineare, wirklich
geschriebene physische Range. Nichtlineare TLB-Spannen werden ohne Partialwrite
abgelehnt. Die vier physischen Area-3-Sichten `0x0C` bis `0x0F` werden fuer
Codeprovenienz und Invalidation auf dasselbe 16-MiB-Haupt-RAM-Backing gefaltet;
ein Write ueber eine Spiegelgrenze wird dafuer in Tail und Head geteilt.

Der normale Produktport deaktiviert Demand-Interpreterausfuehrung vollstaendig:
Nicht gebundener Code endet als typisierter Materialisierungs-/Dispatchfehler.
Nur das explizite `diagnostic_partial`-Profil emittiert den begrenzten
Diagnoseinterpreter. Der Portprojektvertrag `26` weist Profil,
Interpreterstatus, Unbound-Code-Policy und Coverage-Vertrag im Manifest aus
und verwendet fuer freie Speicherprobes sowie PVR-/Systembuszustand nur die
seiteneffektfreien Runtime-ABI-42-Diagnoseschnittstellen.

Im produktiven Einblock-AOT-Pfad zaehlt `CpuState::retired_guest_instructions`
jede betretene Gastinstruktion. Schedulerzeit wird nach der ausgefuehrten
Blocksemantik verbucht; erst der anschliessende Safepoint darf einen Interrupt
annehmen. Faulting Instructions und ausgefuehrte Delay Slots gehen dadurch in
die Zeit ein, nicht ausgefuehrte Blockreste dagegen nicht. Lokales Chaining ist
nur ohne faelliges Schedulerereignis und bei identischem Code-, MMU-,
Watchpoint-, FPSCR- und Runtimezustand erlaubt; ein Chunk umfasst hoechstens 64
Instruktionen. Jeder tatsaechlich betretene Block aktualisiert dabei die
Fortsetzungsmetadaten. Verlaesst eine lokale Kette den Wrapper, verwendet der
externe Dispatcher daher die Terminatorquelle und Siteklasse des letzten
ausgefuehrten Blocks und nicht den urspruenglichen Wrapper-Einstieg. Der
generische C++-Emitter setzt auch bei einem durch Funktionsdiscovery
nachfolgerlosen Block in jedem Backendmodus `PC` auf die Folgeadresse der
letzten Gastinstruktion. Die Produktinvariante prueft einen Fallthrough relativ
zu dieser tatsaechlichen Terminatorquelle und nicht zum Eintritt des
umgebenden Wrappers. Der kumulative Stand verwendet Runtime-ABI 42, Block-ABI 3,
Backend-Interface-ABI 3, PlatformServices-ABI 10, Portvertrag 26 und
Host-Video-Vertrag 2.

Statische Dispatchregistries werden nicht mehr in eine einzelne
Uebersetzungseinheit geschrieben. Der Projektschreiber teilt sie
deterministisch in Shards zu maximal 512 Bloecken. Jeder Funktionsowner besitzt
in einem Shard genau einen Wrapper, auch wenn mehrere seiner Bloecke dort
liegen; ein balancierter Router ordnet die Zieladresse dem passenden Shard zu.
Bei einer erneuten Ausgabe werden nicht mehr benoetigte Sharddateien entfernt,
damit ein verkleinerter Port keinen stale Code mitbaut.

Die Grenzregression erzeugt 513 Bloecke, erwartet genau zwei Shards und prueft
das anschliessende stale Cleanup. Ein vollstaendiges synthetisches
Ninja-/MSVC-Projekt kompiliert und linkt damit in 15 Sekunden. Beim aktuellen
privaten PAL-Nachweis bestehen die sechs fokussierten Regressionstargets 6/6.
Die Registry dieses PAL-Ports umfasst 43 Shards; die zentrale
`runtime-dispatch.cpp` misst 34.879 Byte und 607 Zeilen statt zuvor 36.703.886
Byte und 525.996 Zeilen. Der groesste Shard misst 393.454 Byte. Diese
Aufteilung aendert weder Blockidentitaet noch Lookup-, Guard- oder
Fortsetzungssemantik; sie ist ausschliesslich ein Buildzeit- und
Uebersetzungseinheitenvertrag.

Die kompilierte Fallthroughregression deckt Einzelblock, lokales Chaining und
normalen Backendpfad ab. Das aktuelle fokussierte Kern-Gate besteht 11/11. Der
x64-Debug-Kern-/Runtime-Build der Desktop-GUI-off-Konfiguration mit ASan,
konfiguriertem MSVC-Coverage-Backend und `/analyze /WX` ist mit zwoelf
parallelen Jobs gruen; das anschliessende vollstaendige CTest-Zwischengate
besteht 183/183 Eintraege in 312,97 Sekunden, darunter 181 regulaere Passes und
zwei erwartete Regex-`PASS_REGULAR_EXPRESSION`-Erfolge. Ein Coverage-Bericht
wurde in diesem direkten CTest-Lauf nicht erhoben; Desktop-GUI- und Harness-
Tests sind nicht Teil der 183. Dieser Nachweis schliesst weder `KR-4852` noch
`KR-4853` oder die interne Freigabe `KR-4854`.

Die konsolidierten fokussierten Regressionen des nachfolgenden
KR-4842-Zwischenblocks bestehen 22/22 in 1,57 Sekunden; der generierte
Port-CLI-Pfad besteht 1/1 in 151,12 Sekunden. Dafuer wurde keine Vollsuite und
kein `KR-4852` ausgefuehrt.
Dynamische Wertlaeufe und Runtime-Writer-Provenienz sind damit abgedeckt.
`KR-4842` bleibt ausschliesslich fuer den vollstaendigen
Diagnose=0/1-A/B-Produktlauf offen.

Der optimierte ABI-38-Export der privaten PAL-Testbench dauerte mit zwoelf Jobs
140,9 Sekunden, der inkrementelle Reexport 29,2 Sekunden. Die lokale
Discinstallation war erfolgreich; das Portpaket enthaelt null Retailsektoren
und die Originalquelle blieb erhalten. Der normale Produktlauf erreicht
345.609.251 Gastzyklen und den echten SH-4-Interruptpfad `VBR + 0x600`, meldet
aber weiterhin null Frames, TA-Transfers und Gast-PVR-Frames. Eine getrennte
begrenzte Diagnose beweist ueber Gastwriteprovenienz ein 56-Byte-Copy-plus-
Patch-Codetemplate und fuehrt daraus 19 bytebewiesene Runtimeinstruktionen aus,
bevor der naechste noch nicht statisch gebundene AOT-Einstieg kontrolliert
stoppt. Dieser Diagnosepfad fuegt weder Retailbytes noch eine titelbezogene
Adresse zum Produktvertrag hinzu; `KR-4848` bleibt offen.

Der nachfolgende Runtime-ABI-39-/Block-ABI-3-Produktpfad bindet die
source-relativierten AOT-Templates unter Portprojektvertrag 24. Ein privater,
budgetierter Sonic-Adventure-PAL-AOT-Lauf erreicht aus dem recompilierten
`IP.BIN`-Direct-Framebuffer nach 50 Millionen Gastzyklen in 5,3 Sekunden
`KR_FIRST_GUEST_FRAME` und `KR_FIRST_PRESENTED_FRAME`; TA bleibt null und der
anschliessende Budget-Exit ist erwartet. BootExecutable, Spielboot, die
strukturierten `KR-4848`-Disc-Ladetransaktionen und der allgemeine Materializer
bleiben offen.

Nach dem frueheren 178/178-Gate wurde der damalige
Vertrag-24-/Runtime-ABI-39-Port frisch neu exportiert und gebaut. Er umfasst
1.860 Funktionen, 37 Codepartitionen und null
Retailsektoren; die lokale read-only Originaldisc-Installation umfasst drei
Tracks und 521.461 Sektoren. Der abschliessende 50-Millionen-Lauf reproduziert
beide Framemarker mit zwei Gast-/Direct-FB-Frames und 302.287 geaenderten
Direct-FB-Pixeln. TA, Rendergeneration und Materializer bleiben null.
