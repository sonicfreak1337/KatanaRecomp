# KatanaRecomp Task-Katalog

Dieses Dokument enthaelt die aktiven `v0.49`-Produktaufgaben. Historische Aufgaben bleiben in Git und in `TASK_ID_REGISTRY.md` nachvollziehbar.

## Verbindliche Regeln

- Oberste Prioritaet ist ein lauffaehiger Sonic-Adventure-PAL-Produktport.
- Das echte Endprodukt ist die Bring-up-Abnahme.
- Produktlaeufe werden nach gleicher Gastarbeit verglichen, nicht nach fixer Hostzeit.
- DirectBoot besitzt keinen Sega-Screen als Pflichtmeilenstein; dieses Bild gehoert zu IP.BIN.
- Keine neue breite Testsuite, Funktionsmatrix oder vorsorgliche Regression.
- Kleine vorhandene Tests duerfen nur angepasst werden, wenn eine sonst schwer sichtbare Datenkorruption abgesichert werden muss.
- Produktlaeufe erfolgen erst nach einem zusammenhaengenden Implementierungsblock.
- Keine Sonic-Adressen, Titelhooks oder Retailbytes im generischen Katana-Code.
- Kein Interpreter, JIT oder Emulationsfallback im normalen Produktpfad.
- Dokumentation und Taskstatus werden erst nach realer Produktevidenz als abgeschlossen markiert.

## Aktueller Produktstand

```text
Roadmap-Basis:                     origin/main 3714e26e117c609980b2b9be6ea0b85c2484c2dd
Produktartefakt:                   DirectBoot-v24 / CompletePlatform

restaurierter Game-Entry-Zyklus:   415.233.270
absolutes Stopmaximum:             600.000.000
tatsaechliche Post-Entry-Zyklen:   184.766.730
Hostzeit:                          5,01505 s
gueltige Post-Entry-Gast-MHz:      36,8425
ungueltige alte Anzeige:           119,64 MHz
Zentraldispatches:                 16.033.676
GD-ROM-Kommandos:                  72
AICA-Audiopuffer:                  179
aktive AICA-Kanaele:               0
sichtbarer Screen:                 keiner
letzter PC:                        0x8C666D42
```

Die alte Anzeige teilte den absoluten Zaehlerstand von 600 Millionen durch
die Hostzeit, obwohl 415.233.270 Zyklen restauriert und nicht ausgefuehrt
wurden. Sie ist kein gueltiger Geschwindigkeitswert. `KR-4966` muss das Gate
auf ein relatives Post-Entry-Budget umstellen.

## Aktueller erster Blocker

Der konkrete Waitvertrag liegt im ADXT-/mwSnd-Soundpfad:

```text
Objekt:               0x8C8D3908
Completion-Flag:      0x8C8D3920
Poll-Callback:        0x8C666D42 (RTS; NOP)
Completion-Writer:    0x8C65A458
```

Der Waitpfad setzt `[object+24]` auf `0` und pollt danach auf `1`. Der Writer setzt dasselbe Feld auf `1`. Alle sechs statisch aufgeloesten Caller liegen in ADXT-/mwSnd-Soundpfaden.

NativeDisc-v24 und DirectBoot-v24 enden am selben Waitvertrag, mit demselben
letzten PC und jeweils 72 GD-ROM-Kommandos. Der CompletePlatform-Handoff allein
schliesst die Sound-Completion daher nicht.

Wahrscheinliche Fehlerzone:

```text
ADXT/mwSnd-Worker
  -> G2/AICA/DMAC
  -> Scheduler/IRQ
  -> Completion-Writer
```

Maple/VMU ist fuer diesen konkreten Poll nicht belegt.

## Verbindliche Reihenfolge

```text
KR-4965 Sound-Completion
  +--> KR-4966 relatives Produktgate
  +--> KR-4967 CompletePlatform-Koordinator
         +--> KR-4968 Sound-/DMA-/IRQ-Handoff
         +--> KR-4969 PVR-/Frame-Handoff
         +--> KR-4970 produkt-sicherer Maple-/VMU-Handoff
                    -> KR-4952 und KR-4953 abschliessen
                    -> KR-4962 Game-Entry-Paritaet und Produktboot

Danach Performance:
KR-4955 -> KR-4956 -> KR-4957 -> KR-4958
                                -> KR-4959
                                -> KR-4960

Spielprojekt:
KR-4954 -> KR-4961

Build:
KR-4963 parallel nach stabiler Handoff-Grundlage

Final:
KR-4960 + KR-4961 + KR-4962 + KR-4963 -> KR-4964
```

---

## [x] KR-4951 - Produktgate nach Gastzyklen und getrennte visuelle Meilensteine

Prioritaet: P0

Status: Baseline umgesetzt. Das Folgeproblem der absoluten statt relativen Schedulergrenze und der fehlenden Pflichtmeilensteinwertung wird in `KR-4966` geschlossen.

### Bereits umgesetzt

- 600-Millionen-Gastzyklus-Gate
- getrennte NativeDisc-/DirectBoot-Meilensteine
- Host-Watchdog nur gegen Hanger
- Bericht fuer Hostzeit, MHz, Dispatches und sichtbaren Meilenstein

### Offene Folgerisiken

- der reale v24-Schedulerrestore lief nur bis zum absoluten Maximum
  `600.000.000` statt fuer diese Laufdauer ab Game-Entry
- `visible_screen=none` kann noch mit Exitcode 0 und `first_problem=none` enden

---

## [ ] KR-4952 - Post-IP.BIN-Spielhandoff fuer DirectBootExecutable

Prioritaet: P0

Abhaengigkeiten: KR-4967, KR-4968, KR-4969, KR-4970

Status: Aktiv. Reales `CompletePlatform`-Capture und produktives Apply sind
belegt. Offen bleiben der strikt globale atomare/`noexcept`-Commitvertrag aus
`KR-4967`, das allgemeine Save-erhaltende Produktprofil aus `KR-4970` und die
normativen Subsystemdigests.

### Bereits belegt

- ein privates CompletePlatform-Artefakt wurde aus NativeDisc erfasst
- der DirectBoot-Produktport hat 22 Geraete und 5 typisierte
  Schedulerereignisse daraus angewendet
- der reale Produktlauf erreichte `GameCodeProgressed` ohne neuen
  terminalen Runtimefehler

### Offen

- nach Commitbeginn darf global kein fallibler Schritt mehr existieren
- aktuelle Nutzersaves muessen fuer jedes Produktartefakt bewahrt werden
- NativeDisc und DirectBoot muessen am Entry pro Subsystem normativ
  uebereinstimmen

### Umfang

- Post-BIOS- und Post-IP.BIN-/Game-Entry-Vertrag getrennt halten
- CPU, RAM, Geraete, IRQ und Scheduler normativ erfassen
- produktiven `CompletePlatform`-Apply verwenden
- CPU/MMU und finalen PC/PR erst nach vollstaendig vorbereitetem Plattformcommit veroeffentlichen
- unvollstaendige Handoffs vor dem ersten Gastblock ablehnen

### Akzeptanz

- kein fest codierter BIOS-Return-PR als Game-Entry-Vertrag
- Handoff ist an Content, Bootdatei, Konsolenprofil, Runtime-ABI und Schema gebunden
- NativeDisc und DirectBoot besitzen am Entry gleiche Subsystemdigests
- keine Retailbytes im Repository

---

## [ ] KR-4953 - Privates Game-Entry-Handoff-Artefakt aus Original-GDI

Prioritaet: P0

Abhaengigkeiten: KR-4967, KR-4968, KR-4969, KR-4970

Status: Aktiv. Das reale CompletePlatform-Artefakt mit 22 Geraeten und 5
Schedulerereignissen wurde erfasst und vom DirectBoot-Produktport verwendet.
Offen bleiben der byteidentische Doppel-Capture, eigene Inspect-/Verify-
Operationen und der allgemeine Schutz aktueller Saves.

### Umfang

- NativeDisc unmittelbar vor der ersten BootExecutable-Instruktion erfassen
- deterministische RAM-/VRAM-/AICA-RAM-Deltas
- typisierte Geraete- und Schedulerdaten
- private Content-Slices hashgebunden speichern
- Product- und Diagnostic-Handoffprofile trennen
- VMU/Saves nicht aus einem alten Capture ueberschreiben

### Akzeptanz

- derselbe lokale Handoff startet DirectBoot reproduzierbar
- Artefakt und Original-GDI bleiben ausserhalb des Repositories
- andere Discversion oder Runtime-ABI wird abgelehnt
- Product-Handoff bewahrt aktuelle Nutzerdaten

---

## [ ] KR-4954 - Deklaratives externes Spielprojekt und CLI-Scaffold

Prioritaet: P1

Abhaengigkeiten: KR-4952

### Umfang

- versionierter TOML-/JSON-Descriptor fuer `GameProjectDefinition`
- CLI-Laden, Validierung und Hashbindung
- symbolische native Overrides und Mid-Function-Hooks
- externes CMake-Scaffold gegen installierte Runtime
- private generierte AOT-Quellen und Contentwurzel gitignored
- DirectBoot-, NativeDisc- und GameEntryHandoff-Konfiguration

### Akzeptanz

- externes Projekt ohne eigenen C++-Exporter erzeugbar
- keine Retailbytes im Descriptor
- Runtime-only-Aenderung erfordert keinen SH-4-Neuexport
- Hook-only-Aenderung baut nur das Spielprojekt

---

## [ ] KR-4955 - Explizite Funktionsgrenzen und Tabellenhinweise End-to-End

Prioritaet: P0 nach stabilem DirectBoot

Abhaengigkeiten: KR-4962

### Problem

`GameProjectFunctionBoundary::size` wird derzeit nicht als exakte Analyzer-Funktionsgrenze uebernommen.

### Umfang

- Analyzer-Override um Groesse oder Endadresse erweitern
- Funktionsintervall exakt anwenden
- Jump-/Callbacktabellen aus dem Spielprojekt binden
- Konflikte mit automatischer Analyse sichtbar melden
- Hookadressen als Architekturgrenzen bewahren

### Akzeptanz

- extern definiertes Funktionsintervall wird exakt emittiert
- Nachbarbytes werden nicht still aufgenommen
- falsche Discidentitaet deaktiviert alle Titelhinweise

---

## [ ] KR-4956 - Static-AOT-Dispatchflucht inventarisieren und schliessen

Prioritaet: P0 Performance

Abhaengigkeiten: KR-4955

Status: Zaehler und `KATANA_STATIC_AOT_ESCAPE_STATS` existieren; reale Top-Ursachen muessen aus dem Produktlauf in den Status uebernommen werden.

### Umfang

- vorhandene Escape-Statistik im normalen Produktlauf erfassen
- dominante Gruende und Top-Sites bestimmen
- keine neue parallele Diagnoseinfrastruktur bauen
- die zwei groessten produktiven Fluchtursachen zuerst schliessen

### Akzeptanz

- alle Zentraldispatches sind klassifiziert
- dominante Gruende sind mit Produktzahlen belegt
- Messung veraendert den schnellen Produktpfad nicht

---

## [ ] KR-4957 - Direkte native Calls ueber sichere Timinggrenzen

Prioritaet: P0 Performance

Abhaengigkeiten: KR-4956

### Umfang

- `NativeEntrySafe`, `DirectCallEligible`, `CompletionDeferrable` und `RequiresSafepointBeforeEntry` trennen
- bekannte Calls partitionsuebergreifend nativ ausfuehren
- Safepoints an echten MMIO-/Scheduler-/IRQ-Grenzen statt pauschal am Call
- direkte native Continuation und Return
- Hookgrenzen weiterhin respektieren

### Akzeptanz

- Funktionen mit spaeterem MMIO duerfen nativ betreten werden
- Scheduler-/IRQ-Reihenfolge bleibt korrekt
- Zentraldispatches sinken im gleichen Produktpfad deutlich

---

## [ ] KR-4958 - IR-basierte Registerlokalisierung und RAM-Regionen

Prioritaet: P1 Performance

Abhaengigkeiten: KR-4957

### Umfang

- C++-Textsuche und `replace_all_text()` aus der Lokalisierung entfernen
- IR-Use/Def und Liveness verwenden
- GPR, T, PR, GBR, MACH/MACL und FPUL schrittweise lokalisieren
- Spillgrenzen fuer MMIO, Exception, IRQ, Hook, SR-/Bankwechsel und dynamischen Dispatch
- bewiesene P1-/P2-Haupt-RAM-Regionen direkt binden

### Akzeptanz

- keine semantische C++-Stringersetzung fuer Register
- normale Speicherzugriffe und direkte Calls in lokalisierten Funktionen
- gleiche Gastzyklen und gleicher sichtbarer Meilenstein

---

## [ ] KR-4959 - Ereignisgetriebene Scheduler-/IRQ-Safepoints

Prioritaet: P1 Performance und Korrektheit

Abhaengigkeiten: KR-4968, KR-4956

### Umfang

- Interrupt-Epoch, hoechstes Pending-Level und Pending-Maske als billigen Guard
- voller Routerwalk nur bei echter Zustandsaenderung
- Schedulercommit ueber native Regionen sammeln
- Safepoints vor faelligen Events, MMIO, Exceptions, Hooks, SR-/MMU-Aenderungen oder Quantum
- Host-Lifecycle nicht pro Dispatch ueber Wall-Clock pollen

### Akzeptanz

- keine verlorene Sound-/DMA-/IRQ-Completion
- IRQ-/Schedulerarbeit pro Gastzyklus sinkt
- Produkt- und billige Diagnose nutzen dieselbe Architektur

---

## [ ] KR-4960 - 200-MHz-Produkt-Hotpath

Prioritaet: P0 Performance-Gate

Abhaengigkeiten: KR-4957, KR-4958, KR-4959

### Umfang

- statisches AOT ohne Materializerarbeit im Normalfall
- validierten Ausfuehrungsdeskriptor direkt verwenden
- Block-/Fastpathmetadaten ohne Lookup weiterreichen
- Function-AOT statt Owner-Wrapper/PC-Switch so weit wie sicher
- `dynamic_cast` aus bekannten Produktfastpaths entfernen
- keine Erfolgsstrings oder detaillierten Sitemaps im Hotpath
- MSVC und clang-cl am selben Produktport vergleichen

### Akzeptanz

- mindestens 200 MHz im normalen Produktlauf
- Zielreserve mindestens 250 MHz unpaced
- sichtbarer Bootmeilenstein bleibt erhalten
- keine kuenstlich reduzierten Gastzyklen oder Geraetelatenzen

---

## [ ] KR-4961 - Externes SonicAdventureRecomp-Bring-up-Projekt

Prioritaet: P1 nach produktivem Handoff

Abhaengigkeiten: KR-4954, KR-4955, KR-4953

### Umfang

- externes `SonicAdventureRecomp` aus Scaffold
- generierter SA-Code lokal/gitignored
- PAL-Identitaet, Funktionen, Tabellen, Symbole und Hooks im Spielprojekt
- lokale Entwicklerinstallation ausserhalb des Repositories
- spaeterer Nutzerinstaller auf demselben Contentvertrag
- DirectBoot als Bring-up, NativeDisc als Referenz

### Akzeptanz

- Katana-Kern und Runtime enthalten keine Sonic-Sonderfaelle
- echter SA-Produktlauf startet aus dem externen Projekt
- Originaldaten bleiben lokal

---

## [ ] KR-4962 - NativeDiscBoot-/DirectBoot-Paritaet am Game-Entry

Prioritaet: P0 Boot-Gate

Abhaengigkeiten: KR-4952, KR-4953, KR-4967, KR-4968, KR-4969, KR-4970

Status: Aktiv. NativeDisc-v24 und DirectBoot-v24 enden am selben PC
`0x8C666D42`, mit jeweils 72 GD-ROM-Kommandos und nahezu gleichen
AICA-Pufferzaehlern (180 beziehungsweise 179). Das ist operative Evidenz,
aber kein normativer Digestnachweis. Sound-Completion, erster Spielframe und
ein echter Lauf ueber 600 Millionen Post-Entry-Zyklen bleiben offen.

### Umfang

- NativeDisc bis unmittelbar vor erste BootExecutable-Instruktion
- DirectBoot mit CompletePlatform-Handoff
- per Subsystem Digests vergleichen:
  - CPU/MMU
  - Main RAM/VRAM/AICA RAM
  - PVR/SPG/ASIC/IRQ
  - GD-ROM/G1/DMAC
  - AICA/G2/Sound
  - Maple/VMU
  - Timer/Scheduler
- erste Abweichung typisieren
- danach 600 Millionen Gastzyklen **ab Entry** laufen

### Akzeptanz

- normative Entry-Digests stimmen
- ADXT-/mwSnd-Completionwriter fortgeschritten
- mindestens `FirstGameFramebufferWrite` oder `FirstTaFrame`
- bei verfehltem Meilenstein Exitcode 3 und konkreter Blocker

---

## [ ] KR-4963 - Inkrementeller Runtime-/Spielbuild und Compiler-A/B

Prioritaet: P1

Abhaengigkeiten: stabile Handoff-Grundlage

Status: Der reale unveraenderte warme Direct-v24-Export/Build dauerte etwa
5,3 Sekunden; der erste frische Direct-v24-Export/Build etwa 169,3 Sekunden.
Runtime-only-, Hook-only- und aktueller MSVC-/clang-cl-Produktvergleich
bleiben offen.

### Umfang

- Analyse, IR, Emission, Runtimecompile, AOT-Compile und Link getrennt messen
- Runtime-only nur Runtime rebuilden und Spiel relinken
- Hook-only nur Spielprojekt bauen
- AOT-ABI-Header weiter verschlanken
- Bring-up ohne LTCG, Gate voll optimiert
- MSVC-/clang-cl-Produktvergleich

### Akzeptanz

- Runtimefix plus Relink unter 30 Sekunden als Ziel
- Hook-only unter 15 Sekunden als Ziel
- geaenderte AOT-Partition unter 90 Sekunden
- unveraenderte Partitionen nicht neu emittieren

---

## [ ] KR-4964 - v0.49 Produktabnahme bis sichtbarem Spielbild

Prioritaet: Gate

Abhaengigkeiten: KR-4960, KR-4961, KR-4962, KR-4963

### Umfang

- externen SA-Produktport bauen
- lokale Originaldiscinstallation identitaetspruefen
- CompletePlatform-DirectBoot bis sichtbarem Spiel-/TA-Frame
- NativeDisc als Entry-Paritaetsreferenz
- 200-MHz- und Buildziele dokumentieren

### Akzeptanz

- `BootExecutableEntry`
- `GameCodeProgressed`
- Sound-Completion
- `FirstVisibleGameFrame`
- mindestens 200 MHz
- keine Retaildaten oder Sonic-Sonderfaelle im Katana-Kern

---

## [ ] KR-4965 - ADXT/mwSnd-Sound-Completion bis zum Writer schliessen

Prioritaet: P0 - zuerst

Abhaengigkeiten: keine

Status: Aktiv und weiterhin erster P0-Produktblocker. NativeDisc-v24 und
DirectBoot-v24 erreichen denselben ADXT-/mwSnd-Waitvertrag; der
CompletePlatform-Handoff hat den Completion-Writer noch nicht freigegeben.

### Produktbefund

- Objekt `0x8C8D3908`
- Completion-Flag `0x8C8D3920`
- Poll-Callback `0x8C666D42`
- Completion-Writer `0x8C65A458`
- sechs statische Caller in ADXT-/mwSnd-Soundpfaden

### Umfang

- Ausloeser des Soundworkers bestimmen
- G2-/AICA-/DMAC-Transfer und erwartete IRQ-/Schedulerkante zuordnen
- erste verlorene Kante zwischen Workerstart und Completion-Writer belegen
- allgemeine Ursache reparieren
- kein direktes Hostsetzen des Completion-Flags
- keine private Adresse als generische Sonderbehandlung

### Produktabnahme

Erst nach der zusammenhaengenden Implementierung ein normaler DirectBoot mit derselben post-entry Gastarbeit wie die Baseline.

Erfolg:

- Gast erreicht den echten Completion-Writer und Flag wird `1`, oder
- ein engerer allgemeiner Blocker ist belegt.

Keine neue breite Soundtestmatrix.

---

## [ ] KR-4966 - Post-Entry-Produktgate und erforderliche Meilensteine

Prioritaet: P0

Abhaengigkeiten: KR-4951

Status: Aktiv. Der konkrete v24-Produktlauf belegt den absoluten Budgetfehler.

### Produktbefund

```text
restaurierter Game-Entry-Zyklus: 415.233.270
absolutes Stopmaximum:           600.000.000
ausgefuehrte Post-Entry-Zyklen:  184.766.730
Hostzeit:                        5,01505 s
gueltige effektive Gast-MHz:     36,8425
ungueltig berichteter Wert:      119,64 MHz
```

Die 119,64 MHz verwenden den restaurierten absoluten Zaehlerstand als
ausgefuehrte Arbeit. NativeDisc und DirectBoot haben damit noch keine gleiche
Post-Entry-Arbeit erhalten.

### Umfang

- Gastzyklusbudget als Laufdauer ab Game-Entry definieren
- restaurierten Schedulerzyklus nicht vom Laufbudget abziehen
- erforderlichen Produktmeilenstein im GameProject definieren
- `visible_screen=none` nicht als erfolgreichen Endzustand melden
- Exitcodes unterscheiden:
  - `0` Meilenstein erreicht
  - `3` Budget erreicht, Meilenstein verfehlt
  - `1` typisierter Fehler

### Akzeptanz

- NativeDisc und DirectBoot erhalten dieselbe post-entry Gastarbeit
- schwarzer Lauf endet nicht mit Erfolg
- Produktbericht nennt den verfehlten Meilenstein als erstes Problem

---

## [ ] KR-4967 - Atomarer CompletePlatform-Capture-/Apply-Koordinator

Prioritaet: P0

Abhaengigkeiten: KR-4966

Status: Aktiv, teilweise umgesetzt. Vollstaendige Vorvalidierung, passive
Restoreplaene, semantischer Recapture und ein reales produktives
CompletePlatform-Apply sind belegt. Offen bleiben ein strikt globaler
`noexcept`-Commit nach der ersten Mutation und normative Digests pro
Subsystem.

### Umgesetzter Zwischenstand

- alle 22 Geraetepayloads und 5 typisierten Schedulerereignisse werden vor
  dem Apply validiert
- Geraete besitzen passive Restore-/Game-Entry-Adapter
- der angewendete Zustand kann semantisch erneut erfasst werden
- der DirectBoot-v24-Produktlauf hat den Koordinator real verwendet

### Umfang

- globaler Prepare-/Commit-Vertrag
- alle Allokationen, Deltas, Geraeteplaene und Events vor Mutation vorbereiten
- passive Geraeterestoreplaene
- Schedulerzeit und Eventrehydrierung
- CPU/MMU zuletzt committen
- kein fallibler Schritt nach Commitbeginn
- per Subsystem semantische Digests

### Akzeptanz

- kein teilweise angewendeter Handoff
- Produktpfad entfernt `game-entry-handoff-complete-platform-apply-unavailable`
- fehlendes oder inkonsistentes Subsystem wird vor Gastcode abgelehnt

---

## [ ] KR-4968 - AICA-/G2-/DMAC-/Scheduler-/IRQ-Handoff fuer Soundfortschritt

Prioritaet: P0

Abhaengigkeiten: KR-4965, KR-4967

Status: Aktiv. Die erforderlichen Game-Entry-Adapter sind implementiert und
im CompletePlatform-Artefakt transportiert. Die echte Sound-Completion und
die exakte NativeDisc-/DirectBoot-Paritaet dieses Subsystemverbunds bleiben
offen.

### Umfang

- AICA Register, RTC und Execution Controller
- G2-Kanaele und laufende Transfers
- SH-4-DMAC/G2-Kopplung
- System-ASIC-/IRQ-Quellen fuer Soundcompletion
- TMU/RTC, soweit fuer Workerfortschritt erforderlich
- typisierte ausstehende Events
- aktive Transfers mit frischen Event-IDs rehydrieren

### Akzeptanz

- aktiver Soundtransfer besitzt genau passende Events und IRQ-Quellen
- Worker-/Completionwriter verhalten sich in NativeDisc und DirectBoot gleich
- keine verlorene oder doppelte Completion

---

## [ ] KR-4969 - PVR-/SPG-/ASIC-Handoff fuer den ersten Spiel-Frame

Prioritaet: P0

Abhaengigkeiten: KR-4967

Status: Aktiv. Die PVR-/SPG-/ASIC-Game-Entry-Adapter sind implementiert und
wurden im realen Produkt-Apply verwendet. Ein erster Spielframe und die
normative NativeDisc-/DirectBoot-Paritaet bleiben offen.

### Umfang

- PVR Registerfile und SPG-Timing
- Framebufferbasis, SOF1/SOF2 und Scanoutzustand
- VRAM-Deltas
- TA FIFO/YUV/Renderzustand nur soweit am Entry aktiv
- System-ASIC-Masken und pending Quellen
- laufende Render-/DMA-Ereignisse typisiert rehydrieren

### Akzeptanz

- NativeDisc und DirectBoot besitzen gleichen aktiven Scanoutzustand
- Gamecode-Framebufferwrite oder TA-Frame kann sichtbar werden
- kein Hostframe wird ohne Gastnachweis erfunden

---

## [ ] KR-4970 - Produkt-sicherer Maple-/VMU-Handoff und Event-Rehydration

Prioritaet: P0

Abhaengigkeiten: KR-4967

Status: Aktiv. Game-Entry-Adapter und Produktlauf sind belegt; identische
migrierte Saves wurden in einem realen Fall nachgewiesen. Ein allgemeines
Product-Handoff-Profil mit garantiertem Schutz vor Save-Rollback bleibt
offen.

### Umfang

- `DiagnosticLossless` und `ProductHandoff` trennen
- Product-Handoff bewahrt aktuelle VMU-Working-Copy und Saves
- Topologie, Controller-/VMU-Typ, MMIO- und DMA-Zustand uebernehmen
- Diagnosehistorie und reine Hostmetriken nicht als Produktzustand behandeln
- aktive Maple-DMA exakt einem typisierten Schedulerereignis zuordnen
- vorbereiteten `noexcept` Commitplan liefern

### Akzeptanz

- kein Savegame-Rollback durch Handoff
- aktive DMA besitzt exakt eine Completion
- inaktive DMA besitzt kein Completionevent
- NativeDisc und DirectBoot stimmen in gastseitig sichtbarem Maple-Zustand

---

## Geplante Produktlaeufe

### Lauf A - nach KR-4965

- gewoehnlicher DirectBoot
- gleiche post-entry Gastarbeit wie Baseline
- Sound-Completionwriter oder engerer Blocker

### Lauf B - nach KR-4967 bis KR-4970

- NativeDisc Capture am Entry
- DirectBoot Apply bis vor ersten Spielblock
- Subsystemdigests vergleichen

### Lauf C - nach KR-4962

- 600 Millionen Gastzyklen ab Entry
- mindestens FirstGameFramebufferWrite oder FirstTaFrame
- Exitcode 3 bei verfehltem Meilenstein

### Lauf D - nach KR-4960

- derselbe Produktpfad
- mindestens 200 MHz
- sichtbarer Meilenstein bleibt erhalten

Zwischen diesen Produktlaeufen sind keine Vollsuiten vorgesehen. Ein kleiner bestehender Test darf nur angepasst werden, wenn eine schwer sichtbare Datenkorruption sonst nicht abgesichert werden kann.
