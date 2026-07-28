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

## Getrennte Evidenzstaende

Diese drei Staende duerfen nicht als derselbe Fortschritt berichtet werden:

1. **Letzte reale Produktevidenz:** Der ABI-73-NativeDisc-v33-Port wurde aus
   `1629268` erzeugt, installiert und ausgefuehrt. Er zeigte einen
   IP.BIN-Frame und stoppte nach `72.000.517` Post-Entry-Zyklen bei
   `0x8C65EA06 -> 0x8C0101F2` mit `missing-aot`. Das war weder ein
   600-Millionen-Gate noch eine gueltige Performanceabnahme.
2. **Aktueller eingecheckter Source-Head:** `cb5fb47` auf `main` verwendet
   Runtime-ABI 74, Block-ABI 5, Analyzer-ABI 8,
   PlatformServices-ABI 13, Backend-Interface-ABI 12,
   Portprojektvertrag 64, Native-AOT-Profil 13 und Partitionsschema 5.
   Gegen diesen Stand begann ein neuer kalter Sonic-NativeDisc-Export, endete
   aber nach `381,413 s` noch vor Hostcompiler und Portpaket fail-closed mit
   `function_budget_exhausted=1` und
   `candidate_inventory_truncated=1`.
3. **Lokaler Arbeitsstand:** Nur diese Dokumentationssynchronisierung.
   Candidate-/Summarytrennung, kontextueller Candidate-Return-Walk,
   Owner-Domain-Ingress, Raw-Stored-Budget, Vollstaendigkeitsmetadaten,
   Gateausfuehrung und exakter Whole-Export-Dateibaum sind auf `cb5fb47`
   eingecheckt. Dieser Stand ist noch nicht durch einen Sonic-Export oder
   Produktlauf abgenommen.

Die reviewten P0-Source-Vertraege sind damit eingecheckt. Vor dem
naechsten Sonic-Lauf wird der manifestgebundene Cachevertrag noch aus dem
sauber neu konfigurierten Source-Head ausgefuehrt. Danach folgen genau ein frischer
NativeDisc-Port, reale Discinstallation, 600 Millionen Post-Entry-Gastzyklen
und ein separater Sichtnachweis. DirectBoot wird spaeter mit einem frisch an
den dann aktuellen Runtime-ABI gebundenen CompletePlatform-Handoff geprueft;
NativeDisc benoetigt keinen Handoff.

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

Bis `cb5fb47` eingecheckt und mit vorhandenen kleinen Vertragstests
abgedeckt, aber noch nicht als Sonic-Produkt abgenommen:

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

Die zeitlich letzte reale Produktgrenze liegt im ABI-73-NativeDisc-v33
bereits bei `0x8C65EA06 -> 0x8C0101F2`, Gesamtzyklus `487.233.787`, mit
`missing-aot / guarded-fallback`. Ob sowohl diese Luecke als auch
`0x8C64784E` im aktuellen Produkt passiert werden, ist bis zum
ABI-74-Sonic-Lauf offen. Ein Interpreter-, JIT-, Runtime-Decoder- oder
Emulationsfallback bleibt verboten.

## Verbindliche Reihenfolge

```text
P0-Quellumbau [cb5fb47 eingecheckt; Sonic-Abnahme offen]
  +--> KR-4972 Guarded-AOT-/Exportvollstaendigkeit
  +--> KR-4966 relatives Produktgate
  +--> KR-4967 bis KR-4970 atomarer, evidenz- und Save-sicherer Handoff
  +--> KR-4956 Static-AOT-Fast-Tier und Revalidierung
  +--> KR-4957 direkte Owner-/Call-/endliche Indirect-Einstiege
  +--> KR-4958 IR-Lokalisierung und RAM-Batching/SMC
  +--> KR-4959 Architektur-/Scheduler-/IRQ-Safepoints
         |
         +--> korrigierten ABI-74-Source-Stand [cb5fb47]
                +--> frischer ABI-74-NativeDisc-Sonic-Lauf
                +--> naechsten realen Blocker bestimmen
                +--> KR-4960 200-MHz-Produktgate weiterfuehren
                +--> DirectBoot spaeter mit frischem ABI-74-Handoff

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

Status: Baseline umgesetzt. Der Folgefehler der absoluten statt relativen
Schedulergrenze und der fehlenden Pflichtmeilensteinwertung ist in
`KR-4966` quellseitig geschlossen; die ABI-74-Produktabnahme bleibt offen.

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
`KR-4970` sind im aktuellen ABI-74-Quellpfad vorhanden. Offen bleiben ein
frischer ABI-74-DirectBoot-Handoff und normative
NativeDisc-/DirectBoot-Digests.

### Bereits belegt

- ein privates CompletePlatform-Artefakt wurde aus NativeDisc erfasst
- der DirectBoot-Produktport hat 22 Geraete und 5 typisierte
  Schedulerereignisse daraus angewendet
- der reale Produktlauf erreichte `GameCodeProgressed` ohne neuen
  terminalen Runtimefehler

### Offene Produktabnahme

- den neuen vorbereiteten Commitvertrag mit frischem ABI-74-Handoff ausfuehren
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
ABI-74-Artefakt erfasst werden; byteidentischer Doppel-Capture und eigene
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

Status: Quellseitig implementiert, Produktabnahme offen.
`GameProjectFunctionBoundary::size` wird
als exakte Grenze durch AnalysisOverride/-Seed, Funktionskandidaten, CFG, IR
und AOT transportiert. Analyzer-ABI 6 versioniert den aktuellen Vertrag. Der
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
Zentraldispatchzahlen muessen aus dem ABI-74-Produktlauf folgen.

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
IRQ-/Schedulerarbeitsreduktion und Soundsemantik muessen im ABI-74-Port
gemessen werden.

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
allgemeiner Guarded-Entry-/Exportvertrag; die Summary-/Inventarkorrektur ist
auf `cb5fb47` eingecheckt. Zuerst folgt deshalb der frische
ABI-74-NativeDisc-Lauf ohne Handoff.
DirectBoot-Paritaet, normativer Digestvergleich und sichtbarer Spielframe
folgen spaeter mit neu erfasstem ABI-74-CompletePlatform-Handoff.

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
`cb5fb47` laedt NativeDisc fuer Validierung und Export nur einmal und bindet
den Whole-Export-Cache an ein exaktes Manifest und Dateiset. Weder dieser Cachevertrag
noch ein neuer Kaltexport, Runtime-only-, Hook-only- oder
MSVC-/clang-cl-Produktvergleich sind abgenommen.

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
Die allgemeinen Korrekturen sind auf `cb5fb47` eingecheckt. Der v30-Befund
unten und v33 bleiben historische Produktevidenz; ein neuer
ABI-74-Port existiert noch nicht.

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
eingecheckte Source-Head ist wegen des neuen Analysebudgetfehlers nicht
abnahmefaehig; KR-4972 bleibt bis zu einem erfolgreichen Export und realen
ABI-74-Produktlauf offen.

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
nicht der aktuelle ABI-74-Quellstand. Der unbrauchbare v31-Zwischenport, v28/v29 und
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
Erweiterung des vorhandenen CLI-Porttests und der reale ABI-74-Gatenachweis
stehen noch aus.

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
der ABI-74-Produktlauf muss den neuen Bericht und Exitvertrag abnehmen.

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
CompletePlatform-Apply ist belegt; ein frischer ABI-74-Handoff und normative
Digests pro Subsystem bleiben als Produktabnahme offen.

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
AICA-G2-DMA mit `active=0`, `remaining=0`. Exakte ABI-74-
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
neue DirectBoot-Spielbilder. Sichtbare ABI-74-Hostpraesentation und normative
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
Save-Rollback-Sperre und Eventparitaet muessen noch im frischen ABI-74-
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

### Lauf A4 - aktueller P0-Produktlauf [ausstehend]

- offene Analyse-, Inventar-, Terminalreport- und Cachekorrekturen reviewen,
  fokussiert bauen und auf `main` einchecken
- genau einen frischen ABI-74-NativeDisc-Port aus diesem Stand erzeugen
- Originaldisc real installieren
- exakt 600 Millionen Post-Entry-Zyklen mit relativem Gate ausfuehren
- separaten sichtbaren Fensterlauf aufnehmen
- sowohl den v32-Blocker `0x8C11088C -> 0x8C64784E` als auch die
  v33-Grenze `0x8C65EA06 -> 0x8C0101F2` ueber validiertes statisches AOT
  passieren oder einen engeren typisierten Blocker belegen
- Zentraldispatches, Hostzeit, Gast-MHz, PVR/Hostframes und hoechsten
  sichtbaren Screen gegen v32 dokumentieren

### Lauf B - DirectBoot nach NativeDisc-Abnahme

- frischen ABI-74-CompletePlatform-Zustand am Game Entry erfassen
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
