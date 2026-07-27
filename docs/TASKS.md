# KatanaRecomp Task-Katalog

Dieses Dokument enthaelt die aktiven v0.49-Produktaufgaben. Historische Detailtasks bleiben in Git und in `TASK_ID_REGISTRY.md` nachvollziehbar.

## Verbindliche Regeln

- Oberste Prioritaet ist ein lauffaehiger Sonic-Adventure-PAL-Produktport.
- Der echte Produktport ist die Bring-up-Abnahme.
- Bootkorrektheit wird bei festem Gastzyklusziel bewertet, Performance ueber die Hostzeit fuer genau dieses Ziel.
- DirectBoot besitzt keinen Sega-Screen als Pflichtmeilenstein; dieses Bild gehoert zu IP.BIN und damit NativeDiscBoot.
- Keine neue breite Testsuite, Funktionsmatrix oder vorsorgliche Regression.
- Ein kleiner Test ist nur fuer einen konkret beobachteten Produktfehler erlaubt, wenn er die naechste Produktiteration messbar verkuerzt.
- Keine Sonic-Adressen, Titelhooks oder Retailbytes im generischen Katana-Kern.
- Kein Interpreter, JIT oder Emulationsfallback im normalen Produktpfad.
- Dokumentation und Taskstatus werden erst nach dem echten Produktlauf aktualisiert.

## Aktueller Produktstand

Reviewbasis:

```text
aa1cd51655fdefec5a9891152487f902f91046c6
```

Aktuelle Messung:

```text
MSVC:     600.000.000 Gastzyklen / 14,8563 s / 40,3869 MHz
clang-cl: 600.000.000 Gastzyklen / 14,1289 s / 42,4662 MHz
zentrale Dispatches: 52.329.316
sichtbarer DirectBoot-Meilenstein: keiner
```

Der aktuelle DirectBoot startet die Haupt-Executable, nutzt aber einen Post-BIOS- statt eines bewiesenen Post-IP.BIN-/Game-Entry-Handoffs. Gleichzeitig verlaesst der neue Function-AOT-Pfad seine nativen Regionen viel zu haeufig.

## Empfohlene Reihenfolge

```text
KR-4951
  +--> KR-4952 -> KR-4953 -> KR-4954 -> KR-4955 -> KR-4961 -> KR-4962
  +--> KR-4956 -> KR-4957 -> KR-4958 -> KR-4959 -> KR-4960

KR-4963 parallel nach KR-4951

KR-4960 + KR-4961 + KR-4962 + KR-4963
  -> KR-4964
```

---

## [ ] KR-4951 - Produktgate nach Gastzyklen und getrennte visuelle Meilensteine

Prioritaet: P0

Abhaengigkeiten: keine

### Umfang

- hostzeitbasiertes Drei-Sekunden-Bootgate entfernen
- ein festes Gastzyklusziel fuer vergleichbare Produktlaeufe verwenden
- Host-Watchdog nur gegen echten Hanger verwenden
- NativeDiscBoot- und DirectBoot-Meilensteine trennen
- mindestens folgende Marker unterscheiden:
  - `BootExecutableEntry`
  - `GameCodeProgressed`
  - `FirstGameFramebufferWrite`
  - `FirstTaFrame`
  - `FirstVisibleGameFrame`
  - `TitleScreen`
- Produktbericht um Gastzyklen, Hostzeit, effektive MHz und Zentraldispatches ergaenzen
- keine Diagnoseoption aktivieren, die den normalen Produktpfad veraendert

### Akzeptanz

- derselbe Gastzyklus-Checkpoint wird fuer Vorher-/Nachhervergleiche verwendet
- DirectBoot gilt nicht wegen eines fehlenden Sega-Screens als gescheitert
- NativeDiscBoot darf das Sega-Bild weiterhin als eigenen IP.BIN-Meilenstein melden
- der reale Produktport wird mindestens bis 600.000.000 Gastzyklen ausgefuehrt

---

## [ ] KR-4952 - Post-IP.BIN-Spielhandoff fuer DirectBootExecutable

Prioritaet: P0

Abhaengigkeiten: KR-4951

### Problem

`DirectBootExecutable` startet bei `0x8C010000`, verwendet aber einen fest codierten `DreamcastPostBiosCpuState`. IP.BIN liegt zwischen BIOS und Haupt-Executable und erzeugt einen eigenen Game-Entry-Zustand.

### Umfang

- `PostBiosCpuState` und `GameEntryHandoff` als getrennte Vertraege modellieren
- den DirectBoot-Vertrag auf einen Post-IP.BIN-/Game-Entry-Zustand umstellen
- alle architektonisch sichtbaren CPU-Felder erfassen:
  - R0-R15 und Bankregister
  - PC, PR, SR, GBR, VBR, DBR
  - MACH, MACL, FPUL, FPSCR und erforderliche FPU-Zustaende
  - SSR, SPC, SGR und Exceptionzustand
- RAM-Handoff als typisierte Operationen beziehungsweise private Seitenreferenzen modellieren
- Geraetehandoff fuer mindestens PVR, GD-ROM/G1, DMAC, AICA, Maple, Systembus und Interruptzustand modellieren
- Scheduler-/pending-Eventzustand nur typisiert und reproduzierbar uebernehmen
- bestehenden NativeDiscBoot-Post-BIOS-Vertrag nicht umdeuten

### Akzeptanz

- DirectBoot verwendet keinen BIOS-Return-PR als unbelegten Game-Entry-PR
- der Handoff ist an Contentidentitaet, Bootdateihash, Konsolenprofil, Runtime-ABI und Handoff-Schema gebunden
- unvollstaendige oder inkompatible Handoffs werden vor Gastcode abgelehnt
- keine Retailbytes werden in das Repository geschrieben

---

## [ ] KR-4953 - Privates Game-Entry-Handoff-Artefakt aus Original-GDI

Prioritaet: P0

Abhaengigkeiten: KR-4952

### Umfang

- NativeDiscBoot bis zum ersten Kontrolltransfer in die BootExecutable-Range ausfuehren
- unmittelbar vor der ersten Haupt-Executable-Instruktion einen privaten Handoff erfassen
- einen deterministischen Diff gegen den normalen Runtime-Initialzustand erzeugen
- CPU-, RAM-, Geraete-, Scheduler- und Interruptzustand getrennt serialisieren
- Retaildaten nur als lokale Content-Slices beziehungsweise private Seitenartefakte speichern
- Manifest zuletzt schreiben und danach erneut validieren
- zwei identische Captures derselben GDI muessen bytegleich sein
- CLI-Befehle fuer Capture, Inspect und Verify bereitstellen

### Akzeptanz

- derselbe lokale Handoff kann einen DirectBoot-Lauf reproduzierbar starten
- Artefakt und Original-GDI bleiben ausserhalb des Repositorys
- ein Handoff einer anderen Discversion oder Runtime-ABI wird abgelehnt
- DirectBoot erreicht mindestens `GameCodeProgressed`

---

## [ ] KR-4954 - Deklaratives externes Spielprojekt und CLI-Scaffold

Prioritaet: P0

Abhaengigkeiten: KR-4952

### Umfang

- versioniertes TOML- oder JSON-Schema fuer `GameProjectDefinition`
- CLI-Laden, Validierung und Hashbindung
- symbolische Namen fuer native Overrides und Mid-Function-Hooks
- generiertes CMake-Scaffold fuer ein externes Spielprojekt
- installierte `KatanaRecomp`-Werkzeuge und `KatanaRecomp::runtime_core` verwenden
- kein `add_subdirectory` des vollstaendigen Katana-Quellbaums im normalen Spielprojekt
- lokale Contentwurzel und private generierte AOT-Quellen gitignored halten
- DirectBoot- und NativeDiscBoot-Konfiguration im Descriptor erlauben
- Game-Entry-Handoff-Manifest referenzieren

### Akzeptanz

- ein neues externes Projekt kann ohne eigenen C++-Exporter erzeugt werden
- Hooknamen werden beim Spielprojektbuild zu nativen Funktionen aufgeloest
- der nackte Descriptor enthaelt keine Retailbytes
- eine Runtime-only-Aenderung erzwingt keinen erneuten SH-4-Export

---

## [ ] KR-4955 - Explizite Funktionsgrenzen und Tabellenhinweise End-to-End

Prioritaet: P0

Abhaengigkeiten: KR-4954

### Problem

`GameProjectFunctionBoundary` besitzt eine Groesse, der aktuelle Analyzer-Override uebernimmt aber nur die Startadresse. Der Xenon-artige Titelvertrag ist dadurch unvollstaendig.

### Umfang

- Analyzer-Override um eine explizite Funktionsgroesse beziehungsweise Endadresse erweitern
- Analyzer-ABI und Descriptorversion erhoehen
- Funktionsgrenzen exakt und nicht als blossen Seed anwenden
- Jump Tables und Callbacktabellen aus dem externen Descriptor an Analyse und Codegen binden
- Konflikte mit automatisch bewiesenen Grenzen sichtbar melden
- Hookadressen als architektonische Grenzen erhalten
- keine Hinweise als Vollstaendigkeitsbeweis umdeuten

### Akzeptanz

- ein extern definiertes Funktionsintervall wird exakt emittiert
- keine Nachbarbytes werden still als Teil der Funktion aufgenommen
- bekannte Jump-/Callbacktabellen erzeugen native Kandidaten mit Live-Target-Guard
- falsche Discidentitaet deaktiviert alle Titelhinweise

---

## [ ] KR-4956 - Static-AOT-Dispatchflucht inventarisieren und schliessen

Prioritaet: P0

Abhaengigkeiten: KR-4951

### Umfang

- produktnahe POD-Zaehler fuer jede Rueckkehr zum Zentraldispatcher
- mindestens folgende Ursachen trennen:
  - Callziel nicht bekannt
  - Ziel nicht native-entry-safe
  - Timingklasse nicht deferrable
  - Schedulerereignis faellig
  - Interrupt akzeptierbar
  - MMIO-/Architekturgrenze
  - Hookgrenze
  - Varianten-/Generationguard
  - Call-Depth-Limit
  - Partitions-/Symbolgrenze
- keine Strings, Maps oder Heapallokationen im Hotpath
- terminale Top-Ursachen und Top-Callsites ausgeben
- 600-Millionen-Zyklen-Produktlauf ausfuehren

### Akzeptanz

- die 52.329.316 Zentraldispatches sind vollstaendig nach Ursachen klassifiziert
- der dominante Rueckfallgrund ist konkret benannt
- die Zaehler veraendern Produktdurchsatz und Fastpaths nicht messbar

---

## [ ] KR-4957 - Direkte native Calls ueber sichere Timinggrenzen

Prioritaet: P0

Abhaengigkeiten: KR-4956

### Problem

`can_chain_executable_block()` koppelt direkte Callfaehigkeit an `PureCpu`/`LinearRamOnly`. Eine Funktion mit spaeterem MMIO wird dadurch komplett zentral dispatcht, obwohl ein Safepoint vor dem MMIO genuegt.

### Umfang

- Vertraege trennen:
  - `NativeEntrySafe`
  - `DirectCallEligible`
  - `CompletionDeferrable`
  - `RequiresSafepointBeforeEntry`
- bekannte direkte Calls partitionsuebergreifend nativ ausfuehren
- pending Gastzyklen bei Bedarf vor dem Callee oder seiner ersten Architekturgrenze committen
- direkte Rueckkehr zur nativen Continuation
- Call-Depth-Guard nur als Stackschutz, nicht als regulaerer Dispatchpfad
- Funktionsoverrides und Mid-Hooks weiterhin ueber die externe Hookgrenze leiten

### Akzeptanz

- Funktionen mit internem MMIO duerfen direkt nativ aufgerufen werden
- Scheduler- und Interruptreihenfolge bleibt korrekt
- Zentraldispatches sinken im selben Produktlauf deutlich
- kein Missing-AOT oder stiller Hook-Bypass entsteht

---

## [ ] KR-4958 - IR-basierte Registerlokalisierung und RAM-Regionen

Prioritaet: P1

Abhaengigkeiten: KR-4957

### Umfang

- post-hoc-Textsuche und `replace_all_text()` aus der Registerlokalisierung entfernen
- echte IR-Use/Def- und Livenessinformationen verwenden
- GPR, T, PR, GBR, MACH/MACL und FPUL schrittweise lokalisieren
- Spillgrenzen fuer MMIO, Exceptions, Interrupts, Hooks, SR-/Bankwechsel und dynamischen Dispatch
- bewiesene P1-/P2-Haupt-RAM-Regionen mit einem gemeinsamen Guard direkt adressieren
- Codeinvalidierung und Writeobserver als gebuendelte Wirkung erhalten

### Akzeptanz

- keine semantische C++-Textumschreibung fuer Register mehr
- lokalisierte Funktionen duerfen normale Speicherzugriffe und direkte Calls enthalten
- Produktdurchsatz steigt ohne geaenderte Gastzyklen oder sichtbaren Bootrueckschritt

---

## [ ] KR-4959 - Ereignisgetriebene Scheduler-/IRQ-Safepoints

Prioritaet: P1

Abhaengigkeiten: KR-4956

### Umfang

- Interrupt-Epoch, hoechstes Pending-Level und Pending-Maske als billigen Guard verwenden
- vollen Routerwalk nur bei Geraeteaenderung, Acknowledge, IMASK-/BL-Aenderung oder Annahme
- Schedulercommit ueber native Regionen sammeln
- Safepoints nur vor faelligen Ereignissen, MMIO, Exceptions, Hooks, SR-/MMU-Aenderungen oder Cycle-Quantum
- Host-Lifecycle nicht pro Zentraldispatch ueber die Wall-Clock pollen
- Replay ohne aktives Log sofort und ohne Samplingarbeit verlassen

### Akzeptanz

- IRQ-/Schedulerarbeit pro Gastzyklus sinkt messbar
- keine Interrupt- oder DMA-Completion wird verschoben oder verloren
- Produktpfad und billige Diagnose verwenden dieselbe Ausfuehrungsarchitektur

---

## [ ] KR-4960 - 200-MHz-Produkt-Hotpath

Prioritaet: P0

Abhaengigkeiten: KR-4957, KR-4958, KR-4959

### Umfang

- Static-AOT-Tier ohne Materializerarbeit im Normalfall
- bereits validierten Ausfuehrungsdeskriptor direkt verwenden
- Block-/Fastpathdeskriptor ohne zweiten Lookup oder linearen Scan weiterreichen
- Function-AOT-Regionen statt Owner-Wrapper/PC-Switch so weit wie sicher moeglich ausfuehren
- keine Erfolgsstrings oder detaillierten RuntimeOnly-Sitemaps im Hotpath
- Host-Samplingprofil mit Gastfunktionssymbolen bereitstellen
- MSVC und clang-cl mit demselben Produktport vergleichen

### Akzeptanz

- mindestens 200 MHz im normalen ungedrosselten Produktlauf
- Zielreserve mindestens 250 MHz unpaced
- 600.000.000 Gastzyklen werden ohne Diagnosepfadwechsel erreicht
- der Pacer begrenzt erst oberhalb des korrekten 200-MHz-Gastvertrags

---

## [ ] KR-4961 - Externes SonicAdventureRecomp-Bring-up-Projekt

Prioritaet: P0

Abhaengigkeiten: KR-4953, KR-4954, KR-4955

### Umfang

Diese Aufgabe liefert ein eigenstaendiges externes Spielprojekt, nicht Sonic-Code im Katana-Kern.

- `SonicAdventureRecomp`-Projekt aus dem Scaffold erzeugen
- generierten SA-Code lokal und gitignored halten
- PAL-Discidentitaet, Funktionsgrenzen, Tabellen, Symbole und Hooks im Spielprojekt halten
- lokale Entwicklerinstallation ausserhalb des Repositorys
- spaeteren Nutzerinstaller auf denselben `OriginalDiscValidator`-/Contentvertrag stuetzen
- DirectBoot als Bring-up-Standard, NativeDiscBoot als Referenzpfad
- private Adressen und Hashes nur im privaten beziehungsweise dafuer vorgesehenen Spielprojekt halten

### Akzeptanz

- Katana-Kern und Runtime enthalten keine Sonic-Adressen
- SA-Projekt baut gegen installierte Runtime und verwendet Recompiler als Tool
- bestehende Originaldaten werden lokal installiert und nicht erneut ins Repo kopiert
- echter SA-Produktlauf startet aus dem Spielprojekt

---

## [ ] KR-4962 - NativeDiscBoot-/DirectBoot-Paritaet am Game-Entry

Prioritaet: P0

Abhaengigkeiten: KR-4953, KR-4961

### Umfang

- NativeDiscBoot bis unmittelbar vor die erste BootExecutable-Instruktion ausfuehren
- DirectBoot mit dem erfassten Game-Entry-Handoff starten
- CPU-, RAM-, Geraete-, Scheduler- und Interruptdigests am selben Punkt vergleichen
- erste Abweichung typisieren und nur deren Ursache korrigieren
- Sega-Screen nicht als DirectBoot-Erwartung verwenden

### Akzeptanz

- beide Pfade besitzen am Game-Entry denselben normativen Zustand
- Unterschiede sind ausschliesslich explizit erlaubte Host-/Diagnosefelder
- DirectBoot erreicht danach denselben Gamecode-Checkpoint wie NativeDiscBoot

---

## [ ] KR-4963 - Inkrementeller Runtime-/Spielbuild und Compiler-A/B

Prioritaet: P1

Abhaengigkeiten: KR-4951

### Umfang

- Laufzeiten fuer Analyse, IR, Emission, Runtimecompile, AOT-Compile und Link getrennt messen
- Runtime-only-Aenderung nur Runtime rebuilden und Spiel relinken
- Hook-only-Aenderung nur Spielprojekt rebuilden
- `aot_runtime_abi.hpp` auf schmale POD-/Intrinsic-Abhaengigkeiten reduzieren
- stabile AOT-TUs und PCH beibehalten
- Bring-up mit inkrementellem Link und ohne LTCG
- Gatebuild separat voll optimieren
- realen MSVC-/clang-cl-Vergleich desselben Ports ausfuehren

### Akzeptanz

- warmer Runtimefix plus Relink unter 30 Sekunden als Ziel
- Hook-only-Warmbuild unter 15 Sekunden als Ziel
- geaenderte AOT-Partition unter 90 Sekunden als Ziel
- unveraenderte Partitionen werden nicht re-emittiert oder neu kompiliert
- keine synthetische Benchmark ersetzt die reale Produktbuildzeit

---

## [ ] KR-4964 - v0.49 Produktabnahme bis sichtbarem Spielbild

Prioritaet: Gate

Abhaengigkeiten: KR-4960, KR-4961, KR-4962, KR-4963

### Umfang

- frischen externen SA-Produktport bauen
- vorhandene lokale Originaldiscinstallation identitaetspruefen
- DirectBoot bis mindestens zum ersten sichtbaren Spiel-/TA-Frame ausfuehren
- NativeDiscBoot als Paritaetsreferenz bis zum Game-Entry ausfuehren
- mindestens 200 MHz und die Buildziele dokumentieren
- keine Vollsuite als Ersatz fuer das Produkt ausgeben

### Akzeptanz

- `BootExecutableEntry`, `GameCodeProgressed` und `FirstVisibleGameFrame` erreicht
- kein Interpreter, JIT, Emulationsfallback oder Sonic-Sonderfall im Katana-Kern
- Produkt laeuft mindestens mit 200 MHz
- Originaldaten bleiben lokal und ausserhalb des Repositorys
- verbleibender naechster Produktblocker ist konkret benannt
