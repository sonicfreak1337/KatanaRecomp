# KatanaRecomp-v0.49-Architektur

KatanaRecomp ist ein statischer Recompiler fuer Dreamcast-SH-4-Programme. Der
Produktpfad ist:

```text
Dreamcast-Programm
  -> SH-4-Analyse
  -> Katana-IR und Optimierung
  -> natives C++
  -> Hostcompiler
  -> natives Spielprojekt
```

Ein normaler Port enthaelt keinen allgemeinen SH-4-Interpreter, keinen JIT und
keinen Emulationsfallback. Nicht vorab kompilierter oder nicht mehr gueltiger
Code endet an einer typisierten Runtimegrenze. Der begrenzte
Diagnoseinterpreter ist nur Bestandteil eines ausdruecklich als
`diagnostic_partial` erzeugten Diagnoseports.

## Drei Ebenen

### KatanaRecomp

Der Werkzeugkern ist titelunabhaengig und verantwortlich fuer:

- SH-4-Decoder und Kontrollflussanalyse;
- Funktions-, Block-, Jump-Table- und Callbackerkennung;
- Katana-IR, Optimierungen und statische C++-Codegeneration;
- partitionierte, reproduzierbare AOT-Quellen und Metadaten;
- stabile allgemeine Hook-, Patch- und Spielprojektvertraege.

Titeladressen, Discidentitaeten, private Symbole, Rendererpatches und
spielbezogene Installerlogik gehoeren nicht in diesen Kern.

### KatanaRuntime

Die installierbare Runtime stellt die gemeinsamen Dreamcast-Grenzen bereit:

- CPU-Zustand und sein Vertrag an Runtime- und Architekturgrenzen;
- Haupt-RAM, VRAM, AICA-RAM, OCRAM, MMIO, MMU und Aliase;
- Scheduler, Gastzeit, Interrupts, Exceptions und BIOS-Dienste;
- GD-ROM, PVR/TA, AICA, Maple, Eingabe, Video, Audio, Save und VMU.

`KatanaRecomp::runtime_core` ist der interpreterfreie Produktvertrag.
`KatanaRecomp::runtime` enthaelt zusaetzlich den expliziten
Diagnoseinterpreter und darf nicht versehentlich in einen normalen Port
gelinkt werden.

### Externes Spielprojekt

Ein separates Projekt darf versions- und hashgebunden enthalten:

- explizite Funktionsgrenzen, Jump Tables und Callbacktabellen;
- bekannte Runtimecode-Templates;
- schwache oder erforderliche native Funktionsoverrides;
- bedingte Mid-Function-Hooks mit Continue-, Jump-, Return- oder Abort-Aktion;
- titelbezogene Symbole und optionale Direct-Boot-Konfiguration.

Die Schnittstelle validiert Identitaet, Sortierung, Adressbereiche und
Kontrolltransfervertraege fail-closed. Sie kopiert durch das Binden einer
Definition keine Titeldaten in KatanaRuntime.

Der oeffentliche C++-Vertrag liegt in
`katana/runtime/game_project.hpp`; der Export nimmt ihn ueber
`PortExportOptions::game_project` entgegen. v0.49 liefert dafuer noch kein
serialisiertes CLI-Descriptorformat und kein Spielprojekt-Scaffold. Das
externe Projekt muss Definition, Callbackcode und Registrierung selbst in
sein Portbinary integrieren.

## Statischer und dynamischer AOT-Dispatch

Der Static AOT Fast Tier ist fuer nach dem Seal unveraenderliche native
Bloecke bestimmt. Eine kompakte zweistufige Tabelle bildet kanonische
Codepages und Halfword-Offsets direkt auf validierte AOT-Eintraege ab. Der
Caller fuehrt den bereits aufgeloesten Funktionszeiger aus; ein zweites
`RuntimeBlockTable::resolve` ist nicht erforderlich.

Der Dynamic AOT Tier bleibt fuer Runtimecode, Overlays, Module,
MMU-Varianten, Relocationen, Invalidierungen und Materialisierung
verantwortlich. Sein Ausfuehrungsdeskriptor traegt Blockhandle,
Funktionszeiger, virtuelle und physische Herkunft, Groesse, Variantenschluessel,
Endklasse, Runtime-Registrierung, optionalen Fastpath und die erforderlichen
Generationen.

Direkt abgebildete P1-/P2-Ziele pruefen zuerst den callsitegebundenen
Inline-Cache. Nur P0-/P3-/MMU-gemappte Ziele benoetigen immer die vollstaendige
Uebersetzung. Ein Cachetreffer bleibt an Adressraum-, MMU-, Runtime-, FPU-,
Code-, Modul-, Relocation- und Blockgeneration gebunden.

## Function-Level-AOT

Der Produkt-Emitter gruppiert analysierte Gastfunktionen in native
C++-Funktionen. Interne Basic Blocks werden Labels, interne Kanten native
`goto`- beziehungsweise strukturierte Kontrollfluesse. Bewiesene direkte Calls
und eindeutige, live verglichene Callbackziele koennen andere AOT-Funktionen
direkt aufrufen.

Direkte Gastcalls verwenden einen threadlokalen Tiefenwaechter. Wird das
konservative Limit erreicht, bleibt `cpu.pc` auf dem bereits vorbereiteten
Gastziel und der Hoststack wickelt zum statischen Zentraldispatcher ab. Das ist
weder Interpretation noch Laufzeitdekodierung.

Registerlokalisierung und direkte RAM-Zugriffe sind konservative
Beweisoptimierungen. Die aktuelle Lokalisierung umfasst ausgewaehlte mehrfach
verwendete `r0` bis `r15` ausschliesslich in reinen Leaf-Funktionen ohne
Speicher-, FPU-, Spezialregister-, privilegierte oder architektonische
Grenzoperation. Direkte RAM-Zugriffe brauchen einen statisch bewiesenen
P1-/P2-Haupt-RAM-Zugriff und einen aktuellen Laufzeitguard. Ein Guardmiss
verwendet den allgemeinen korrekten Speicher- oder Dispatchpfad.

## Gastzeit und Diagnose

Reine native Regionen sammeln Gastzyklen. Ein Commit ist vor MMIO,
Schedulerereignissen, annehmbaren Interrupts, Exceptions, SR-/IMASK-Aenderung,
expliziten Safepoints oder dem begrenzten Cycle-Quantum erforderlich.
Interruptmetadaten werden ereignisgetrieben ueber Epoch, Pending-Level und
Pending-Maske aktualisiert.

Produkt-Performance verwendet feste Aggregatzaehler. Detaillierte
RuntimeOnly-Sitemetriken und ihre Map werden nur im Diagnoseprofil, bei
expliziten Diagnoseschaltern oder in der Runtimeprobe aktiviert. Der
vorreservierte Dispatchrecorder ist im normalen Produktmodus nicht an den
Dispatcher gebunden; Formatierung erfolgt terminal. Fastpathdeskriptoren
werden anhand stabiler Gastadressen direkt ausgewaehlt und nicht bei jedem
zentralen Dispatch linear gescannt. Wait-Loop-Rohwerte und vollstaendige
Dispatchereignisse sind ausdrueckliche lokale Opt-ins.

Die immer aktive Crash Capsule ist ein fester POD-Zustand mit einem
16-Ereignis-Ring fuer letzten Block, MMIO-Zugriff, Schedulerereignis und ersten
Fehler. Ihr Aufzeichnungspfad verwendet keine Strings, Maps, Heapallokationen
oder Locks; erst ein terminaler Fehler formatiert eine Zusammenfassung. Ein
automatisch durch Gastzyklus/PC/Fehler aktiviertes und begrenztes
Deep-Trace-Fenster bleibt offene Diagnosekonsolidierung.

## Sicherheits- und Eigentumsgrenzen

- Keine Sonic- oder sonstigen Titeladressen im generischen Kern.
- Keine Retailbytes oder lokalen Pfade im Repository oder Portpaket. Das
  Portpaket enthaelt nur die fuer die Installation erforderlichen
  Hash-/Contentidentitaeten; titelbezogene Identitaeten bleiben ausserhalb des
  generischen Kerns.
- Keine still erfolgreichen No-op-Stubs fuer unbekannte Hardware.
- Kein Flycast-, dcrecomp- oder sonstiger uebernommener Emulatorcode.
- Runtimecode wird nur nach Byteidentitaet, Herkunft und Generation aktiviert.
- DirectBoot und NativeDiscBoot verwenden dieselbe Dreamcast-Runtime; sie
  unterscheiden ausschliesslich die Bootstrapgrenze.
