# Codex Handoff

Dieses Dokument definiert, wie Codex oder ein anderer automatisierter
Bearbeiter an KatanaRecomp arbeitet. Die repositoryweiten Regeln in
`../AGENTS.md` sind verbindlich und haben Vorrang vor widersprechenden
aelteren Prozessbeschreibungen.

## Pflichtlekture vor jeder Aenderung

1. `AGENTS.md`
2. `ROADMAP.md`
3. `docs/STATUS.md`
4. `docs/TASKS.md`
5. `docs/TASK_ID_REGISTRY.md`
6. `CHANGELOG.md`
7. `docs/SONIC_ADVENTURE_ACCEPTANCE.md`
8. der fuer den Task relevante Detailplan
9. betroffene Header, Implementierungen und vorhandene Tests, sofern deren
   bestehender Vertrag durch den Task beruehrt wird

## Projektweiter Taskablauf

Codex bearbeitet immer genau einen freigegebenen Task aus `docs/TASKS.md`.
Fuer jeden Task gilt exakt:

```text
Task implementieren
  -> alle durch den Task betroffenen Pfade reviewen
     und bestaetigte Fehler innerhalb dieses Reviews schliessen
  -> den reviewten Task direkt auf main committen und pushen
  -> naechster Task
```

Die Reviewstufe ist keine Kommentarrunde, sondern die Fehlerfindungs- und
Fixstufe. Sie umfasst mindestens:

- die geaenderte Implementierung;
- direkte und transitive Aufrufer und Verbraucher;
- Datenfluss, Kontrollfluss, Ownership und Lebenszeiten;
- Fehler-, Abbruch-, Rollback- und Teilmutationspfade;
- Decoder, Analyse, IR, Codegenerator und Runtime, soweit betroffen;
- ABI-, Cache-, Schema-, Versions- und Artefaktvertraege;
- AOT-Vollstaendigkeit, statische Bindung und Runtimeautoritaet;
- Dokumentation und Taskstatus;
- vorhandene Tests nur dann, wenn sie selbst gebrochen, widerspruechlich oder
  zahlenmaessig falsch sind.

Bestaetigte Fehler im Taskscope werden vor dem Push geschlossen. Eine
separate standardmaessige Fix-, Verifikations-, Test- oder
Integrationsphase wird nicht angelegt.

Keine benachbarten Roadmap-Punkte werden nebenbei implementiert, ausser sie
sind fuer den Task zwingend notwendig. Ein Review darf ausserhalb des
Taskscopes liegende Beobachtungen knapp notieren, daraus aber weder neue
Tasks noch eine Scope-Erweiterung ableiten.

## Direkte Arbeit auf main

- Regulaere Tasks werden direkt auf `main` bearbeitet, committed und
  gepusht.
- Keine neuen Taskbranches, Pull Requests oder parallelen
  Integrationszweige ohne ausdrueckliche Nutzeranweisung.
- Vor jeder Aenderung aktuellen `main`-Head und Dateistand erfassen.
- Vor jedem Schreibvorgang pruefen, dass keine fremden oder neueren
  Aenderungen ueberschrieben werden.
- Erst der Push des reviewten Tasks gibt den naechsten Task frei.
- Der Push ist die Freigabe; fuer den naechsten ungegateten Task ist keine
  weitere Nutzeranweisung erforderlich.
- Ein Commit beschreibt genau den abgeschlossenen Task oder, bei einer
  reinen Dokumentationsaenderung, genau den geaenderten Projektvertrag.

## Sonic ist der Test

Der private Sonic-Adventure-PAL-Port ist projektweit der massgebliche
Produkt- und Integrationstest:

```text
realer Portexport
  -> Installation aus der lokalen Originaldisc
  -> normaler Produktlauf
  -> sichtbarer Boot-/Spielfortschritt
```

Daraus folgt:

- keine neuen Unit-Tests, Regressionstests, Testmatrizen, synthetischen
  Fixtures, Stresslaeufe, Testprojekte, Ersatzgates oder
  Konformitaetssuiten;
- fehlende neue Tests sind kein Finding;
- Reviews verlangen keine neue Testabdeckung;
- vorhandene Tests duerfen auf gebrochene Erwartungen, falsche Testzahlen
  oder widerspruechliche Semantik geprueft und bei Bedarf repariert werden,
  ihr Bestand wird aber nicht erweitert;
- ein Implementierungstask startet keinen eigenen Testbuild und keine Matrix
  als Pushgate;
- Sonic-Laeufe erfolgen an den in Roadmap und Tasks festgelegten
  Produktgates oder auf ausdrueckliche Nutzeranweisung, nicht nach jedem
  Task;
- mehrere zusammenhaengende, reviewte Tasks duerfen vor dem naechsten
  Sonic-Lauf auf `main` landen;
- vorhandene CI oder bestehende Checks koennen beobachtet werden, ersetzen
  aber weder den Quellpfadreview noch den Sonic-Produktnachweis;
- ein technischer Frame, ein Counter oder eine gruene synthetische
  Auswertung ist kein sichtbarer Spielboot.

Die genauen Retail-, Datenschutz- und Inhaltsgrenzen stehen in
`docs/SONIC_ADVENTURE_ACCEPTANCE.md`.

## Startprozedur

1. freigegebenen Task und seine Abhaengigkeiten bestimmen;
2. aktuellen `main`-Head erfassen;
3. `git`-/Repositoryzustand und beruehrte Dateien pruefen;
4. aktuellen Source-, Diagnose- und Produktevidenzstand getrennt erfassen;
5. relevante Architektur- und Detaildokumente lesen;
6. den vollstaendigen betroffenen Pfad bestimmen;
7. erst danach implementieren.

Regulaere Implementierungstasks konfigurieren oder bauen beim Start nicht und
starten keine Tests. Ein realer Export oder Produktlauf wird nur ausgefuehrt,
wenn der Task selbst ein dokumentiertes Sonic-Diagnose- oder Produktgate ist
und der Nutzer diesen Lauf freigegeben hat.

## Laufzeit und Ressourcen

- Kein Prozess und keine einzelne Phase laeuft laenger als 15 Minuten, ausser
  der Nutzer hebt die Grenze fuer genau einen benannten Lauf auf.
- Jeder potenziell lange Prozess meldet spaetestens alle zehn Sekunden
  belastbaren Fortschritt oder einen Heartbeat.
- Ausgabe muss waehrend des Laufs sichtbar sein; ein erst am Ende
  ausgegebener Pufferlog reicht nicht.
- Heartbeats ohne Aenderung von Phase, geplant, queued, aktiv, fertig oder
  kanonisch publiziert belegen nur Liveness.
- Bleibt ein Prozess 60 Sekunden ohne nachweisliche Arbeitsbewegung, wird er
  als Stall beendet und sein Prozessbaum quiesziert.
- CPU-Last, steigende Cache-, Evaluation-, Requeue- oder Contextzaehler sind
  allein kein Fortschritt.
- Bei `planned > 0` und `canonical == 0` gilt der First-Publish-Vertrag aus
  `AGENTS.md`.
- Produktive Arbeit nutzt die verfuegbaren Hostressourcen parallel;
  Ein-Kern-Ausfuehrung ist kein akzeptabler Default.
- Ein abgebrochener Prozess wird mit seinem gesamten Prozessbaum beendet,
  bevor ein Nachfolger startet.

## Schichtentrennung

### Decoder

Zustaendig fuer:

- Opcode-Maske;
- Operanden;
- Immediate- und Displacement-Dekodierung;
- Instruktionsmetadaten;
- lesbare Disassembly.

Nicht zustaendig fuer Runtime-Speicher, Kontrollflussstrategie oder
C++-Emission.

### Analyse

Zustaendig fuer:

- Sprungziele und Delay Slots;
- Basic Blocks und Funktionen;
- indirekten Kontrollfluss;
- Code-Daten-Trennung;
- Guarded-AOT-Inventar und Vollstaendigkeit;
- Context-, Summary-, Candidate- und Dependency-Vertraege.

### IR

Zustaendig fuer:

- semantische, backendunabhaengige Operationen;
- Operandbreiten;
- Status- und Speichereffekte;
- Architekturgrenzen und Verifikation.

### Codegenerator

Zustaendig fuer:

- Uebersetzung gueltiger IR;
- Runtime-ABI-Nutzung;
- statische native AOT-Ausgabe;
- keine erneute SH-4-Dekodierung;
- keine versteckte Analyse und keinen Runtime-Fallback.

### Runtime

Zustaendig fuer:

- CPU-Zustand;
- Speicherbus und MMIO;
- Ausnahmen und Interrupts;
- Scheduler und Geraete;
- Hostpresentation, Eingabe und Plattformdienste;
- keine erfundenen Hardwareerfolge.

Nicht jede Aenderung betrifft jede Schicht. Das Review muss aber
systematisch pruefen, welche Schichten und Vertraege tatsaechlich betroffen
sind.

## Reviewregeln

Ein guter Taskreview beantwortet mindestens:

1. Ist die Implementierung vollstaendig verdrahtet?
2. Bleiben alle Eingangs-, Ausgangs- und Fehlerpfade korrekt?
3. Gibt es stille Datenverluste, Teilmutationen oder fail-open Verhalten?
4. Sind Register-, Speicher-, Vorzeichen-, Carry-/Borrow- und
   Reihenfolgevertraege korrekt, soweit der Task sie beruehrt?
5. Bleiben identische Register- und Aliasfaelle korrekt?
6. Sind Cache-Keys, Invalidierung und Versionierung vollstaendig?
7. Kann eine Analysegrenze oder ein Budget unbemerkt Produktcode auslassen?
8. Bleibt der normale Produktpfad strikt AOT-only?
9. Wurden breite Stringersetzungen, doppelte `case`-Labels oder ungenaue
   Texttransformationen eingefuehrt?
10. Gelangen Retaildateien, geschuetzte Bytes oder daraus unzulaessig
    erzeugte verteilbare Inhalte in Repository oder Paket?
11. Sind vorhandene Testzahlen oder bestehende Tests konkret falsch oder
    gebrochen?

Nicht gefragt wird:

- Welche neuen Tests koennten noch gebaut werden?
- Welche neue Matrix waere vorsichtshalber nett?
- Welche synthetische Reproduktion koennte Sonic ersetzen?

Das Fehlen neuer Tests wird nie als Finding ausgegeben.

## Rechtliche und inhaltliche Grenzen

Nicht committen oder verteilen:

- kommerzielle Executables;
- BIOS-Dateien;
- Disc-Images oder Tracks;
- extrahierte Assets;
- private Captures, Rohlogs, Hashes oder lokale Pfade;
- aus Referenzprojekten kopierten oder mechanisch uebersetzten Code;
- aus kommerziellem Gastcode erzeugte spielgebundene Artefakte, sofern deren
  Veroeffentlichung nicht ausdruecklich rechtlich geklaert ist.

Titelgebundene Generierung und Installation erfolgen lokal im externen
Spielprojekt. Der generische Katana-Kern enthaelt keine Sonic-Adressen oder
Sonderpfade.

## Referenzprojekte und Dokumentation

Referenzen duerfen verwendet werden, um:

- Architektur und beobachtbares Verhalten zu verstehen;
- offizielle SH-4- und Dreamcast-Vertraege zu vergleichen;
- Dateiformate und Semantik zu recherchieren;
- eine unabhaengige generische Implementierung zu begruenden.

Nicht erlaubt sind:

- Codekopie;
- mechanische Uebersetzung;
- ungepruefte Uebernahme von Kommentaren oder Tabellen;
- Aenderungen an Referenzdateien;
- Ableitung neuer Testpflichten aus einer Referenz.

## Dokumentationspflicht

Jeder Task aktualisiert die Dokumente, deren aktueller Vertrag oder Status
durch die Aenderung betroffen ist. Historische Evidenz bleibt als historisch
markiert. Source-, Diagnose- und Produktevidenz duerfen nicht vermischt
werden.

Die Abschlussmeldung eines Tasks enthaelt:

- Task-ID;
- geaenderte Schichten und Dateien;
- implementierte Semantik;
- reviewte Pfade;
- gefundene und geschlossene Findings;
- verbleibende bekannte Grenzen im Taskscope;
- Commit auf `main`;
- naechsten nicht blockierten Task.

Sie enthaelt keine Liste fehlender Tests und keine Empfehlung fuer eine neue
Testmatrix.

## Aktueller P0-Handoff

Die Sourcebasis fuer den aktuellen Candidate-Resolution-Pfad ist
`dd3ff7eccec5c3f0c6308ee44c315fb2f6bf55fa` plus das reviewte KR-4994-Delta;
Analyzer-ABI 33, Function-Analysis-Epoch-Schema 15.

Der terminale Sonic-v56-Diagnoselauf ergab:

```text
Laufzeit:                                      1:28:24
Exitcode:                                      5
committed Roots:                               1 / 1.191
Contextual-Return-Evaluationsbudget:           65.536, ausgeschoepft
Context-Limit:                                 nicht erreicht
eindeutige Contexts:                           25.728
physische Auswertungen:                        27.872
Eviction-Recomputes:                           0
Retention:                                     incomplete-root
Portartefakt / game.exe / Screenshot:          keines / keine / keiner
```

Null Eviction-Recomputes liefern keinen Beleg fuer Cache-Eviction als
Hauptursache. Der P0 liegt im Candidate-Resolution-Pfad. Das
Per-Function-Budget von `65.536`
und die laufweiten Aggregate von `25.728` Contexts und `27.872` physischen
Auswertungen besitzen noch keinen belegten gemeinsamen Root-/Funktionsscope.
Die historische v56-Ausgabe besass noch keinen gemeinsamen Root-, Funktions-
und Zaehlscope; ihre Rohwerte bleiben getrennte historische Aggregate.

Der gemeinsame Source-Fix ist fuer KR-4985 und KR-4986 abgeschlossen. KR-4987
ist source-seitig abgeschlossen: Die Read-Lens-projizierte Contextual-
SemanticLane-Identitaet verwendet vollstaendige Key-Bytes; Vertragsluecken,
Truncation und Fallback bleiben strikt FullState. Exakte Provenienz/Restore
und Discovery -> Freeze -> Publish bleiben unveraendert. Der gezielte
`katana-recomp`-Build war laut Review in `42,4 s` erfolgreich. Der D9-Lauf ist
beendet und fail-closed; Root 0 konvergierte ohne Portartefakt oder
Produkterfolg.

diese Source-Fixes beheben die historische Budgetfehlbelastung vor
semantischer Deduplizierung
durch kollisionssichere Full-State-Semantic-Lanes und private exakte
Provenienz-Replays. Die D1-Telemetrie ist explizit opt-in.

Der einzige freigegebene D1-Lauf lieferte bei `185,370 s` nichtterminale
Root-0-Evidenz: `0/1191` Roots completed, Wave `1.019`, Frontier `0` (maximal
`223`), `288` Contexts, `15.170` logische Requests, `6.724` Semantic-Lanes,
`6.725` physische Auswertungen, `5.846` Cache-Reuses, `15.157` exakte
Subscriber und `226.886` Provenienzverknuepfungen. Requeues: `1` initial
root, `287` neue exakte Lane, `8.248` Input-Widening, `177` Summary,
`405` Forward-Edge und `6.052` stale Dependency; stale Discards `12.643`.
Die temporaere JSONL war nach dem Supervisor-I/O-Fehler bis `185,586 s`
lesbar/gespuelt, aber ohne terminalen Datensatz und ohne atomare Publikation.
Root 1 wurde nicht erreicht; D1/G1 ist deshalb fail-closed und unentschieden.

Der D9-Lauf dauerte `20,331 s` und endete beim ersten fail-closed
Telemetrie-/Publikationssignal. Root 0 erreichte Wave `184`, Frontier `0`
(maximal `216`), `288` admitted contexts, `2.724` admitted evaluations/
Semantic-Lanes, `4.349` logical requests, `3.739` physical evaluations,
`2.497` input-widening und `932` stale-dependency requeues, `1.740` stale
snapshot discards sowie `939` semantic und `2.377` provenance-only widenings.
Budgets blieben unverbraucht; Epochs published/discarded `0/1`, Retention
`incomplete-root`. 64 Truncations waren state/identity mit `values=0`, 6
Value-Overflows hatten jeweils `merged_values=9`, und 462 Stack-Loss-
Diagnosen verteilten sich auf 189 forwarded-call, 158 candidate-store, 113
fixpoint-call und 2 forwarded-tail; tail-store-identity-loss `0`. Kein
Portartefakt und kein Produkterfolg.

Der aktuelle D-Lauf dauerte `460,6 s` gesamt, Candidate Resolution ca.
`325,8 s`; der identifizierte Kindprozess wurde nach belegter
Nichtverbesserung manuell beendet. Es gab `0/1194` committed Roots, HOL `0`,
Wave `103`, `272` Contexts, `1.044` Semantic-Lanes, `1.029` contextual
physical evaluations, `2.430` contextual logical requests, `1.359` Input-
Widening-, `29` Summary- und `733` stale-Dependency-Requeues, `1.359` stale
snapshot discards, `518.425.788 B` Cache-Payload, `3.964` physische
Auswertungen gesamt und `0/0` publizierte/verwarfene Epochen. Context-/
Evaluation-/Composite-Budgets blieben unverbraucht; kein Portartefakt. Bei
Attempts `1024`, `2048` und `4096` blieb die relevante
Admission-/Stack-Diagnostik bitgenau gleich; der Durchsatz stieg, der
semantische Lane-Treiber bleibt offen. KR-4981 ist nicht bestanden.

Der verbindliche aktuelle Pfad lautet:

```text
D9 beendet fail-closed; kein Portartefakt und kein Produkterfolg
```

KR-4988 bis KR-4991 bleiben inaktiv. KR-4994 ist source-seitig abgeschlossen;
der semantische Lane-Treiber bleibt der offene P0-Produktblocker.
Candidate-Resolution-Gesamtzeit,
Limitfreiheit, terminale IncompleteRoot-/Retentionwerte, Coverage und G1
sind ohne vollstaendigen schweren Root und den historischen Root 1 nicht
entscheidbar. D2/G2 wurde nicht ausgefuehrt. KR-4981 bleibt das globale
Produktgate; ein Retry ist erst nach KR-4994 plus Sol-Review genau einmal
zulaessig. Ein zweiter D1-Lauf gehoert
nicht zu diesem Dokumentationspass.

D1 und D2 sind ausdruecklich freizugebende Sonic-Diagnoseexporte, keine
Testmatrix. Der vollstaendige KR-4993-Source-Endreview ist abgeschlossen; das
Analyzer-ABI-Finding wurde mit ABI 32 geschlossen. KR-4994 ist source-seitig
abgeschlossen, aber der semantische Lane-Treiber bleibt offen. Es gibt kein
bestandenes Produktgate; die Produkt-P0-Abnahme bleibt offen.

## Abschlusscheck vor dem Push

```text
- [ ] Taskscope vollstaendig implementiert
- [ ] alle betroffenen Pfade reviewt
- [ ] bestaetigte Findings geschlossen
- [ ] AOT-/Runtime-/Fehlervertraege fail-closed
- [ ] keine Sonic-Sonderfaelle oder Retaildaten
- [ ] keine neue Test-, Fixture- oder Matrixinfrastruktur
- [ ] vorhandene falsche Testzahlen oder gebrochene Tests, falls betroffen,
      korrigiert
- [ ] relevante Dokumentation aktualisiert
- [ ] direkt auf main committed und gepusht
```
