# KatanaRecomp: Repository-weite Core-, Performance- und Build-Review

**Prüfstand:** `main` auf Commit `9e8257b5fbac8003fae445477bb7a40af67ca34b`
**Reviewdatum:** 17. Juli 2026
**Prioritäten:** Gastgenauigkeit, bestehende Funktionserhaltung, Laufzeit, Analyse- und Buildgeschwindigkeit

## 1. Umfang und Aussagekraft

Geprüft wurden die produktentscheidenden Pfade des Repositorys:

- SH-4-Decoder und Instruktionsmetadaten
- rekursive Analyse, lokale und interprozedurale Wertanalyse, Jump Tables
- Katana-IR, Verifier, Optimierer und C++-Backend
- CPU-Zustand, Exceptions, Interrupts, Scheduler, Timer und DMA
- Speicherbus, Store Queues, Cache-/MMU-Grenzen und Codeinvalidierung
- GDI, ISO9660 und GD-ROM
- PVR, AICA, Maple und Hostruntime
- Portexport, Codegencache, CMake, Presets und Testarchitektur
- aktuelle Roadmap- und Taskabhängigkeiten

Die Review erfolgte statisch gegen den exakten GitHub-Stand. Die verfügbare
Ausführungsumgebung konnte das Repository nicht als lokalen Arbeitsbaum bauen,
ausführen oder benchmarken. Deshalb wird hier nicht behauptet, ein Test sei
tatsächlich fehlgeschlagen. Die bestätigten Befunde ergeben sich direkt aus
inkonsistenten Produktpfaden, Tests oder Verträgen. Jede Reparatur muss mit
frischen Debug- und optimierten Builds, Differentialtests und der vollständigen
bestehenden Regression bestätigt werden.

## 2. Gesamturteil

Das Projektziel ist sinnvoll umgesetzt:

- Decoder, Analyse, IR, Backend und Runtime sind getrennt.
- Titeladressen und Sonic-spezifische Runtimepatches werden vermieden.
- unsichere Kontrollflussziele werden sichtbar klassifiziert.
- proprietäre Daten bleiben außerhalb des Repositorys.
- der Port besitzt einen eigenständigen Runtimepfad.
- unbekannte Fälle sollen sichtbar statt still erfolgreich sein.

Die Architektur muss nicht neu gebaut werden. Mehrere vorhandene Kernverträge
sind aber noch nicht belastbar genug für weiteren Retail-Bring-up. Die
wichtigsten Probleme betreffen CPU-Kontrollzustand, Store Queues,
Exception-/Interruptfortsetzung, Codeinvalidierung, Analysesoundness,
Blockregistry, Gasttiming und die optimierte Buildmatrix.

**Empfehlung:** `KR-4715` blockieren, zuerst ein Core-Korrektheitsgate und danach
ein Performance-/Buildgate durchführen. Alle bestehenden korrekten Funktionen
und Tests bleiben erhalten. Falsche historische Erwartungen werden nicht
gelöscht, sondern durch unabhängige Spezifikationsregressionen ersetzt.

---

# 3. Bestätigte P0-Befunde

## KR-R01: Registerbankwahl ignoriert `SR.MD`

**Dateien**

- `src/runtime/runtime.cpp`
- `tests/runtime/runtime_tests.cpp`

`CpuState::write_sr` tauscht R0 bis R7, sobald sich `SR.RB` ändert. Die effektiv
sichtbare SH-4-Registerbank hängt jedoch von privilegiertem Modus und RB
gemeinsam ab. Der bestehende Test schreibt `RB=1` bei nicht gesetztem `MD` und
erwartet ausdrücklich einen Bankwechsel. Der Test schützt damit die falsche
Semantik.

**Risiko**

- falsche R0-R7-Werte in User Mode
- fehlerhafte Exception- und Interruptregister
- falsche RTE-Wiederherstellung
- schwer diagnostizierbare BIOS-/HLE-Fehler

**Reparatur**

- effektive Bank als `MD && RB`
- Banktausch nur bei Änderung dieser effektiven Auswahl
- vollständige User-/Privileged- und RB0-/RB1-Matrix
- unabhängige SH-4-Vektoren statt Implementierung als Testorakel

---

## KR-R02: Store Queue wird über Adressbit 25 statt Bit 5 gewählt

**Dateien**

- `src/runtime/store_queue.cpp`
- `src/runtime/dreamcast_boot.cpp`
- `tests/runtime/store_queue_tests.cpp`

`queue_index` verwendet `(address >> 25) & 1`. Das P4-Lesefenster wiederholt
dieselbe Auswahl. Die Tests verwenden weit auseinanderliegende Bereiche wie
`0xE0000000` und `0xE2000000`, wodurch die falsche Implementierung als Erfolg
gilt.

**Risiko**

- SQ0 und SQ1 enthalten falsche Bytes
- `PREF` überträgt die falsche Queue
- TA-Pakete und RAM-Transfers werden verfälscht
- Codeinvalidierung kann auf falschen Transferdaten beruhen

**Reparatur**

- SQ-Auswahl überall über Adressbit 5
- Testadressen `0xE0000000` und `0xE0000020`
- QACR, Zielbildung, Queuegrenzen und Zugriffbreiten separat testen
- bisherigen Bit-25-Fall als explizite Bugregression erhalten

---

## KR-R03: Call-Delay-Slot aktualisiert PR in falscher Reihenfolge

**Datei**

- `src/codegen/cpp_emitter.cpp`

Bei direkten und indirekten Calls wird zunächst der Delay Slot emittiert und
danach `cpu.pr = PC + 4` geschrieben. Damit überschreibt der Call einen
PR-Schreibzugriff des Delay Slots. Das widerspricht dem verzögerten
SH-4-Kontrollzustand.

**Risiko**

- falsche Rücksprungadresse
- Fehler bei `STS PR` oder `LDS ...,PR` im Delay Slot
- inkonsistente Ergebnisse zwischen Analyse und generiertem Code

**Reparatur**

- verzögerten PR-Wert explizit modellieren
- BSR, BSRF und JSR mit PR-lesenden und PR-schreibenden Delay Slots
- Referenz-, IR- und C++-Pfad differenziell vergleichen

---

## KR-R04: `RTE` beendet den generierten Gastlauf

**Dateien**

- `src/codegen/cpp_emitter.cpp`
- `src/codegen/port_export.cpp`

Der Emitter restauriert SR und SPC, führt den Delay Slot aus und kehrt aus der
Blockfunktion zurück. Der Portadapter klassifiziert `ReturnFromException` als
normales `Return`; die Dispatchkette beendet daraufhin den gesamten Lauf statt
bei SPC weiterzumachen.

**Risiko**

- jeder behandelte Interrupt beendet die Runtime
- BIOS-/Systemhandler können nicht zurückkehren
- Timer-, DMA- und Gerätepfade bleiben synthetisch grün, aber produktiv
  unbrauchbar

**Reparatur**

- eigener Blockendtyp `ExceptionReturn`
- dynamisches Folgeziel ist das restaurierte `cpu.pc`
- Delay Slot läuft mit restauriertem SR
- RTE darf den Hostlauf nicht als Funktionsreturn beenden

---

## KR-R05: `SLEEP` fällt als normaler Block weiter

**Dateien**

- `src/codegen/cpp_emitter.cpp`
- `src/codegen/port_export.cpp`

Der Emitter setzt `cpu.sleeping = true`, die Blockklassifikation kennt `Sleep`
aber nicht und verwendet `Fallthrough`. Der nächste Block kann deshalb direkt
weiterlaufen.

**Reparatur**

- eigener Blockendtyp `Sleep`
- Scheduler wartet bis zu einem akzeptierten Interrupt oder Budgetende
- Wakeup nur über den normalen Interruptpfad
- keine Host-Wall-Clock als Gastwahrheit

---

## KR-R06: Akzeptierte Interrupts werden als fataler Gastfehler behandelt

**Dateien**

- `src/runtime/exception.cpp`
- `src/runtime/platform_interrupt.cpp`
- `src/codegen/port_export.cpp`

`enter_exception` setzt auch für Interrupts `trap_pending`. Der generierte
Blockwrapper priorisiert dieses Flag als Exception, und `dispatch_chain` bricht
mit `guest-exception-before-checkpoint` ab. Der Interruptvektor wird nicht als
normaler Gastcode fortgesetzt.

**Risiko**

- TMU-, DMA-, PVR-, GD-ROM- und AICA-Interrupts beenden den Lauf
- Scheduler und Geräte können Ereignisse erzeugen, die der Gast nicht korrekt
  behandeln kann
- RTE kann diesen Pfad ohnehin noch nicht fortsetzen

**Reparatur**

- Gast-Exceptionseintritt von fatalem Hostabbruch trennen
- zu Vektorblöcken dispatchen
- Handler, Maskierung, Verschachtelung und RTE gemeinsam testen

---

## KR-R07: Generierte Gaststores umgehen Codeinvalidierung

**Dateien**

- `src/codegen/cpp_emitter.cpp`
- `src/codegen/port_export.cpp`
- `src/runtime/code_invalidation.cpp`

Generierte CPU- und FPU-Stores schreiben direkt über `cpu.memory.write_*`.
Der `ExecutableCodeTracker` wird nur in einzelnen PlatformServices-, DMA-,
Store-Queue- oder Copy-Pfaden aufgerufen. Normale rekompilierte Gaststores
besitzen keinen zentralen Write-Observer.

**Risiko**

- selbstmodifizierender Code verwendet stale Hostblöcke
- RAM-Overlays und geladene Module können alten Code ausführen
- physische Aliase und Inline-Caches bleiben gültig
- dynamischer Code ist vor dem Alpha-Bring-up nicht sicher

**Reparatur**

- einheitlicher Gastschreibvertrag für CPU, FPU, DMA, SQ, Copy und Fallback
- Bytevergleich vor Generationserhöhung
- Alias-, Link- und Inline-Cache-Invalidierung im selben Vorgang
- stale Block darf über keinen Lookupweg erreichbar bleiben

---

## KR-R08: Runtime-Blocktabelle besitzt instabile Zeiger und schlechte Komplexität

**Dateien**

- `include/katana/runtime/block_table.hpp`
- `src/runtime/block_table.cpp`

Die Tabelle speichert Blöcke in einem `std::vector`, gibt rohe Zeiger auf
Elemente zurück, scannt vor jedem Insert alle Blöcke und sortiert nach jedem
Insert den gesamten Vektor. `lookup`, `lookup_physical` und Aliasermittlung sind
linear. `push_back`, `sort` und `erase` können ausgegebene Zeiger ungültig
machen.

**Risiko**

- Dangling Pointer bei dynamischer Registrierung oder Invalidierung
- quadratischer Aufbau bei großen Programmen
- linearer indirekter Dispatch
- schlechte Skalierung auf zehntausende Blöcke

**Reparatur**

- stabile Block-IDs oder Handles
- immutable statische Bulk-Tabelle
- getrennte dynamische Registry
- O(1)- oder O(log N)-Indizes nach virtueller, physischer und Variantadresse
- 100.000-Block-Stress- und Mutationsregression

---

## KR-R09: Unbekannte Caller werden in Kandidateneingaben übersprungen

**Datei**

- `src/analysis/function_value_analysis.cpp`

`merge_candidate_input` ignoriert einen Callerzustand, wenn dessen Wert
unbekannt oder leer ist. Ein anderer bekannter Caller kann dadurch eine
scheinbar vollständige endliche Guardmenge erzeugen, obwohl mindestens ein
Pfad unbekannt bleibt.

**Risiko**

- unterapproximierte Zielmengen
- fehlende Funktionen im generierten Port
- optimistisches `unresolved == 0`
- Runtime-Miss trotz angeblich vollständiger Analyse

**Reparatur**

- Vollständigkeit als eigenes Lattice-Merkmal
- unbekannter Caller taintet die Zusammenführung
- Guardmenge nur bei vollständigen Caller-Kontexten
- negative Mehrcaller-Regressionen

---

## KR-R10: Kontextabhängige Auflösungen werden nur nach Adresse dedupliziert

**Datei**

- `src/analysis/function_value_analysis.cpp`

Nach der Funktionsauswertung werden Zielauflösungen mit `std::unique` nur über
`instruction_address` dedupliziert. Unterschiedliche Zielmengen,
Guardzustände oder Provenienzen verschiedener Kontexte werden nicht
konservativ vereinigt. Eine Variante bleibt übrig.

**Reparatur**

- Zielmengen pro Site vereinigen
- Vollständigkeit und Guardstatus konservativ mergen
- Provenienz aller Kontexte erhalten
- deterministische Reihenfolge ohne Informationsverlust

---

## KR-R11: Interne Exceptionursache und gastseitiger Eventcode können abweichen

**Datei**

- `src/runtime/exception.cpp`

Für nicht ausgerichtete Zugriffe wird ein AddressError-Cause gewählt. Andere
Speicherfehler erhalten einen BusError-Cause, während der eingetragene
gastseitige Eventcode weiterhin aus demselben Address-Error-Paar gewählt wird.
Diagnose und Gastzustand können unterschiedliche Ursachen melden.

**Reparatur**

- tabellengetriebene Exceptionmatrix
- Cause, EXPEVT/INTEVT, Vektor, TEA und SPC gemeinsam ableiten
- Read/Write, Fetch/Data, Delay Slot und MMU-Fälle testen

---

## KR-R12: GD-ROM besitzt eine zweite, nicht angebundene Gastuhr

**Dateien**

- `src/runtime/disc.cpp`
- `src/runtime/dreamcast_boot.cpp`
- `src/codegen/port_export.cpp`

`GdRomAsyncReader` verwaltet eine eigene `current_cycle_` und schließt Requests
nur bei `advance_to` ab. Der produktive Port avanciert den allgemeinen
`EventScheduler`, nicht den GD-ROM-Reader.

**Risiko**

- Requests bleiben dauerhaft pending
- GD-ROM-Interrupts fehlen
- Discstreaming kann nach dem Boot stehenbleiben

**Reparatur**

- GD-ROM direkt auf dem gemeinsamen Scheduler planen
- keine zweite Gastuhr
- Command, Datenbereitstellung, DMA und ASIC-Interrupt in einer Ereigniskette

---

## KR-R13: Gasttiming und Laufbudgets besitzen keine gemeinsame Wahrheit

**Dateien**

- `src/runtime/dreamcast_boot.cpp`
- `src/codegen/port_export.cpp`
- Scheduler-, TMU-, RTC- und DMA-Pfade

Die Initialisierung verwendet provisorische Teiler und der generierte
Dispatcher zusätzliche feste Block- und Eventbudgets. Das konfigurierte
Gastzyklusbudget ist nicht die einzige verbindliche Laufgrenze.

**Risiko**

- falsche Geräte- und Interruptreihenfolge
- nicht vergleichbare Metriken
- Timeouts ohne stabile Gastbedeutung
- scheinbar deterministische, aber hardwareunplausible Checkpoints

**Reparatur**

- ein zentraler versionierter Gastzyklusvertrag
- Instruktionskosten und Geräteteiler als explizite Tabellen
- Scheduler, TMU, RTC, DMA, GD-ROM und PVR auf derselben Uhr
- genau eine Budget- und Metrikquelle

---

## KR-R14: Hostsmoke und Gastfortschritt sind nicht sauber getrennt

**Datei**

- `src/codegen/port_export.cpp`

Ein Gastcheckpoint gilt bereits, sobald irgendein Block ungleich dem
initialen Entry ausgeführt wurde. Danach erzeugt der Hostpfad zusätzliche
Framebuffer- und Audiomessungen. Diese Werte beweisen nicht automatisch, dass
der Gast PVR oder AICA tatsächlich angesteuert hat.

**Reparatur**

- Checkpoints an allgemeine semantische Bereiche binden
- `host_backend_smoke_frame` und `guest_pvr_frame` getrennt zählen
- `host_audio_smoke` und `guest_aica_buffer` getrennt zählen
- kein Sonic-Checkpoint aus bloßer Blocknummer oder Hostfixture

---

# 4. P1-Genauigkeitsrisiken

## 4.1 FPU ist stark von Host-FPU und libm abhängig

`ScopedHostRounding` verändert die Host-Rundung pro Operation. FSCA, FSQRT,
FIPR, FTRV und FMAC verwenden Hostfunktionen wie `std::sin`, `std::cos`,
`std::sqrt` und `std::fma`. FPSCR Cause/Flag/Enable, NaN- und Denormalregeln
sind nur teilweise modelliert.

**Maßnahmen**

- bitgenaue unabhängige FPU-Vektoren
- deterministische FSCA-Implementierung
- explizite FPSCR-Exceptionsemantik
- MSVC-/GCC-/Clang-Differentialtests
- Host-Rundungswechsel pro Block oder Softwarepfad statt pro Operation

## 4.2 MMU und generierter Speicherpfad sind nicht vereinheitlicht

Die Runtime besitzt MMU- und Guardstrukturen, generierte Loads und Stores
laufen aber direkt über `cpu.memory`. Solange MMU nicht unterstützt wird, muss
das Capability-Profil sie klar ablehnen. Bei Unterstützung darf kein zweiter
Speicherpfad existieren.

## 4.3 Flash-Zugriffsbreiten brauchen einen expliziten Vertrag

Breite Basiszugriffe können in mehrere Bytezugriffe zerfallen. Bei
commandbasiertem Flash kann das mehrere Zustandsübergänge auslösen. Legale
Breiten müssen explizit implementiert, illegale sichtbar abgelehnt werden.

## 4.4 PVR und AICA bleiben Minimalmodelle

`StartRender` meldet Renderende unmittelbar. TA-, Renderzeit- und
Gast-Audiofortschritt sind noch nicht stark genug vom Hostsmoke getrennt. Das
ist als Pre-Alpha-Modell zulässig, aber kein Spieloutputnachweis.

## 4.5 BIOS-HLE bleibt sichtbar unvollständig

Das Projekt macht fehlende Dienste erfreulich nicht zu No-ops. Die
fehlenden Dienste müssen aus realen Runtimeblockern priorisiert und anschließend
allgemein getestet werden.

---

# 5. Performancebefunde

## 5.1 Speicherbus ist ein linearer Hotpath

`Memory::resolve`, `contains` und `maps_device` scannen alle Regionen.
Dreamcast-Aliase erhöhen die Regionszahl. `notify_access` baut bei jedem
Zugriff ein Ereignis mit Regionsstring und kopiert Observer in einen temporären
Vektor.

**Maßnahmen**

- zweistufige Seiten-/Regionsdekodiertabelle
- direkte RAM-/VRAM-/AICA-Fastpaths mit Guards
- Nullkostenpfad ohne Trace oder Watchpoint
- kompakte Regions-ID statt Stringkopie

## 5.2 Breite lineare Speicherzugriffe zerfallen in virtuelle Bytezugriffe

`LinearMemoryDevice` implementiert nur Bytezugriffe direkt. 16- und
32-Bit-Zugriffe verwenden mehrere virtuelle Aufrufe und Boundschecks.

**Maßnahmen**

- endian-sichere u16/u32-Overrides
- ausgerichtete Hostloads nur unter getesteten Regeln
- Differentialtest gegen den Bytepfad

## 5.3 Codeinvalidierung scannt jeden Block pro Write

`ExecutableCodeTracker::observe_write` prüft alle Blöcke und speichert jede
Invalidierung dauerhaft.

**Maßnahmen**

- Page-to-Block-Index
- stabile Block-IDs
- begrenzter Ringbuffer plus Aggregate
- byteidentische Writes ohne Generationserhöhung

## 5.4 Diagnosehistorien wachsen unbegrenzt

Store-Queue-Transfers, Invalidierungsereignisse und andere Recorder verwenden
unbegrenzte Vektoren. Lange Sitzungen können Hostspeicher verlieren.

**Maßnahmen**

- Trace-Level
- begrenzte Ringbuffer
- Dropped-Counter und monotone Aggregate
- vollständige Rohtraces nur explizit außerhalb des Repositorys

## 5.5 DMA arbeitet zu feingranular

Ein Schedulerereignis pro Transferunit skaliert bei großen Transfers schlecht.

**Maßnahmen**

- deterministische Batches bis zum nächsten fremden Event
- Burstgrenzen und Interruptzeitpunkt erhalten
- Differentialtest gegen Einzelunit-Referenz

## 5.6 Disc-I/O öffnet Dateien pro Read

`FileDiscSource::read` öffnet bei jedem Zugriff einen neuen `ifstream`.
GDI-/ISO-Pfade allozieren häufig neue Puffer und suchen Tracks wiederholt.

**Maßnahmen**

- persistente read-only Handles oder `pread`
- Track-/LBA-Index
- Batchsektoren und Read-Ahead
- ISO-Verzeichnis- und Sektorcache
- Provenienz-Hashes nur einmal erfassen

## 5.7 Analysefixpunkte rechnen zu viel neu

Rekursive Analyse, Zielauflösung und Funktionssummaries führen wiederholte
Ganzprogrammabläufe aus. Provenienzstrings und kopierte Listen erhöhen Zeit und
Speicher.

**Maßnahmen**

- inkrementelle CFG-/SCC-Worklists
- typisierte Provenienz-IDs
- immutable Arenen und Spans
- gemeinsame Site-/Edge-/Block-/Funktionsindizes

## 5.8 Codegencache ist nicht ausreichend crash- und kollisionsfest

Der Cachekey basiert auf 64-Bit-FNV. Dateien werden direkt mit `trunc`
geschrieben, ohne atomaren Publish oder Inhaltsmanifest.

**Maßnahmen**

- SHA-256 Content Addressing
- temporäre Datei plus atomarer Rename
- Schema-, Längen- und Hashprüfung
- immutable Write-once oder Prozesslock

## 5.9 Exportierte Ports bauen den gesamten Katana-Quellbaum mit

Die generierte CMake-Datei verlangt `KATANA_RUNTIME_ROOT`, bindet den
KatanaRecomp-Quellbaum per `add_subdirectory` ein und linkt `katana_core`.

**Maßnahmen**

- minimales installierbares Runtime-SDK
- `find_package(KatanaRecomp CONFIG REQUIRED)`
- nur benötigte Runtime-/IO-Targets
- ABI- und Packageversion vor dem Build prüfen

## 5.10 Die Buildmatrix prüft nur Debug

Alle vorhandenen Presets erben `CMAKE_BUILD_TYPE=Debug`. Ein dauerhafter
RelWithDebInfo-/Releasepfad fehlt.

**Risiko**

- Optimierungs-UB wird spät entdeckt
- Performance kann nicht regressionsgeprüft werden
- Compilerunterschiede erscheinen erst am Alpha-Gate

**Maßnahmen**

- MSVC, GCC und Clang in Debug und RelWithDebInfo
- Sanitizer, statische Analyse und Release-Differential
- Core-/CLI-Presets ohne Desktop-GUI als Standard
- optimierte CI nicht bis Alpha verschieben

## 5.11 Sehr viele einzelne Testprogramme erhöhen Buildkosten

Die CMake-Datei registriert eine große Zahl kleiner Executables.

**Maßnahmen**

- Tests nach Subsystem konsolidieren
- gemeinsame Objektbibliotheken und Fixtures
- schnelle Unit-, mittlere Integrations- und schwere Gateprofile
- PCH oder Unity nur nach Messung

## 5.12 Versions- und Packagequelle ist nicht kanonisch

Das CMake-Projekt meldet weiterhin `0.37.0`, während der interne Meilenstein
`v0.46.0` ist. Das kann exportierte Package-, ABI- und Buildmetadaten
verfälschen.

---

# 6. Funktionssicherungsregeln

Für alle Reviewtasks gilt:

1. Keine bestehende korrekte Regression wird gelöscht oder abgeschwächt.
2. Falsche historische Erwartungen werden durch einen unabhängigen
   Spezifikationsnachweis ersetzt und als Bugregression dokumentiert.
3. CPU-/FPU-Semantik läuft durch Decoder, IR, Verifier, generierten C++-Pfad
   und Referenz-/Interpreterpfad.
4. Jeder Runtimefix besitzt Erfolgs-, Grenz-, Fehler-, Alias- und
   Invalidierungsfall.
5. Debug und RelWithDebInfo liefern dieselben Gastresultate.
6. Jeder Fastpath besitzt einen deaktivierbaren sicheren Referenzmodus.
7. Performanceoptimierungen laufen differenziell gegen den Referenzpfad.
8. Gateberichte trennen Korrektheit, Performance und Diagnosemetriken.
9. Ein neuer Task gilt erst als abgeschlossen, wenn die vollständige bestehende
   Regression weiterhin besteht.
10. Keine Reparatur darf Sonic-Adressen, Titelpatches oder stillen Fallback
    einführen.

---

# 7. Neue Priorisierung

## Stufe A: P0-Core-Korrektheit

1. `KR-4611` - SH-4-Kontrollzustand, Delay Slots, RTE, SLEEP und Interrupts
2. `KR-4612` - Store Queue und Cacheadressierung
3. `KR-4613` - einheitliche Gastwrites und Codeinvalidierung
4. `KR-4614` - sounde Kontrollfluss- und Wertanalyse
5. `KR-4615` - stabile und skalierbare Runtime-Blockregistry
6. `KR-4616` - einheitliches Gasttiming und Scheduler-/Geräteintegration
7. `KR-4617` - unabhängige Cross-Engine-Konformitätstests
8. `KR-4618` - Core-Korrektheitsgate

## Stufe B: P1-Performance und Build

9. `KR-4621` - Speicher-, Dispatch- und Invalidierungs-Hotpaths
10. `KR-4622` - inkrementelle Analyse, IR und Codegen
11. `KR-4623` - Disc-, GDI-, ISO- und GD-ROM-I/O
12. `KR-4624` - Buildgraph, Runtime-SDK, Cache und Testmatrix
13. `KR-4625` - Performance-/Buildgate

## Stufe C: bisherige v0.47-Ziele

14. `KR-4715` bis `KR-4719`
15. `KR-4703`
16. `KR-4704`
17. `KR-4705`

Erst danach beginnt der echte Sonic-Runtimelauf der Alpha-Phase.

---

# 8. Schlussurteil

KatanaRecomp verfolgt weiterhin das richtige Ziel und besitzt eine
wesentlich belastbarere Grundarchitektur als titelbezogene Recompiler-
Prototypen. Der aktuelle Funktionsumfang darf aber noch nicht als sichere
Retail-Runtimebasis gelten.

Mehr Analyseabdeckung wäre vor den P0-Reparaturen nicht automatisch
Fortschritt. Sie würde größere Mengen Gastcode unter teilweise falschen CPU-,
Interrupt- und Invalidierungsverträgen erzeugen.

Der richtige nächste Schritt ist daher:

```text
Coresemantik korrigieren
-> Soundness beweisen
-> Hotpaths und Builds vermessen
-> erst danach die 117 Kontrollflussstellen weiter erschließen
```

Damit bleibt das Kernversprechen erhalten: keine Titelhacks, keine stillen
Fehler und keine Geschwindigkeit auf Kosten der Gastsemantik.
