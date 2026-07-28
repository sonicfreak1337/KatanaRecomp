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

Historische v24-Vergleichsbasis:

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

Aktueller v28-Funktionslauf:

```text
Main-Basis vor Implementierung:    8e5ab3145fb5fcafc056fd87025baf3497085342
Produktartefakt:                   DirectBoot-v28 / GameProject + CompletePlatform
GameProject-Artefaktidentitaet:    sha256:9d4edff0270275f0b4931b733b2bd03ef330893f79d4728d6580adaf1107249f
MSVC-Gateexport:                   1.946 Funktionen, 42 Partitionen
Discinstallation:                  3 Tracks, 521.461 Sektoren
Retailsektoren Repo / Portpaket:   0 / 0
restaurierter Game-Entry-Zyklus:   415.233.270
CompletePlatform-Apply:            22 Geraete, 5 Events
Endzyklus am typisierten Fehler:   553.990.562
Post-Entry-Zyklen:                 138.757.292
Fortschritt gegen v26:             +1.086.915 Zyklen
externe Walltime bis Fehler:       5,275792 s
Post-Entry-Rate bis Fehler:         26,3008 MHz
v26-Vergleich bis Fehler:          5,746371 s / 23,9578 MHz
warmer unveraenderter Hostbuild:   0,219272 s
vollstaendiger warmer Export:      4,209083 s
Exportcache:                       42 Partitionshits, Analyse/IR + Metadaten Hit
Produkt-EXE:                       52.446.208 Bytes (v26: 52.406.784)
Zentraldispatches:                 10.079.932
Dispatches gegen v26:              +123.498
G2-Kanaele:                        alle inaktiv
GD-ROM-Kommandos:                  72
AICA-Audiopuffer:                  165
PVR-Gast-/Direct-Frames:           2 / 2
veraenderte Direct-Pixel:          302.287
sichtbarer Screen:                 keiner
terminale Diagnose:                aot-template-mismatch
Materializergrund:                 AotTemplateMismatch (14)
Callsite / Ziel:                   0x8C11088C / 0x8C64784E
```

Der Lauf ist keine 600-Millionen-Performanceabnahme: Er endet
`46.009.438` Zyklen vor dem weiterhin falsch absoluten Maximum. Die
`26,3008 MHz` aus der tatsaechlichen Post-Entry-Arbeit sind nur ein
provisorischer Vergleich gegen `23,9578 MHz` bei v26 (`+9,78 %`) bis zum
funktionalen Fehler bei identischem Restore; sie ersetzen kein relatives
600-Millionen-Gate. Sechzehn reale Fensteraufnahmen blieben schwarz.

## Aktueller erster Blocker

Der fruehere Waitvertrag lag im ADXT-/mwSnd-Soundpfad:

```text
Objekt:               0x8C8D3908
Completion-Flag:      0x8C8D3920
Poll-Callback:        0x8C666D42 (RTS; NOP)
Completion-Writer:    0x8C65A458
```

Der Waitpfad setzt `[object+24]` auf `0` und pollt danach auf `1`. Der Writer setzt dasselbe Feld auf `1`. Alle sechs statisch aufgeloesten Caller liegen in ADXT-/mwSnd-Soundpfaden.

Die erste verlorene Kante bestand aus zwei allgemeinen Holly-G2-Fehlern:

```text
SB_G2APRO 0x4659404F
  -> Start-/Endbyte waren vertauscht

ADTSEL 5 + ADST 1
  -> CPU-Start mit AICA-Request-Level
  -> SB_FFST.bit0=0 wurde beim Armieren nicht ausgewertet
```

Beides ist generisch repariert. v26 schliesst G2-Kanal 0 ab und verlaesst
`0x8C666D42`, ohne das Flag vom Host zu setzen und ohne eine Titeladresse in
den Kern einzubauen. Der Writer beziehungsweise der Flagwechsel wurde nicht
separat beobachtet; KR-4965 ist durch den nachgewiesenen engeren allgemeinen
Blocker abgenommen.

KR-4971 ist abgeschlossen. Das private externe `GameProjectArtifact` seedet
die im v26-Lauf beobachtete exakte Grenze `0x8C010F22 + 0x18` hashgebunden
in Analyzer, CFG, IR und AOT. v28 passiert das alte Ziel; keine Sonic-Adresse
wurde in generischen Code aufgenommen.

Der erste aktive Blocker ist jetzt KR-4972:

```text
Dispatchlabel: aot-template-mismatch
Materializer:  AotTemplateMismatch (14)
Callsite:      0x8C11088C
Ziel:          0x8C64784E
Gastzyklus:    553.990.562
```

Das Ziel liegt unveraendert im initialen Boot-Executable und beginnt mit
einem `BRA` zum gemeinsamen Zielpfad `0x8C6478C2`. Das ist ein Hinweis auf
einen Callback-/Shared-Tail-/Thunk-Vertrag, aber noch kein Beweis fuer die
exakte Funktionsgrenze oder endgueltige Modellierung. Ein Interpreter-, JIT-,
Runtime-Decoder- oder Emulationsfallback bleibt verboten.

## Verbindliche Reihenfolge

```text
KR-4965 Sound-Completion [abgeschlossen ueber engeren Blocker]
  +--> KR-4971 RuntimeOnly-AOT-Coverage fuer statisch identifizierbares Ziel
         [abgeschlossen]
         +--> KR-4972 Hashgebundene Shared-Callback-/Thunk-AOT-Coverage
                herstellen [zuerst]
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

Status: Teilweise umgesetzt. `GameProjectArtifact` Format 1 kann eine
vollstaendige deklarative Definition mit Payload- und Artefakt-SHA-256
schreiben und laden. `port-executable --game-project` validiert die exakte
Boot-/Contentidentitaet und kann mit `--game-entry-handoff` kombiniert werden.
Ein benutzerfreundlicher Textdescriptor und das externe CMake-Scaffold bleiben
offen.

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

Status: Teilweise umgesetzt. `GameProjectFunctionBoundary::size` wird jetzt
als exakte Grenze durch AnalysisOverride/-Seed, Funktionskandidaten, CFG, IR
und AOT transportiert. Analyzer-ABI 3 versioniert diese Layoutaenderung. Der
v28-Produktport belegt die exakte externe Grenze fuer das zuvor fehlende
statische Ziel. Ungerade oder ungueltige Groessen, widerspruechliche
Grenzdefinitionen, Ueberlappungen und Delay-Slot-Splits werden fail-closed
abgewiesen. Jump-/Callbacktabellen, Konflikte mit automatisch erkannten
inneren Grenzen und die breitere Produktabnahme bleiben offen.

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

Status: Teilweise begonnen. Ein privates externes, vollstaendig hashgebundenes
Sonic-CMake-Projekt baut gegen das installierte
`KatanaRecomp::runtime_core`, erzeugt das `GameProjectArtifact` fuer den
v28-Produktport und haelt die beobachtete Funktionsgrenze ausserhalb des
Katana-Repositories. Ein wiederverwendbares oeffentliches Scaffold sowie der
normale Hook-/Port-Buildworkflow bleiben offen.

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

Status: Aktiv. Der alte gemeinsame End-PC `0x8C666D42` ist durch v26/v28
ueberholt: DirectBoot beendet die konkrete G2-DMA und erzeugt zwei technische
Direct-Frames. v28 passiert zusaetzlich das alte KR-4971-Ziel, endet aber am
Shared-Callback-/Thunk-AOT-Fehler aus KR-4972. Das ist funktionaler
Fortschritt, jedoch kein normativer Digestnachweis. Sichtbarer
Spielframe, unabhaengiger NativeDisc-Vergleich und ein echter Lauf ueber
600 Millionen Post-Entry-Zyklen bleiben offen.

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

Status: Der vollstaendige warme v28-MSVC-Gateexport dauerte `4,209083 s` mit
`42` Partitionscachehits sowie Analyse-/IR- und Metadatenhit. Ein unveraenderter
Hostbuild dauerte `0,219272 s`; die EXE ist `52.446.208` Bytes gross.
Der erste frische Direct-v24-Export/Build dauerte etwa `169,3 s`.
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

## [x] KR-4965 - ADXT/mwSnd-Sound-Completion bis zum Writer schliessen

Prioritaet: P0 - zuerst

Abhaengigkeiten: keine

Status: Abgeschlossen ueber die ausdrueckliche Alternativabnahme
"engerer allgemeiner Blocker". v26 beendet den konkreten G2-Transfer und
verlaesst den ADXT-/mwSnd-Waitvertrag. Der Writer-/Flagwechsel wurde nicht
separat instrumentiert.

### Produktbefund

- Objekt `0x8C8D3908`
- Completion-Flag `0x8C8D3920`
- Poll-Callback `0x8C666D42`
- Completion-Writer `0x8C65A458`
- sechs statische Caller in ADXT-/mwSnd-Soundpfaden
- `SB_G2APRO=0x4659404F` wurde byteverkehrt dekodiert
- `ADTSEL=5` benoetigt beim Armieren das reale AICA-Request-Level aus
  `SB_FFST`

### Umfang

- Ausloeser des Soundworkers bestimmen
- G2-/AICA-/DMAC-Transfer und erwartete IRQ-/Schedulerkante zuordnen
- erste verlorene Kante zwischen Workerstart und Completion-Writer belegen
- allgemeine Ursache reparieren
- kein direktes Hostsetzen des Completion-Flags
- keine private Adresse als generische Sonderbehandlung

### Produktabnahme

Der reale v26-DirectBoot wurde nach der zusammenhaengenden Implementierung
ausgefuehrt.

Erfolg:

- Gast erreicht den echten Completion-Writer und Flag wird `1`, oder
- ein engerer allgemeiner Blocker ist belegt.

Ergebnis: G2-Kanal 0 endet mit `active=0`, `remaining=0`; der alte Poll-PC
wird verlassen. Bei Gastzyklus `552.903.647` folgt der engere allgemeine
RuntimeOnly-AOT-Blocker aus KR-4971. Keine neue breite Soundtestmatrix.

---

## [x] KR-4971 - RuntimeOnly-AOT-Coverage fuer statisch identifizierbares Ziel herstellen

Prioritaet: P0

Abhaengigkeiten: KR-4965

Status: Abgeschlossen. Das v26-Ziel wird ueber eine exakte externe,
hashgebundene Funktionsgrenze statisch analysiert und emittiert. Der reale
v28-Port passiert dieses Ziel und endet an einem neuen, engeren allgemeinen
Blocker. `AotTemplateMismatch` wird terminal getrennt von
`ByteIdentityMismatch` diagnostiziert.

### Produktbefund

```text
Gastzyklus:                   552.903.647
Post-Entry-Zyklen:            137.670.377
Callsite:                     0x8C602B0A
RuntimeCode-Ziel:             0x8C010F22
terminales Dispatchlabel:     byte-identity-mismatch (irrefuehrend)
interner Materializerfehler:  AotTemplateMismatch (14)
Materializer-Anfragen:        2
Materialisierungen:           1
Materializer-Misses:          1
RuntimeOnly-Dispatchanteil:   282.818 ppm
```

### Umfang

- statisch identifizierbare Ziele ueber externe, hash-/bytegebundene
  Spielprojekt-Funktionsgrenzen in Analyse und AOT seeden
- `GameProjectFunctionBoundary::size` bis zur Analyse transportieren
- externes GameProject und GameEntryHandoff im Export gemeinsam verwenden
- fuer das beobachtete Ziel eine statisch erzeugte, bei Laufzeit
  identitaetsgepruefte AOT-Variante bereitstellen
- Code-, Modul-, Relocation- und Blockgenerationen korrekt invalidieren
- `AotTemplateMismatch`, echten `ByteIdentityMismatch` und fehlendes AOT
  terminal getrennt diagnostizieren
- keinen Interpreter, JIT, Runtime-Decoder oder Emulationsfallback einfuehren
- keine Titeladresse als generischen Sonderfall einbauen

### Produktabnahme

- Der finale v28-MSVC-Gateport erzeugt `1.946` Funktionen in `42` Partitionen.
- Das private externe Artefakt bindet die exakte Grenze
  `0x8C010F22 + 0x18` an Boot- und Contentidentitaet; generischer Code
  enthaelt die Adresse nicht.
- Der alte Call passiert; der neue erste Fehler folgt erst bei
  `0x8C11088C -> 0x8C64784E`.
- Der Lauf gewinnt `1.086.915` Gastzyklen gegen v26 und behaelt zwei
  technische PVR-Direct-Frames, `302.287` geaenderte Pixel sowie vollstaendig
  inaktive G2-Kanaele.
- Die terminale Klasse lautet korrekt `aot-template-mismatch`.
- Sechzehn reale Fensteraufnahmen bleiben schwarz; 600-Millionen- und
  Performanceabnahme bleiben offen.

---

## [ ] KR-4972 - Hashgebundene Shared-Callback-/Thunk-AOT-Coverage herstellen

Prioritaet: P0 - zuerst

Abhaengigkeiten: KR-4971

Status: Teilweise umgesetzt, weiterhin erster Produktblocker. Die generische
Analyse gewinnt das Ziel aus Candidate-Tail-Jumps und einem bewiesenen
Runtime-Stackframe zurueck. Der vollstaendige Export mit dem externen
Spielprojekt uebernimmt den gewonnenen Seed aber noch nicht in CFG,
Source-Map und AOT. Der reale v30-DirectBoot endet deshalb weiterhin am
indirekten Call auf das unveraenderte Ziel des initialen Boot-Executables.

### Produktbefund

```text
Gastzyklus:                   553.990.562
Post-Entry-Zyklen:            138.757.292
Callsite:                     0x8C11088C
RuntimeCode-Ziel:             0x8C64784E
terminales Dispatchlabel:     aot-template-mismatch
interner Materializerfehler:  AotTemplateMismatch (14)
zentrale Dispatches:          10.079.932
sichtbarer Screen:            keiner; 15 Aufnahmen schwarz
```

Das unveraenderte Ziel beginnt mit einem `BRA` auf den gemeinsamen Zielpfad
`0x8C6478C2`. Die allgemeine Analyse erkennt `0x8C64784E` jetzt als Funktion
und `0x8C6478C2` als erreichbaren gemeinsamen Body. Die Analyse endet mit
`1.715` Seeds, `1.756` Funktionen und `154.092` Instruktionen ohne
Budgeterschoepfung. Der aus Commit
`854141b8780626e24815c0bbbb60b5927635a1a6` frisch erzeugte
v30-Produktport erzeugt dagegen keinen CFG-, Source-Map- oder AOT-Eintrag
fuer das Ziel und reproduziert die v28-Grenze exakt.

### Umfang

- [x] Caller-, Callback- und Shared-Tail-Herkunft im generischen Analyzer
  ueber konkrete Codepointer-Provenienz bestimmen
- [x] Runtime-Frame-Spill, Reload und Objektstore mit engen Guards
  modellieren
- [ ] den generisch gewonnenen Seed durch die externe
  Spielprojekt-/Exportkonfiguration bis in CFG, IR und AOT erhalten
- [ ] nur falls danach noch erforderlich: bewiesene Metadaten hashgebunden
  im externen Spielprojekt ablegen
- keine Titeladresse als Sonderfall in KatanaRecomp oder KatanaRuntime
- kein Interpreter, JIT, Runtime-Decoder oder Emulationsfallback

### Produktabnahme

- derselbe echte DirectBoot-Port passiert `0x8C64784E` ueber validiertes
  statisches AOT oder belegt einen noch engeren allgemeinen Blocker
- Sound-/G2- und technische PVR-Evidenz bleiben erhalten
- reale Discinstallation und sichtbare Aufnahme werden erneut ausgefuehrt
- vorzeitiger Fehler wird nicht als 600-Millionen-Performancewert ausgegeben

Der v30-Lauf erfuellt die erneute Discinstallation und Sichtpruefung, aber
nicht die erste Abnahmebedingung. KR-4972 bleibt deshalb offen.

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

Status: Aktiv mit belegtem Produktfortschritt. Die erforderlichen
Game-Entry-Adapter sind implementiert und im CompletePlatform-Artefakt
transportiert. v26 und v28 beenden die nach dem Entry armierte AICA-G2-DMA mit
`active=0`, `remaining=0`. Exakte NativeDisc-/DirectBoot-Paritaet und der
Abgleich eines bereits restaurierten aktiven Hardware-Request-Kanals ohne
Completionevent bleiben offen.

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

Status: Aktiv mit technischem Framefortschritt. Die PVR-/SPG-/ASIC-
Game-Entry-Adapter sind implementiert und wurden im realen Produkt-Apply
verwendet. v28 meldet weiterhin zwei Gast-/Direct-Frames mit `302.287`
veraenderten Pixeln. Sechzehn reale Fensteraufnahmen bleiben schwarz; sichtbare
Hostpraesentation und normative NativeDisc-/DirectBoot-Paritaet sind offen.

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

### Lauf A - nach KR-4965 [ausgefuehrt]

- v26-DirectBoot mit real installiertem PAL-Disc-Cache
- G2-Kanal 0 abgeschlossen, alter Sound-Poll verlassen
- engerer RuntimeOnly-AOT-Blocker bei `0x8C010F22`
- zwei technische Direct-Frames, aber kein sichtbarer Hostframe

### Lauf A2 - nach KR-4971 [ausgefuehrt]

- v28 mit privatem `GameProjectArtifact` und CompletePlatform-Handoff
- altes RuntimeCode-Ziel ueber hashgebundene statische AOT-Variante passiert
- `1.086.915` Gastzyklen Fortschritt gegen v26
- neuer engerer Blocker aus KR-4972
- 16 reale Aufnahmen schwarz; keine 600-Millionen-Abnahme

### Lauf A3 - KR-4972-Analyserunde [ausgefuehrt, Abnahme offen]

- generischer Analyzer erkennt `0x8C64784E` und den gemeinsamen Body
  `0x8C6478C2`
- v30 wurde frisch exportiert, mit der privaten Disc installiert und real
  ausgefuehrt
- produktive CFG, Source-Map und AOT-Ausgabe enthalten den Seed noch nicht
- unveraenderter Fehler bei `553.990.562` Gastzyklen und `10.079.932`
  Zentraldispatches
- 15 reale Fensteraufnahmen schwarz

### Lauf A4 - nach KR-4972-Exportintegration

- gewonnenen generischen Seed bis in produktive CFG, IR und AOT erhalten
- denselben gewoehnlichen DirectBoot erneut installieren und ausfuehren
- Ziel ueber validiertes statisches AOT passieren oder einen engeren
  typisierten Blocker belegen

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
