# KatanaRecomp Roadmap

Status: Pre-Alpha

Aktuelle Phase: `v0.49.0` - statischer Dreamcast-Recompiler, externe Spielprojekte und produktiver Sonic-Adventure-Bring-up

Erster oeffentlicher Release: `v0.50.0` Alpha

## Produktziel

KatanaRecomp ist ein statischer SH-4-Recompiler mit einer getrennt installierbaren KatanaRuntime. Ein konkretes Spiel wird in einem eigenen, hashgebundenen Recomp-Projekt gebaut.

```text
KatanaRecomp
  -> analysiert SH-4
  -> erzeugt natives C++

KatanaRuntime
  -> stellt die gemeinsamen Dreamcast-Plattformgrenzen bereit

SonicAdventureRecomp
  -> bindet generierten SA-Code, lokale Originaldaten, DirectBoot, Hooks und Patches
  -> erzeugt die startbare Produkt-EXE
```

KatanaRecomp und KatanaRuntime bleiben im selben Repository, sind aber getrennte Build- und Installationsprodukte. Titeladressen, Titelhooks, private Symbole und Installationsprofile gehoeren in das jeweilige externe Spielprojekt.

## Unverhandelbare Grenzen

- kein allgemeiner SH-4-Interpreter im normalen Produktport
- kein JIT
- kein Emulationsfallback
- keine stillen No-op-Stubs oder erfundenen Hardwareerfolge
- keine Sonic-spezifischen Adressen im generischen Katana-Kern
- keine Retail-, BIOS- oder Assetdaten im Repository oder verteilbaren Paket
- Flycast und XenonRecomp sind Referenzen, keine Codequellen
- das echte erzeugte Produkt ist die Bring-up-Abnahme
- keine neuen breiten Testmatrizen waehrend des Spiel-Bring-ups

## Verifizierter Ausgangsstand

Quellstand der Review:

```text
aa1cd51655fdefec5a9891152487f902f91046c6
Document v0.49 product measurements
```

Der Umbau hat echte Architekturgrundlagen geliefert:

- installierbares `KatanaRecomp::runtime_core`
- schmale AOT-ABI und Precompiled Header fuer generierte TUs
- getrennte Static-/Dynamic-AOT-Tiers
- validierte Ausfuehrungsdeskriptoren ohne normales zweites Tabellenlookup
- P1-/P2-Inline-Cache
- native Blocklabels und erste direkte native Calls
- konservative Registerlokalisierung und Haupt-RAM-Fastpaths
- executable-first Artefakt aus der eigenen `.gdi`
- allgemeine externe `GameProjectDefinition`
- Bring-up-/Gate-Buildprofile sowie Partition- und Metadatencache

Die aktuelle Produktevidenz ist trotzdem nicht ausreichend:

```text
MSVC:     600.000.000 Gastzyklen in 14,8563 s = 40,3869 MHz
clang-cl: 600.000.000 Gastzyklen in 14,1289 s = 42,4662 MHz
zentrale Dispatches: 52.329.316
sichtbarer Meilenstein: keiner
```

Die alte NativeDiscBoot-Linie erreichte das Sega-Bild. Der neue DirectBoot-Pfad soll dieses Bild nicht anzeigen, weil das Sega-Logo aus IP.BIN stammt. Der fehlende Sega-Screen ist daher allein kein DirectBoot-Fehler. Ein DirectBoot-Produkt muss stattdessen ab `BootExecutable` beziehungsweise `GameEntry` bewertet werden.

## Reviewbefunde

### 1. DirectBoot verwendet den falschen Handoff-Begriff

Der aktuelle DirectBoot startet die Haupt-Executable bei `0x8C010000`, wendet aber einen fest codierten `DreamcastPostBiosCpuState` an. IP.BIN laeuft zwischen BIOS und Haupt-Executable, richtet Stack, VBR, Cache und Hardware ein und kann RAM- sowie Geraetezustand veraendern. Ein Post-BIOS-Zustand ist daher kein bewiesener Post-IP.BIN-/Game-Entry-Zustand.

Der aktuelle Vertrag bindet im Wesentlichen CPU-Spezialregister. Er bindet nicht vollstaendig:

- alle GPR-/Bank-/FPU-Zustaende am Game-Entry
- niedrige RAM-Seiten und weitere von IP.BIN veraenderte RAM-Bereiche
- PVR-, GD-ROM-/G1-, DMAC-, AICA-, Maple- und Systembuszustand
- Scheduler- und pending-Interruptzustand
- den exakten PR-/Returnvertrag des Transfers von IP.BIN zur Haupt-Executable

DirectBoot braucht deshalb einen eigenen `GameEntryHandoff`-Vertrag. Der vorhandene Post-BIOS-Vertrag bleibt fuer NativeDiscBoot erhalten.

Umgesetzter Zwischenstand: `GameEntryHandoff` Schema 2 beschreibt und bindet
CPU, RAM-Operationen, Geraete und Scheduler getrennt. Ein privater
titelgebundener Artefaktprovider ist in die Spielprojektschnittstelle
integriert. Der derzeitige reale Capture-/Apply-Pfad ist jedoch absichtlich
nur `CpuMemoryDiagnostic`; Geraete und Scheduler bleiben ausstehend und das
Produktgate verbietet diesen Diagnosepfad. Der Befund ist daher noch nicht
geschlossen und ein erfolgreicher DirectBoot wird nicht behauptet.

### 2. DirectBoot und NativeDiscBoot werden mit falschen visuellen Erwartungen verglichen

`NativeDiscBoot` fuehrt IP.BIN aus und kann das Sega-Lizenzbild erzeugen.

`DirectBootExecutable` ueberspringt IP.BIN und darf deshalb keinen Sega-Screen als Pflichtmeilenstein besitzen. Seine Pflichtmeilensteine sind:

```text
BootExecutableEntry
GameCodeProgressed
FirstGameFramebufferWrite oder FirstTaFrame
FirstVisibleGameFrame
TitleScreen
```

### 3. Function-Level-AOT faellt im Produkt zu haeufig in den Zentraldispatch zurueck

52.329.316 zentrale Dispatches fuer 600.000.000 Gastzyklen bedeuten nur etwa 11,5 Gastzyklen pro Zentraldispatch. Das ist mit einem Xenon-artigen Function-AOT-Hotpath unvereinbar.

Der aktuelle native Call-/Labelpfad ist an `can_chain_executable_block()` gekoppelt. Dieser akzeptiert nur `PureCpu` und `LinearRamOnly`, prueft Scheduler-, Interrupt-, Mapping-, Varianten- und Codegenerationen und lehnt viele normale Funktionen mit Speicher- oder Geraetegrenzen komplett ab. Dadurch werden Calls und Blockuebergaenge, die innerhalb einer nativen Funktion mit einem gezielten Safepoint korrekt weiterlaufen koennten, erneut zentral dispatcht.

Chaining, direkte native Callfaehigkeit und aufgeschobener Schedulercommit muessen getrennte Vertraege werden:

- `NativeEntrySafe`
- `DirectCallEligible`
- `CompletionDeferrable`
- `RequiresSafepointBeforeEntry`

Eine Funktion mit MMIO darf einen direkten nativen Call erhalten, wenn vor dem MMIO ein Safepoint emittiert wird. Sie muss nicht allein wegen eines spaeteren MMIO-Zugriffs komplett ueber den Zentraldispatcher aufgerufen werden.

### 4. Die Spielprojekt-API ist noch kein benutzbares externes Produkt

`GameProjectDefinition` modelliert Funktionsgrenzen, Tabellen, Hooks, Symbole, Identitaeten und eine Bootkonfiguration. Es fehlen aber:

- ein serialisierbarer, versionsgebundener Descriptor
- CLI-Laden und Validierung dieses Descriptors
- ein generiertes externes Projekt-Scaffold
- symbolische Hookbindung fuer ein separates Spielrepository
- ein lokaler Entwicklerinstaller, der exakt denselben Contentvertrag wie der spaetere Nutzerinstaller verwendet

Ausserdem wird `GameProjectFunctionBoundary::size` aktuell nicht als echte Analyzer-Funktionsgrenze weitergereicht. Der Analyzer-Override kennt nur eine Startadresse. Damit ist ein zentraler Xenon-artiger Titelhinweis derzeit nur teilweise wirksam.

Der Game-Entry-Handoff-Provider und seine deklarative Bindung sind inzwischen
als C++-Vertrag vorhanden. Noch offen sind der serialisierte
Spielprojektdescriptor, das Scaffold und der vollstaendige Plattform-Apply.

### 5. Registerlokalisierung ist zu konservativ und technisch fragil

Die aktuelle Lokalisierung gilt nur fuer reine Leaf-Funktionen ohne Speicher, FPU, Spezialregister oder Calls. Sie bestimmt Registerverwendung durch Textsuche im bereits erzeugten C++ und ersetzt `cpu.r[n]` anschliessend textuell.

Das muss durch IR-/Liveness-basierte Registerlokalisierung ersetzt werden. Post-hoc-Stringersetzung ist kein stabiler Codegenvertrag und darf nicht auf komplexere Funktionen ausgeweitet werden.

### 6. Die Runtimegrenzen sind getrennt, aber der AOT-Includevertrag bleibt breit

`aot_runtime_abi.hpp` inkludiert weiterhin mehrere breite Runtimeheader. Das vergroessert die Rebuildflaeche generierter TUs. Der Header muss auf echte ABI-PODs und schmale Intrinsics reduziert werden.

### 7. Der Build ist warm brauchbar, kalt noch zu teuer

Der warme Export liegt bei rund 2,3 Sekunden. Der erste vollstaendige Export/Build bleibt mit rund 199 Sekunden teuer. Der naechste Buildfokus ist deshalb nicht ein weiterer Exportcache, sondern:

- Runtime-only-Aenderung ohne AOT-Reemission
- Hook-only-Aenderung nur im Spielprojekt
- mehr parallele, stabile AOT-TUs ohne uebermaessige Headerkosten
- realer MSVC-/clang-cl-Produktvergleich
- phasengetrennte Messung von Analyse, Emission, Compile und Link

## Verbindlicher v0.49-Kritischer Pfad

```text
KR-4951 Produktgate nach Gastzyklen und getrennte visuelle Meilensteine
  |
  +--> KR-4952 Post-IP.BIN-Spielhandoff fuer DirectBootExecutable
  |      -> KR-4953 Privates Game-Entry-Handoff-Artefakt aus Original-GDI
  |      -> KR-4954 Deklaratives externes Spielprojekt und CLI-Scaffold
  |      -> KR-4955 Explizite Funktionsgrenzen und Tabellenhinweise End-to-End
  |      -> KR-4961 Externes SonicAdventureRecomp-Bring-up-Projekt
  |      -> KR-4962 NativeDiscBoot-/DirectBoot-Paritaet am Game-Entry
  |
  +--> KR-4956 Static-AOT-Dispatchflucht inventarisieren und schliessen
         -> KR-4957 Direkte native Calls ueber sichere Timinggrenzen
         -> KR-4958 IR-basierte Registerlokalisierung und RAM-Regionen
         -> KR-4959 Ereignisgetriebene Scheduler-/IRQ-Safepoints
         -> KR-4960 200-MHz-Produkt-Hotpath

KR-4963 Inkrementeller Runtime-/Spielbuild und Compiler-A/B laeuft parallel.

KR-4960 + KR-4961 + KR-4962 + KR-4963
  -> KR-4964 v0.49 Produktabnahme bis sichtbarem Spielbild
```

## Produktmeilensteine

### B0 - Game-Entry korrekt

- Haupt-Executable aus der eigenen GDI identifiziert und hashgebunden
- exakter Post-IP.BIN-Handoff angewendet
- erster native Block bei `0x8C010000` erreicht
- kein Interpreter/JIT/Fallback

### B1 - Gamecode macht Fortschritt

- fester Gastzyklus-Checkpoint erreicht
- kein Missing-AOT oder stiller Geraetefehler
- DirectBoot und NativeDiscBoot stimmen ab dem Game-Entry in den normativen Zustandsdigests ueberein

### B2 - Sichtbares Spielbild

- Direct-FB- oder TA-Ausgabe stammt aus der Haupt-Executable
- aktiver Scanout liest den erzeugten Bereich
- Host praesentiert den Frame

### B3 - Echtzeit

- mindestens 200 MHz effektive Gastgeschwindigkeit im normalen Produktpfad
- Zielreserve mindestens 250 MHz unpaced
- Diagnose-off und billige Produktdiagnose verwenden denselben schnellen Ausfuehrungspfad

### B4 - Titelbild und Eingabe

- Titelbild oder erster interaktiver Spielscreen
- Controller im real gestarteten Spiel nachgewiesen
- mehrminuetiger stabiler Lauf

## Arbeitsregeln

- Eine Implementierungsrunde endet mit einem echten Produktlauf.
- Bootkorrektheit wird bei festem Gastzyklusziel gemessen, nicht bei fixer Hostzeit.
- NativeDiscBoot und DirectBoot werden ab demselben Game-Entry verglichen.
- Keine neue breite Testsuite. Ein kleiner Regressionstest ist nur fuer einen bereits beobachteten Produktfehler erlaubt.
- Keine weiteren Controller-, GUI-, Paketierungs- oder Komfortarbeiten vor B2.
- Kein Hardwareausbau auf Verdacht. Der naechste Produktendpunkt entscheidet.
- Roadmap- und Taskstatus werden erst nach dem Produktlauf aktualisiert.
- Alte, durch einen bestaetigten Nachfolgeexport ersetzte Portordner werden
  gezielt geloescht; aktuelle DirectBoot-/NativeDisc-Referenzen,
  Boot-Executable-Artefakte und Nutzerdaten bleiben erhalten.

## Nicht auf dem aktuellen P0-Pfad

- vollstaendige Dreamcast-Kompatibilitaet fuer weitere Titel
- umfassendes Replay jeder Gastinstruktion
- vollstaendige PVR-/AICA-Featureabdeckung ohne SA-Produktbefund
- GUI-Politur
- oeffentliche Releasepaketierung
- neue Konformitaetsmatrizen

## v0.49 Definition of Done

`v0.49.0` ist erst abgeschlossen, wenn:

- KatanaRecomp, KatanaRuntime und ein externes Spielprojekt sauber getrennt gebaut werden koennen
- die Haupt-Executable aus der Original-GDI lokal installiert und direkt gestartet werden kann
- DirectBoot einen bewiesenen Post-IP.BIN-/Game-Entry-Handoff nutzt
- NativeDiscBoot und DirectBoot ab dem Game-Entry denselben normativen Zustand liefern
- der normale Produktport mindestens 200 MHz erreicht
- ein sichtbarer, von der Haupt-Executable erzeugter Spiel-/TA-Frame erreicht wird
- keine Sonic-Adressen, Retailbytes oder Titelhooks im generischen Katana-Kern liegen
- keine neue umfangreiche Testsuite den Produktnachweis ersetzt
