# KatanaRecomp Task-Katalog

Dieses Dokument enthaelt die aktiven `v0.49`-Produktaufgaben. Historische Aufgaben bleiben in Git und in `TASK_ID_REGISTRY.md` nachvollziehbar.

## Verbindliche Regeln

- `AGENTS.md` gilt fuer jeden Task und jeden automatisierten Bearbeiter.
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
- Kein Prozess und keine einzelne Phase laeuft laenger als 15 Minuten, ausser
  der Nutzer hebt die Grenze ausdruecklich fuer genau einen benannten Lauf auf.
- Jeder potentiell lange Prozess besitzt spaetestens alle zehn Sekunden einen
  belastbaren Fortschrittsindikator oder Heartbeat.
- Jeder P0-Task wird fokussiert verifiziert, einzeln committed und gepusht.
- Vor dem naechsten privaten Sonic-Port folgt zwingend die unabhaengige
  Gesamtpruefung KR-4984 mit Schliessung und Re-Review aller P0/P1-Funde.

## Getrennte Evidenzstaende

Diese drei Staende duerfen nicht als derselbe Fortschritt berichtet werden:

1. **Letzte reale Produktevidenz:** Der historische ABI-77/78-
   NativeDisc-Stand bleibt die letzte ausgefuehrte Produktevidenz. Seine
   Details und sichtbaren Screens stehen in `STATUS.md`; er ist kein Nachweis
   fuer den aktuellen Source.
2. **Aktueller Source-Checkpoint:** `18f8537` verwendet Runtime-ABI 85,
   Block-ABI 5, Analyzer-ABI 23, PlatformServices-ABI 13,
   Backend-Interface-ABI 12, Portprojektvertrag 75, Native-AOT-Profil 13 und
   Partitionsschema 5. Er enthaelt umfangreiche Analyse-, Cache-,
   Fortschritts-, Runtime-CPU- und D3D11-Umbauten, ist aber kein
   P0-Abschluss oder Produktproof.
3. **Letzter Exportversuch:** Der lokale NativeDisc-v24-Iterationslauf wurde
   nach etwa `3 h 27 min` mitten in der dritten vollstaendigen
   Function-Value-Neuberechnung beendet. Er erzeugte kein Portartefakt,
   keinen Sonic-Lauf und keinen Screenshot.

Der naechste kritische Pfad ist ausschliesslich KR-4974 bis KR-4984. Erst
nach bestandenem 8-/12-/24-Thread-Performancegate, beweispflichtiger
GPU-Entscheidung und unabhaengiger Gesamtpruefung folgt genau ein frischer
NativeDisc-Port mit realer Discinstallation, Produktlauf und neuem echten
Fensterscreenshot. DirectBoot wird spaeter mit einem an den dann aktuellen
Runtime-ABI gebundenen CompletePlatform-Handoff geprueft; NativeDisc
benoetigt keinen Handoff.

## Aktueller Produktstand

Letzte reale Produktevidenz, NativeDisc-v33 aus `1629268`:

```text
kalter Gesamtexport:              711,2 s
Funktionen / Partitionen:         2.519 / 63
Produkt-EXE:                      109.217.792 Bytes
Originaldisc-Installation:        3 Tracks / 521.461 Sektoren
finaler Gesamtzyklus:             487.233.787
Post-Entry-Zyklen:                72.000.517
Post-Entry-Zentraldispatches:     9.044.195
Hostframes:                       8
hoechster belegter Meilenstein:   IP.BIN-Frame
erster typisierter Fehler:        missing-aot / guarded-fallback
Callsite / Ziel:                  0x8C65EA06 / 0x8C0101F2
```

Der direkte unveraenderte Ninja-Warmbuild dauerte `0,200236 s`. Ein
identischer Voll-Warmexport traf den Whole-Export-Cache nicht und wurde nach
`124 s` beendet. Der anschliessende Exportversuch aus `7ecdefb` erzeugte
wegen des Analysebudgets weder Hostbuild noch Produktport. Es existiert daher
noch keine neuere reale Boot-, Sicht-, Dispatch- oder MHz-Evidenz.

## Review-Ledger dieser Runde

Die folgenden Vertraege wurden bis zum aktuellen dokumentierten
Source-Checkpoint `18f8537` erweitert und teilweise mit fokussierten kleinen
Vertragstests abgedeckt. Das ersetzt weder KR-4984 noch eine
Sonic-Produktabnahme:

- Candidate-Carrier besitzen eine eigene Markierung; ihr Entfernen loescht
  keine reale Jump-Kante mit gleicher Callsite und gleichem Ziel.
- externe bedingte Inventarnachfolger werden als bewachte Regionen verfolgt
  oder als unvollstaendig gemeldet.
- Objektadressprovenienz und die Inventarprovenienz eines tatsaechlich als
  Codepointerargument beobachteten Werts sind getrennt.
- Shapevalidierung unterscheidet ungueltigen, externen und budgetbedingt
  abgeschnittenen Code und besitzt ein globales Arbeitsbudget.
- der schnelle P1/P2-Chain-Treffer revalidiert die zielbezogene
  Codegeneration.
- verschachtelte native Owner-Aufrufe sichern Exitquelle, Endkind,
  Siteklasse und Tailstatus in einem Aufrufframe.
- der PowerShell-Gatewrapper uebernimmt den echten Child-Exitcode; der
  normale Runtimebericht erlaubt Erfolg bei angefordertem Budget nur nach
  vollstaendiger Arbeit und erreichtem Meilenstein.
- Safepoint-Resume-Einstiege und lexikalisch begrenzte
  Registerersetzungen schliessen die konkret reviewten Kontrollfluss- und
  Texttrefferfehler. Strukturierte Operandemission ist damit noch nicht
  erreicht;
- Candidate-Call-Carrier bleiben auf den begrenzten Inventarpfad beschraenkt
  und speisen nicht mehr den semantischen Summary-Fixpunkt.
- wiederholte Beobachtungen derselben Callsite-/Callee-Kombination werden
  vor dem interprozeduralen Worklist-Update konservativ vereinigt.
- ein Shared-Tail mit mehreren Funktionsownern ist ein gueltiger begrenzter
  Inventareinstieg statt automatisch ein Truncationfehler.
- parameterabhaengige Candidate-Returns werden ausserhalb des semantischen
  Fixpunkts in einem separat begrenzten kontextuellen Inventarwalk
  zurueckgefuehrt.
- normale Fallthrough-/Call-Continuation-Wechsel ueber Owner-Domaingrenzen
  werden als eigene bewachte Inventarregionen verfolgt.
- rohe Stored-Kandidaten erhalten ein eigenes Budget; Returned-Table- und
  vollstaendige Stored-Evidenz koennen sich nicht gegenseitig vollstaendig
  aus dem 1.024er-Ergebnisbudget verdraengen.
- Analyseabbruch, Portmetadaten und Latent-AOT-Vollstaendigkeit transportieren
  die typisierten Budgetflags fail-closed.
- der Whole-Export-Cache akzeptiert nur einen exakt manifestierten
  generierten Dateibaum sowie exakt `src/main.cpp` und repariert fehlende,
  veraenderte oder injizierte Artefakte durch einen echten Cachemiss.
- der Gatewrapper wird mit 0/1/3/124, Leerzeichenpfad, isoliertem
  600-Millionen-Budget, vorzeitigem Meilensteinlauf und typisiertem
  Runtimefehler ausgefuehrt.

Noch offen:

- erfolgreicher Export und echter Sonic-Lauf aus dem korrigierten Stand;
- `candidate_inventory_truncated` braucht fuer Produktpostmortems getrennte
  Ursachen fuer Region-, Block-, Forwarding- und Rohkandidatenbudget;
- realer Sonic-Nachweis der terminalen Produktzusammenfassung fuer vor dem
  normalen Bericht geworfene typisierte Runtimefehler;
- strukturierte Registeroperandemission statt nachtraeglicher, wenn auch
  lexikalisch begrenzter C++-Textersetzung;
- gemeinsamer realer Nachweis fuer Lokalisierung, RAM-Batchcommit,
  Invalidierung und zentralen Wiedereintritt;
- 600-Millionen-Gate, 200 MHz, sichtbarer Spielmeilenstein,
  DirectBoot-Paritaet und MSVC-/clang-cl-A/B.

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
wurden. Sie ist kein gueltiger Geschwindigkeitswert. Der aktuelle
`KR-4966`-Quellvertrag verwendet deshalb ein relatives Post-Entry-Budget;
seine frische Produktabnahme ist offen.

Historischer v28-Funktionslauf:

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

## Historischer erster Blocker und aktueller Prüfpunkt

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

Der erste historisch im Produkt beobachtete Blocker war KR-4972:

```text
Dispatchlabel: aot-template-mismatch
Materializer:  AotTemplateMismatch (14)
Callsite:      0x8C11088C
Ziel:          0x8C64784E
Gastzyklus:    553.990.562
```

Das Ziel liegt im initialen Boot-Executable und beginnt mit einem `BRA` zum
gemeinsamen Zielpfad `0x8C6478C2`. Der aktuelle Analyzer- und Exportvertrag
erhaelt diesen Guarded-AOT-Einstieg jetzt bis CFG, IR, Source-Map und
statischer AOT-Ausgabe. Kuenstliche Candidate-Carrier entfernen keine realen
Jump-Kanten mehr, externe bedingte Inventarnachfolger werden verfolgt oder
als unvollstaendig markiert und Objektadressprovenienz wird nicht als
Codepointerprovenienz des geladenen Werts weitergegeben. Der Export verlangt
fuer jeden akzeptierten Guarded-AOT-Einstieg einen emittierten statischen
Block, ein natives Template oder eine explizite Ablehnung.

Die zeitlich letzte reale Produktgrenze liegt historisch im
ABI-73-NativeDisc-v33
bereits bei `0x8C65EA06 -> 0x8C0101F2`, Gesamtzyklus `487.233.787`, mit
`missing-aot / guarded-fallback`. Ob sowohl diese Luecke als auch
`0x8C64784E` im aktuellen Produkt passiert werden, ist offen. Der
abgebrochene v24-Iterationslauf erzeugte kein Portartefakt und keinen
Produktnachweis. Ein Interpreter-, JIT-, Runtime-Decoder- oder
Emulationsfallback bleibt verboten.

## Verbindliche Reihenfolge

```text
Source-Checkpoint 18f8537 [kein P0-Abschluss, kein Produktartefakt]
  +--> KR-4974 reproduzierbare Phasen- und Miss-Telemetrie
  +--> KR-4975 semantische FunctionEvaluation-Keys
  +--> KR-4976 persistente FunctionValue-/SCC-Session
  +--> KR-4977 gemeinsamer Multi-Root-Inventory-Fixpunkt
  +--> KR-4978 inkrementeller CFG-/Seed-/Candidate-Fixpunkt
  +--> KR-4979 priorisierter globaler Executor und Speicherbudget
  +--> KR-4980 schichtweiser persistenter NativeDisc-Buildcache
  +--> KR-4981 8-/12-/24-Thread-Performancegate
  +--> KR-4982 GPU-Entscheidungsgate
  +--> KR-4983 nur bei positivem Gate: capability-gated GPU-Pfad
  +--> KR-4984 unabhaengige Gesamtpruefung und P0/P1-Schliessung
         |
         +--> genau ein ABI-passender NativeDisc-Sonic-Port
                +--> Originaldisc installieren
                +--> realen Lauf und frische Fenster-Screenshots auswerten
                +--> erst danach naechsten realen Blocker bestimmen

DirectBoot und KR-4962:
erst nach NativeDisc-Abnahme mit frisch ABI-passendem Handoff
```

---

## [x] KR-4951 - Produktgate nach Gastzyklen und getrennte visuelle Meilensteine

Prioritaet: P0

Status: Baseline umgesetzt. Der Folgefehler der absoluten statt relativen
Schedulergrenze und der fehlenden Pflichtmeilensteinwertung ist in
`KR-4966` quellseitig geschlossen; die ABI-passende Produktabnahme nach
KR-4974 bis KR-4984 bleibt offen.

### Bereits umgesetzt

- 600-Millionen-Gastzyklus-Gate
- getrennte NativeDisc-/DirectBoot-Meilensteine
- Host-Watchdog nur gegen Hanger
- Bericht fuer Hostzeit, MHz, Dispatches und sichtbaren Meilenstein

### Historisch beobachtete Folgerisiken, quellseitig geschlossen

- der reale v24-Schedulerrestore lief nur bis zum absoluten Maximum
  `600.000.000` statt fuer diese Laufdauer ab Game-Entry
- `visible_screen=none` konnte mit Exitcode 0 und `first_problem=none` enden

---

## [ ] KR-4952 - Post-IP.BIN-Spielhandoff fuer DirectBootExecutable

Prioritaet: P0

Abhaengigkeiten: KR-4967, KR-4968, KR-4969, KR-4970

Status: Quellseitig implementiert, Produktabnahme offen. Reales historisches
`CompletePlatform`-Capture und produktives Apply sind belegt. Der strikt
globale vorbereitete Commitvertrag aus `KR-4967`, die Trennung von
Gastzustand und Hostevidenz sowie das Save-erhaltende Produktprofil aus
`KR-4970` sind im aktuellen Quellpfad vorhanden. Offen bleiben ein
frischer ABI-passender DirectBoot-Handoff und normative
NativeDisc-/DirectBoot-Digests.

### Bereits belegt

- ein privates CompletePlatform-Artefakt wurde aus NativeDisc erfasst
- der DirectBoot-Produktport hat 22 Geraete und 5 typisierte
  Schedulerereignisse daraus angewendet
- der reale Produktlauf erreichte `GameCodeProgressed` ohne neuen
  terminalen Runtimefehler

### Offene Produktabnahme

- den neuen vorbereiteten Commitvertrag spaeter mit frischem ABI-passenden
  Handoff ausfuehren
- Save-Autoritaet und Post-Entry-Evidenzbaseline im Produktbericht belegen
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

Status: Quellseitig implementiert, Produktabnahme offen. Das historische
CompletePlatform-Artefakt mit 22 Geraeten und 5 Schedulerereignissen wurde
erfasst und vom DirectBoot-Produktport verwendet. Der aktuelle Vertrag
bewahrt autoritative VMU-/Flashdaten und trennt Product- von
Diagnostic-Evidenz. Vor dem naechsten DirectBoot-Lauf muss ein neues
ABI-passendes Artefakt erfasst werden; byteidentischer Doppel-Capture und eigene
Inspect-/Verify-Operationen bleiben offen.

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

Status: Teilweise umgesetzt. `GameProjectArtifact` Format 4 fuer
Spielprojektvertrag 5 kann eine
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

Status: Quellseitig implementiert, Produktabnahme offen.
`GameProjectFunctionBoundary::size` wird
als exakte Grenze durch AnalysisOverride/-Seed, Funktionskandidaten, CFG, IR
und AOT transportiert. Analyzer-ABI 23 versioniert den aktuellen Vertrag;
Analyzer-ABI 6 war der damalige Einfuehrungsstand. Der historische
v28-Produktport belegt die exakte externe Grenze fuer das zuvor fehlende
statische Ziel. Ungerade oder ungueltige Groessen, widerspruechliche
Grenzdefinitionen, Ueberlappungen und Delay-Slot-Splits werden fail-closed
abgewiesen. Guarded-AOT-Einstiege, Shared-Tails, Candidate-Carrier und
Codepointerprovenienz besitzen jetzt einen expliziten Exportvertrag mit
Vollstaendigkeitsinvariante. Die breitere Produktabnahme bleibt offen.

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

Status: Quellseitig implementiert, Produktabnahme offen. Der statische
zweistufige Page-/Halfword-Fast-Tier liefert direkt gebundene
Ausfuehrungsdeskriptoren und Fastpaths ohne zweites Resolve. P1/P2-
Inline-Caches liegen vor der erneuten Zieluebersetzung. Nach
Codeinvalidierung wird jeder statische Zieltreffer zusaetzlich gegen seine
eigene Tracker-/Blockgeneration revalidiert. Reale Escape- und
Zentraldispatchzahlen muessen aus dem spaeteren ABI-passenden Produktlauf
folgen.

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

Status: Quellseitig implementiert, Produktabnahme offen. Statische Bloecke
springen direkt in ihren nativen Owner-Einstieg; bekannte direkte Calls und
endliche, live verglichene indirekte Ziele koennen unter Timing-, Tiefen-,
Code- und Architekturguards direkt ausgefuehrt werden. Unbekannte Ziele
bleiben an der allgemeinen validierenden Dispatchgrenze.

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

Status: Teilweise quellseitig implementiert, Produktabnahme offen. IR-Use/Def
und Liveness waehlen GPR sowie T, PR, GBR, MACH, MACL und FPUL aus und geben
lokalen Zustand an Architekturgrenzen ab. Die eigentliche
Registerausdruckwahl erfolgt weiterhin nach der C++-Emission; `e6d609d`
begrenzt diese Ersetzung lexikalisch auf C++-Code und verhindert die konkret
reviewten Kommentar-, String- und Praefixtreffer, ersetzt aber noch keine
strukturierte Operandemission. Bewiesene P1/P2-Haupt-RAM-Stores koennen in
einem festen `DirectLinearWriteBatch` gesammelt werden; Flush, Guardmiss und
SMC-/Modul-/Blockinvalidierung sind quellseitig verdrahtet. Die gemeinsame
Produktkette ist noch nicht real abgenommen.

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

Status: Quellseitig implementiert, Produktabnahme offen. Chain- und
Safepointguards verwenden ereignisgetriebene IRQ-/Architekturepochen;
Gastzyklen und lokalisierter Zustand werden an MMIO-, Exception-, Call-,
SR-/Bankwechsel-, Scheduler- und Quantumgrenzen committed. Die reale
IRQ-/Schedulerarbeitsreduktion und Soundsemantik muessen im spaeteren
ABI-passenden Port gemessen werden.

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

Status: Handoff-, Save- und Post-Entry-Vertraege sind quellseitig vorhanden,
Produktabnahme offen. Der alte gemeinsame End-PC `0x8C666D42` ist durch
v26/v28 ueberholt. Fuer die danach beobachteten AOT-Luecken existiert ein
allgemeiner Guarded-Entry-/Exportvertrag. Der aktuelle dokumentierte
Source-Checkpoint ist `18f8537`; der danach abgebrochene v24-Iterationslauf
lieferte kein Portartefakt. Zuerst sind KR-4974 bis KR-4984 abzuschliessen,
danach folgt genau ein ABI-passender NativeDisc-Lauf ohne Handoff.
DirectBoot-Paritaet, normativer Digestvergleich und sichtbarer Spielframe
folgen spaeter mit neu erfasstem ABI-passenden CompletePlatform-Handoff.

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
Der erste frische Direct-v24-Export/Build dauerte etwa `169,3 s`. Der
ABI-73-v33-Kaltexport dauerte dagegen `711,2 s`; sein unveraenderter
Ninja-Hostbuild dauerte `0,200236 s`, waehrend ein identischer Voll-Warmexport
den Whole-Export-Cache verfehlte und nach `124 s` abgebrochen wurde.
Der historische Stand `cb5fb47` lud NativeDisc fuer Validierung und Export
nur einmal und band den Whole-Export-Cache an ein exaktes Manifest und
Dateiset. `18f8537` enthaelt weitere Cache- und Parallelitaetsarbeit, doch der
nachfolgende v24-Iterationslauf wurde nach rund 3 h 27 min ohne Portartefakt
abgebrochen. Weder der aktuelle Kaltbuildvertrag noch Runtime-only-,
Hook-only- oder MSVC-/clang-cl-Produktvergleiche sind abgenommen.

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

Status: Der Guarded-AOT-Entry- und Exportvollstaendigkeitsvertrag ist
quellseitig vorhanden, die aktuelle Gesamtanalyse aber noch nicht
exportfaehig. Die generische Analyse gewinnt Ziel und Shared-Tail aus
Candidate-Tail-Jumps und einem bewiesenen Runtime-Stackframe, ohne eine feste
vollstaendige Dispatchkante zu erfinden. Auf `7ecdefb` trat ein
Candidate-Call-Carrier jedoch in den semantischen Summary-Fixpunkt ein; der
Sonic-Export erschoepfte nach 65.536 Funktionsevaluationen sein Budget.
Zugleich markierte der aggregierte Inventarwalker den Lauf als trunciert.
Die damaligen allgemeinen Korrekturen wurden auf `cb5fb47` eingecheckt und
bis `18f8537` weiterentwickelt. Der v30-Befund unten und v33 bleiben
historische Produktevidenz; ein aktuelles Portartefakt existiert nicht.

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
- [x] den generisch gewonnenen Seed durch die externe
  Spielprojekt-/Exportkonfiguration bis in CFG, IR und AOT erhalten
- [x] kuenstliche Candidate-Carrier anhand ihrer vollen Identitaet entfernen,
  ohne reale Jump-Kanten mit gleicher Callsite und gleichem Ziel zu loeschen
- [x] externe bedingte Inventarnachfolger weiterverfolgen oder den Walk
  ausdruecklich als unvollstaendig markieren
- [x] Objektadress- und Codepointerwertprovenienz getrennt halten
- [x] Exportvollstaendigkeit fuer jeden akzeptierten Guarded-AOT-Einstieg
  erzwingen
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

Der historische v30-Lauf erfuellt die erneute Discinstallation und
Sichtpruefung, aber nicht die erste Abnahmebedingung. Der aktuelle
Source-Checkpoint ist durch den abgebrochenen v24-Iterationslauf nicht
abgenommen; KR-4972 bleibt bis zu einem erfolgreichen Export und realen,
nach KR-4974 bis KR-4984 zulaessigen Produktlauf offen.

---

## [x] KR-4973 - NativeDisc-Sichtregression und proof-unabhaengige PVR-Ausgabe

Prioritaet: P0

Abhaengigkeiten: KR-4969, KR-4972

Status: Abgeschlossen durch den realen v32-NativeDisc-Produktlauf.

### Ursache und allgemeine Reparatur

- Der exakte saubere Kontrollstand `906f185` reproduzierte den sichtbaren
  Sega-Screen erneut. Die erste schwarze Folge trat nach dem mit `f550747`
  freigeschalteten Flag-Poll-Batching unter aktiver MMU auf.
- `try_composite_callback_flag_poll_batch` akzeptiert deshalb wieder nur
  `AddressTranslationMode::NoMmu`. AT=1 faellt vor jeder Zustandsmutation auf
  die echten statischen AOT-Bloecke zurueck; CountedLoop- und MMIO-Tiers
  bleiben unveraendert.
- Ein gueltiger VBlank-Scanout besitzt nun eine eigene bounded latest-wins
  Hostqueue. Ein bereits konsumierter oder noch wartender
  `PvrGuestFrameProof` kann reale VRAM-/Registerpixel nicht mehr von der
  Presentation abhalten. Proofmarker und Erfolgsmetriken bleiben weiterhin
  streng beweisgebunden.
- `port <gdi> --game-project <artifact>` bindet jetzt dieselben
  hashgebundenen externen Metadaten wie `port-executable`. Erst dadurch ist
  NativeDisc gegen DirectBoot an derselben AOT-Grenze vergleichbar.

### Reale Produktabnahme

```text
Produkt:                         Sonic Adventure PAL NativeDisc-v32
Runtime-ABI:                     64
Discinstallation:                3 Tracks / 521.461 Sektoren
Retailsektoren im Paket/Repo:    0 / 0
Gastzyklus am Fehler:            553.990.562
externe Produktzeit:             6,701 s
provisorische Rate bis Fehler:   82,67 MHz
zentrale Dispatches:             11.080.283
Hostframes:                      127
PVR Gast-/Direct-Frames:         2 / 2
PVR Softwareframes:              1
hoechster sichtbarer Screen:     Sega-Lizenzscreen ab 2,032 s
Callsite / Ziel:                 0x8C11088C / 0x8C64784E
DirectBoot-v30-Vergleich:        gleicher Zyklus und gleiches Ziel, 0 Hostframes
MSVC-Export:                     2.051 Funktionen / 46 Partitionen
Produkt-EXE:                     53.677.056 Bytes
Produkt-EXE SHA-256:             2ebc3a2c451aa307b20e2c1242c353a58eea652498681eaa55bba852f54affab
unveraenderter Ninja-Warmbuild:  0,203137 s
```

Die reale 640x480-Aufnahme ab `2,032` Sekunden zeigt den Sega-Screen.
`sega_seen=false` im privaten Wrapper
ist ein Klassifikator-False-Negative: Der PAL-Screen verwendet einen grauen
statt weissen Hintergrund, waehrend `blue_ratio=0,0609375` und
`non_black_ratio=0,9830208` stabil sind.

Der Lauf endet vor 600 Millionen Gastzyklen am bereits bekannten KR-4972-
Coveragefehler und ist deshalb kein Performancegate. Er belegt aber den
geforderten sichtbaren Bootfortschritt und den identischen funktionalen
Grenzpunkt beider Bootpfade. Diese Werte sind historische ABI-64-Evidenz,
nicht der aktuelle Source-Checkpoint. Der unbrauchbare v31-Zwischenport, v28/v29 und
ihre Workspaces sowie die 906-/Baseline-Scratchkopien wurden nach
Pfadpruefung entfernt; insgesamt waren `14.912.142.577` Bytes regenerierbar.

---

## [ ] KR-4966 - Post-Entry-Produktgate und erforderliche Meilensteine

Prioritaet: P0

Abhaengigkeiten: KR-4951

Status: Quellseitig implementiert, Produktabnahme offen. Der konkrete
v24-Produktlauf belegt den historischen absoluten Budgetfehler. Der aktuelle
Vertrag berechnet das Ziel relativ ab Game Entry, berichtet die ausgefuehrte
Post-Entry-Arbeit getrennt und erlaubt bei angefordertem Budget Exitcode 0
nur nach vollstaendigem Budget und erreichtem Pflichtmeilenstein. Der
ABI-73-v33-Wrapper verlor auf dem typisierten Fehler noch den Child-Exitcode;
`199328b` ersetzt diesen Prozessstartvertrag. Eine kleine ausfuehrende
Erweiterung des vorhandenen CLI-Porttests und der reale ABI-passende
Gatenachweis nach KR-4974 bis KR-4984 stehen noch aus.

### Produktbefund

```text
restaurierter Game-Entry-Zyklus: 415.233.270
absolutes Stopmaximum:           600.000.000
ausgefuehrte Post-Entry-Zyklen:  184.766.730
Hostzeit:                        5,01505 s
gueltige effektive Gast-MHz:     36,8425
ungueltig berichteter Wert:      119,64 MHz
```

Die historischen 119,64 MHz verwenden den restaurierten absoluten
Zaehlerstand als ausgefuehrte Arbeit. NativeDisc und DirectBoot hatten damit
noch keine gleiche Post-Entry-Arbeit erhalten. Der Quellfehler ist behoben;
der spaetere ABI-passende Produktlauf muss den neuen Bericht und Exitvertrag
abnehmen.

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

Status: Quellseitig implementiert, Produktabnahme offen. Vollstaendige
Vorvalidierung, vorbereitete Speicher-/Geraete-/Scheduler-/IRQ-/CPU-
Commitplaene, CPU-PC/PR als letzte Veroeffentlichung und semantischer
Recapture bilden den atomaren Vertrag. Ein reales historisches
CompletePlatform-Apply ist belegt; ein frischer ABI-passender Handoff und
normative Digests pro Subsystem bleiben als Produktabnahme offen.

### Umgesetzter Quellstand

- alle 22 Geraetepayloads und 5 typisierten Schedulerereignisse werden vor
  dem Apply validiert
- Geraete besitzen passive Restore-/Game-Entry-Adapter
- der angewendete Zustand kann semantisch erneut erfasst werden
- alle falliblen Vorbereitungen erfolgen vor Commitbeginn
- Speicher, Geraete und Scheduler werden aus vorbereiteten Plaenen committed;
  CPU-PC und PR zuletzt
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

Status: Quellseitig implementiert, Produktabnahme offen. Die erforderlichen
Game-Entry-Adapter, Event-/IRQ-Vertraege und vorbereiteten Restoreplaene sind
implementiert. v26 und v28 beenden historisch die nach dem Entry armierte
AICA-G2-DMA mit `active=0`, `remaining=0`. Exakte ABI-passende
NativeDisc-/DirectBoot-Paritaet und der Abgleich eines bereits restaurierten
aktiven Hardware-Request-Kanals ohne Completionevent bleiben als
Produktabnahme offen.

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

Status: Quellseitig implementiert, Produktabnahme offen. Die
PVR-/SPG-/ASIC-Game-Entry-Adapter und vorbereiteten Restoreplaene sind
implementiert. Product-Handoff uebernimmt nur gastseitig sichtbaren Zustand
und setzt Frameproofqueue, Renderer-/Pixelmetriken, Direct-VRAM-Schatten und
Hostfehler am Game Entry auf eine neue Baseline. Die zwei historischen
v28-Gast-/Direct-Frames mit `302.287` Pixeln sind deshalb kein Nachweis fuer
neue DirectBoot-Spielbilder. Sichtbare ABI-passende Hostpraesentation und normative
NativeDisc-/DirectBoot-Paritaet bleiben offen.

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

Status: Quellseitig implementiert, Produktabnahme offen. Game-Entry-Adapter
und historischer Produktlauf sind belegt. Das aktuelle Product-Handoff-Profil
uebernimmt Maple-Topologie, MMIO-/DMA-/Controller- und Eventzustand, laesst
aber installierte VMU- und Flash-Working-Copies autoritativ. Diagnosehistorie
und Hostmetriken werden nicht als gastseitiger Zustand restauriert. Die
Save-Rollback-Sperre und Eventparitaet muessen noch im frischen ABI-passenden
Produktpfad abgenommen werden.

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

## P0 NativeDisc-Kaltbuild-Architektur

Der vollstaendige Befund, die Messbasis, gemeinsamen Korrektheitsinvarianten,
GPU-Schwellen, RAM-/Zeitgates sowie Migrations- und Rollbackregeln stehen in
[`P0_NATIVE_DISC_COLD_BUILD_PERFORMANCE.md`](P0_NATIVE_DISC_COLD_BUILD_PERFORMANCE.md).
Die folgenden Tasks werden nicht durch einen weiteren langen Sonic-Export
unterbrochen. Nach dem Performancegate folgt KR-4984; erst danach wird genau
ein neuer realer NativeDisc-Port gebaut.

## [x] KR-4974 - Reproduzierbare Kaltbuild-Telemetrie und Miss-Reason-Ledger

Prioritaet: P0

Abhaengigkeiten: keine

Status: Abgeschlossen. Die Telemetrie ist durch die realen Analyse-, Cache-,
IR-, Partitions-, Hostconfigure- und Hostbuildpfade verdrahtet. Der
produktweite Nachweis bleibt gemaess `AGENTS.md` der spaetere reale Kaltport;
KR-4981 wertet dessen harte Zeit-/Ressourcengates aus.

### Abschlussstand

Implementiert:

- versionierte opt-in JSONL-Telemetrie mit Manifest-, Progress-, Resource-
  und Terminalrecords;
- begrenzte asynchrone Aufzeichnung, explizite Verlust-/Completenessfelder,
  geordneter Flush und atomare terminale Veroeffentlichung;
- Input-/Output-/Workspace-/Publishlock-/GDI-Track-Aliasschutz sowie
  Ablehnung reservierter Windows-Geraetenamen;
- Windows-Job- und POSIX-Prozessgruppenressourcen; finale POSIX-`wait4`-
  Werte erhalten kurzlebige Kindprozessarbeit fuer CPU, Faults und Peak-RSS;
- exakte Cachelookups, Ready-Hits, In-Flight-Coalesces, Misses, Evictions,
  Eintraege, Bytes und genau ein primaerer Grund pro Miss;
- Head-of-Line-, dynamische Seed-/Workset- und
  geplant/gestartet/ready/committed-Indikatoren;
- eine deterministische retailfreie NativeDisc-Stressfixture mit `smoke`-
  und `reference`-Profil.

Abgenommen wurden der kombinierte Build aller betroffenen Targets, die engen
Progress-/Hostprozessvertraege sowie der reale Komponentenpfad der
retailfreien Fixture einschliesslich beider Codegen-Worker-Praezedenzfaelle.
Der dabei sichtbar gewordene Windows-Stall nach bereits abgeschlossenem CMake-
Configure wurde im Produktpfad geschlossen: jeder beaufsichtigte Hostbefehl
besitzt einen privaten MSPDB-Endpunkt mit einsekuendiger natuerlicher
Shutdownfrist; die Job-Leere bleibt weiterhin der Quieszenzbeweis. Eine
erneute breite Matrix wurde gemaess der verbindlichen Produkt-vor-Test-Regel
nicht gestartet. Die 8-/12-/24-Thread-Gates gehoeren unveraendert zu KR-4981.

### Umfang

- Phasen-/Subphasentimings, wachsendes Workset und echte CPU-/RAM-Metriken
- geplant, queued, aktiv, ready und kanonisch committed getrennt
- Cachehits, Evictions, In-Flight-Coalesces und vollstaendiges
  Evaluation-Miss-Reason-Ledger
- Referenzhost-/Toolchain-/Buildprofilmanifest und Prozessbaumressourcen
- stabile JSONL-Ausgabe und retailfreie Stressform

### Akzeptanz

- keine mehr als zehn Sekunden lange Phase ohne Heartbeat
- `lookups = ready_hits + in_flight_coalesces + misses`; primaere
  Missgruende summieren sich exakt zu `misses`
- Head-of-Line-Stall und dynamischer Seedzuwachs sind messbar

---

## [ ] KR-4975 - Semantische FunctionEvaluation-Key-Projektion und Cachelinsen

Prioritaet: P0

Abhaengigkeiten: KR-4974

Status: Geplant.

### Umfang

- versionierte Linsen fuer Summary, Candidate Contract, Guarded Inventory,
  Contextual Return und isolierte Observation
- ABI-Read-Set-basierte Projektion von Register-, Stack- und Memoryfacts
- komponentisierte Keydigests und internierte Candidate-/Evidence-Sets

### Akzeptanz

- irrelevante Eingangsaenderung erzeugt keinen Miss
- relevante Eingangsaenderung kann keinen falschen Hit erzeugen
- unvollstaendige Read-/Stack-/Sink-Evidenz verwendet Vollzustand oder
  deaktiviert den Cache
- projizierte und konservative Vollzustandsauswertung sind kanonisch gleich

---

## [ ] KR-4976 - Persistente FunctionValue-Programm-/SCC-Session

Prioritaet: P0

Abhaengigkeiten: KR-4974, KR-4975

Status: Geplant.

### Umfang

- immutable FunctionProgramGraph-Arena mit stabilen Fingerprints
- persistenter SCC-DAG, Callergraph und ABI-Vertraege
- versionierte Dependency-/Summaryzustande und atomare Analysis-Epoch
- bestehende direkte Callee-Stackprojektion erhalten; Register-/Memoryfacts
  nur bei vollstaendiger Read-Evidenz weiter verengen

### Akzeptanz

- unveraenderte Funktionen bauen Graph- und ABI-Daten nicht erneut
- lokale Aenderung invalidiert nur den nachweisbaren Dependency-Closure
- unvollstaendige Register-/Memory-Read-Evidenz bleibt konservativ
- fehlerhafte oder abgebrochene Epoch wird nicht teilweise publiziert

---

## [ ] KR-4977 - Gemeinsamer Multi-Root-Guarded-Inventory-Fixpunkt

Prioritaet: P0

Abhaengigkeiten: KR-4975, KR-4976

Status: Geplant.

### Umfang

- globaler ContextKey und internierte Rootprovenienz
- gemeinsame forwarded/contextual Worklist
- explizite Isolation-/Korrelationspartitionen
- teilbare Fortsetzungen fuer lange transitive Contextketten

### Akzeptanz

- identischer Context wird pro Dependency-Version einmal ausgewertet
- keine Root-, Callsite- oder Ownerkorrelation wird erfunden
- physische Deduplication aendert keine logische per-Root-/per-Isolations-
  Budget-, FIFO- oder Truncationdiagnostik
- bestehende Forwarding-/Contextbudgets bleiben seriell/parallel identisch
  und fail-closed

---

## [ ] KR-4978 - Inkrementeller CFG-/Seed-/Candidate-Contract-Fixpunkt

Prioritaet: P0

Abhaengigkeiten: KR-4976, KR-4977

Status: Geplant.

### Umfang

- monotone Seedfakten mit Ursache
- Dirty-SCC-/Caller-/Inventory-Sink-Invalidierung
- inkrementelle ProgramGraph-Erweiterung und stabile Finalmaterialisierung

### Akzeptanz

- spaete Seeds starten keine unbetroffene Vollanalyse neu
- Funktionen, Bloecke, Resolutionen und AOT-Inventar bleiben exakt
- nicht darstellbarer Zustand faellt konservativ auf CPU-Neuberechnung
  zurueck

---

## [ ] KR-4979 - Priorisierter Analyseexecutor und begrenzter Speicherhaushalt

Prioritaet: P0

Abhaengigkeiten: KR-4974, KR-4977, KR-4978

Status: Geplant.

### Umfang

- typisierte, kosten- und critical-path-priorisierte Workitems
- teilbare SCC-/Context-/Root-Continuations im gemeinsamen Workerpool
- globales RAM-/Cache-/Contextbudget mit sicherer Eviction und Spill

### Akzeptanz

- schwere Fenster nutzen mindestens 75 Prozent der konfigurierten Kerne
- 16-GiB-Hosts geraten nicht in Paging-Sturm oder OOM
- RAM-Druck erzeugt kein semantisches Truncation

---

## [ ] KR-4980 - Schichtweiser persistenter NativeDisc-Buildcache

Prioritaet: P0

Abhaengigkeiten: KR-4975, KR-4976, KR-4978

Status: Geplant.

### Umfang

- atomare ProgramGraph-, ABI-, SCC-, Inventory- und IR-Shards
- komponentenbezogene Implementierungsidentitaeten
- positive/negative Ergebnisse, Korruptionsdiagnose, LRU und Groessenlimit
- vorhandene Whole-Export-, Latent-AOT-, Codegen- und Hostcaches integrieren

### Akzeptanz

- fehlender, alter oder beschaedigter Shard ist ein sicherer Miss
- lokale Aenderung invalidiert nur semantisch gebundene Ebenen
- exakter Warmexport bleibt unter 30 Sekunden

---

## [ ] KR-4981 - 8-/12-/24-Thread-Kaltbuild-Performancegate

Prioritaet: P0 Performance-Gate

Abhaengigkeiten: KR-4974 bis KR-4980, KR-4982 und bei positivem GPU-Gate
KR-4983

Status: Geplant als Gate-Vorbereitung.

### Akzeptanz

- nur retailfreie oeffentliche/synthetische Last; noch kein privater
  Sonic-Port vor KR-4984
- festes Referenzhostmanifest mit CPU/SMT/RAM/SSD/OS/Compiler/Buildprofil
  und exakt dokumentiertem Kaltzustand
- voller kalter Port auf 24 Threads hoechstens 8 Minuten und 12 GiB
  Process-Tree-Private/Commit-Peak
- voller kalter Port auf 12 Threads hoechstens 11 Minuten und 10 GiB
  Process-Tree-Private/Commit-Peak
- voller kalter Port auf 8 Threads hoechstens 15 Minuten und 8 GiB
  Process-Tree-Private/Commit-Peak
- Analyse plus Codegen hoechstens 6/9/12 Minuten
- drei oeffentliche Wiederholungen je Threadklasse, Median und Maximum
- kein Einzelprozess und keine Phase laenger als 15 Minuten
- rund 84.000 logische Evaluationrequests/-kontexte; physische
  Evaluationen duerfen durch sichere Deduplication sinken
- keine reduzierte Funktions-, Block-, Resolution- oder AOT-Abdeckung

---

## [ ] KR-4982 - GPU-Offload-Entscheidungsgate und repraesentativer Prototyp

Prioritaet: P0 Entscheidungsgate

Abhaengigkeiten: KR-4974, KR-4975, KR-4977, KR-4979

Status: Profil-/Reject-Inventar darf nach KR-4974 beginnen; finaler
Prototypvergleich und Gateabschluss warten auf den optimierten CPU-Pfad.

### Umfang

- nur profilbelegte homogene Batchkerne untersuchen
- optimierte CPU-SIMD-/Threadpool-Referenz und begrenzter
  D3D11-Compute-Prototyp
- Setup, Shadercompile, H2D, Kernel, D2H, RAM und VRAM messen
- gebundene Waits, Device-Lost, Timeout sowie nachweisbare Struktur-/
  Digestfehler und CPU-Fallback pruefen
- per-Device-/Batch-Crossover; unbekannte oder langsamere Devices bleiben
  auf CPU

### Akzeptanz

- mindestens zweifacher Phasendurchsatz inklusive Transfers
- mindestens 15 Prozent medianer End-to-End-Kaltportgewinn auf zwei
  repraesentativen diskreten GPUs
- keine iGPU-, CPU-only- oder Unsupported-Regression
- hoechstens 1 GiB zusaetzlicher Analyzer-VRAM
- unterhalb der Schwelle wird der Prototyp verworfen und das negative
  Entscheidungsergebnis dokumentiert

---

## [ ] KR-4983 - Deterministische capability-gated GPU-Beschleunigung

Prioritaet: bedingtes P0

Abhaengigkeiten: positives KR-4982-Gate

Status: Nur bei positivem Gate geplant.

### Umfang

- separates Analyse-Compute-Backend, keine Runtime-Presenter-Kopplung
- Capability-, Treiber- und Speicherpruefung
- per-Device-Crossover und gebundener GPU-Wait
- atomare Batchuebernahme und vollstaendige CPU-Neuberechnung bei erkannten
  API-/Timeout-/Device-Lost-/Struktur-/Digestfehlern
- GPU-an/aus-Telemetrie und Abschaltoption

### Akzeptanz

- CPU und GPU liefern bytegleiche kanonische Artefakte
- Device-Lost, Timeout oder erkannter Struktur-/Digestfehler kann nie
  teilweise publiziert werden
- KR-4982-Schwellen bleiben im integrierten End-to-End-Pfad erfuellt

---

## [ ] KR-4984 - Unabhaengige Gesamtpruefung und P0/P1-Schliessung vor NativeDisc-Produktlauf

Prioritaet: P0, letzter Gate-Vorbereitungstask

Abhaengigkeiten: KR-4981, KR-4982 und gegebenenfalls KR-4983

Status: Geplant. Der naechste reale NativeDisc-Lauf ist bis zum Abschluss
gesperrt.

### Umfang

- unabhaengiger End-to-End-Review von CFG/Seeds, Function Value, SCC,
  Candidate Contract, Guarded Inventory und Rootkorrelation
- Review von Cachelinsen, Shards, Executor, Fortschritt, RAM, IR, Codegen,
  Hostbuild und Packaging
- Review exakter Latent-AOT-Hints, bytegleicher Multi-Extent-Bindings und
  sichtbarer-Frame-Baseline/-Pixelverteilung
- Review des optionalen Analyse-GPU-Pfads und seines CPU-Fallbacks
- Review der beruehrten Runtime-CPU-/Parallelwork- und
  CPU-/D3D11-Ausgabepfade
- Runtime-Vorher/Nachher bei gleicher Gastarbeit: Gesamt-CPU-Zeit,
  CPU/Gastzyklus, effektive Kerne, Threadhotspots, Busy-Wait/Framepacing,
  Hostframes und Speicher
- Presenterbackend/Fallback, Uploadbytes/-anzahl, Upload-/Present-/Blockzeit
  sowie verfuegbare GPU-Zeit-/Auslastungs-/VRAM-Metriken
- pfadbezogenes Finding-Ledger und erneute Pruefung jeder Reparatur

### Akzeptanz

- alle P0/P1-Funde geschlossen und nachreviewed
- betroffene Korrektheits- und Performancegates nach jeder Reparatur erneut
  gruen
- keine unverdrahtete Option, Identitaet, Telemetrie oder Fallbackgrenze
- kein ungebundener Runtime-Busy-Spin; bestaetigte Runtime-Hotspotarbeit pro
  gleicher Gastarbeit mindestens 20 Prozent unter Vorher-Baseline
- Runtime-Multicore/GPU nur bei gemessenem Hotspot und end-to-end
  CPU-/Walltimegewinn, nicht fuer eine schoene Auslastungszahl
- erst dann genau ein frischer NativeDisc-Port, Installation und echter
  Screenshot-Nachweis ueber die bekannte Sega-/Schwarzgrenze
- der private 24-Thread-Kaltport bestaetigt das 8-Minuten-Gesamtziel; eine
  Verfehlung laesst den P0 offen und erzwingt vor einem weiteren Versuch den
  betroffenen Implementierungs-/Reviewzyklus
- Gesamtbuildzeit und alle Phasen-/CPU-/RAM-/GPU-Messwerte berichtet

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

### Lauf A4 - naechster P0-Produktlauf [hinter KR-4974 bis KR-4984 gegatet]

- KR-4974 bis KR-4983 implementieren, jeweils pruefen, committen und pushen
- in KR-4984 alle betroffenen Analyse-, Inventar-, Cache-, Executor-,
  Fortschritts-, Hostbuild- und Runtimepfade unabhaengig pruefen und alle
  P0/P1-Befunde schliessen
- genau einen frischen ABI-passenden NativeDisc-Port aus diesem Stand
  erzeugen
- Originaldisc real installieren
- exakt 600 Millionen Post-Entry-Zyklen mit relativem Gate ausfuehren
- separaten sichtbaren Fensterlauf aufnehmen
- sowohl den v32-Blocker `0x8C11088C -> 0x8C64784E` als auch die
  v33-Grenze `0x8C65EA06 -> 0x8C0101F2` ueber validiertes statisches AOT
  passieren oder einen engeren typisierten Blocker belegen
- Zentraldispatches, Hostzeit, Gast-MHz, PVR/Hostframes und hoechsten
  sichtbaren Screen gegen v32 dokumentieren

### Lauf B - DirectBoot nach NativeDisc-Abnahme

- frischen ABI-passenden CompletePlatform-Zustand am Game Entry erfassen
- DirectBoot Apply bis vor ersten Spielblock; kein Sega-Screen erwartet
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
