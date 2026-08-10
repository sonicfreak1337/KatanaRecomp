# Codex Handoff

Dieses Dokument definiert, wie Codex oder ein anderer automatisierter
Bearbeiter an KatanaRecomp arbeitet. Die repositoryweiten Regeln in
`../AGENTS.md` sind verbindlich und haben Vorrang vor widersprechenden
aelteren Prozessbeschreibungen.

## Aktueller RuntimeOnly-Bring-up

Funktionaler RuntimeOnly-Source-Stand: `efc531b`. Aktuell gelten Runtime-ABI `89`,
PlatformServices-ABI `14`, Analyzer-ABI `34`, Function-Analysis-Epoch-Schema
`27`, lokales In-Process-Evaluation-Cache-Schema `13`, Backend-Interface-ABI
`13` und Portprojektvertrag `75`.
Die inkompatible Erweiterung der oeffentlichen SDK-Layouts
`PortExportOptions` und `LatentAotDiscoveryOptions` hebt das Backend-
Interface-ABI auf `13`; bestehende generierte Ports muessen neu exportiert
werden.
Aktuelles Native-AOT-Emissionsprofil: `25`, AOT-Partitionsschema: `5`.

Der opt-in CLI-Modus `port --analysis-mode runtime-only` ist fuer den
vollstaendigen NativeDisc-Produktport mit `--game-project` zulaessig; der
Default bleibt `platform`. RuntimeOnly setzt `GuestCallAbi::Unknown`, umgeht
die blockierende SuperHC-FunctionValue-/Candidate-Resolution, erzeugt
weiterhin nativen AOT-Code und verwendet RuntimeOnly-Dispatch ueber eine
exakte statische Guest->Host-Tabelle. Stop-on-miss und typed abort bleiben
aktiv; Interpreter, JIT, Runtime-Decoder und geratene Ziele sind nicht Teil
des Pfads. Der Whole-Export-Cache ist modegebunden.

Sonic-spezifische `SA_PRIVATE_*`-Dumps und Diagnose-Stacktraces sind aus dem
Repository entfernt; allgemeine Runtime-/Codegen-Fixes bleiben erhalten.

Der PlatformAbi-Default bleibt erhalten. Ordinary-/Inventory-Stack-Alias-
Capture und Lane-Fusion sind deferred PlatformAbi-Optimierungsbefunde und
wurden im RuntimeOnly-Bring-up nicht implementiert. Aeltere Candidate-
Resolution-Abschnitte in diesem Handoff sind historische PlatformAbi-
Diagnostik und keine Aussage, dass der aktuelle Bring-up kein `game.exe`
erzeugt.

### Aktueller RuntimeOnly-v25/v29-Produktstand

Der aktuelle Sonic-PAL-Lauf dauerte `45,539 s` ohne Fatalfehler oder Crash.
Nach Presented by Sega blieb der Kontaktbogen ab etwa `30 s` schwarz;
Memory-Card-Screen und Hauptmenue wurden nicht erreicht. Post-entry wurden
`45,7111 MHz` aus `1.790.309.442` Zyklen in `39,1658 s` gemessen. `396`
erfolgreiche Renderabschluesse publizierten Video, ISP und TSP am selben
Gastzyklus. Die resetfeste TA-Metrik meldete `396` Lifetime-Frames ueber `807`
post-entry Resets. Der unveraenderte Sichtpfad widerlegt die fehlende
RenderDone-Fanout als alleinige Ursache. Der aktuelle P0 liegt in der
nachgelagerten Gast-IRQ-/ACK- und FB_R-Scanout-Folge; KR-4981 bleibt offen.

Der naechste Lauf benoetigt vorab eine kleine generische Korrelation der drei
Completionbits mit Maskierung/ACK und dem anschliessend vom Gast gewaehlten
FB_R-Scanout. Kein automatischer FB_W->FB_R-Flip, kein Movie-Skip und keine
private Bildbruecke ist als Produktfix zulaessig.

Historische v16-Evidenz (nicht aktueller Produktstand):
Der alte Lauf endete fail-closed am generischen Fehler `missing-aot`.
Das historische Memory-Card-Gate blieb offen; Candidate-Resolution und
PlatformAbi-Optimierungen bleiben deferred.

Die aktuellen generischen Source-Deltas umfassen zusaetzlich die vollstaendige
Holly-RenderDone-Fanout und resetfeste TA-Lifetime-/Resetmetriken. Die Cross-
Shard-Codecopy-Abhaengigkeit, der togglebare direkte AOT-Bytecopy-Batch und das
begrenzte Post-Root-Drain bleiben erhalten. Stop-on-miss und typed abort bleiben
unveraendert.

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

## Historischer Candidate-P0-Handoff

Der funktionale Source-Checkpoint fuer den historischen Candidate-Resolution-
Pfad ist `49b0f72a9f49d60a4eb6e0481460cd57c5625735`; Analyzer-ABI 34,
Function-Analysis-Epoch-Schema 27, lokales In-Process-Evaluation-Cache-Schema 13.

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

Der aktuelle Zweikanal-Sourcefix vergleicht den oeffentlichen Call-/State-
Effekt ohne Evidence-Wachstum fuer die logische Lane; alpha-normalisierte
Evidence-Mitgliedschaft bleibt in begrenzten privaten Replaykapseln fuer
physische Auswertung und Restore. Evidence-Stale erzeugt damit kein neues
logisches Budgetereignis; Cap-/Replayfehler bleiben fail-closed.

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

Der korrekte VsDevCmd-Incremental-Build von `katana-recomp --parallel 12`
war in `42,8 s` erfolgreich; es blieben nur bekannte getenv-/Shadowing-
Warnungen. Der fruehere D-Lauf dauerte `460,6 s` gesamt, Candidate Resolution ca.
`325,8 s`; der identifizierte Kindprozess wurde nach belegter
Nichtverbesserung manuell beendet. Es gab `0/1194` committed Roots, HOL `0`,
Wave `103`, `272` Contexts, `1.044` Semantic-Lanes, `1.029` contextual
physical evaluations, `2.430` contextual logical requests, `1.359` Input-
Widening-, `29` Summary- und `733` stale-Dependency-Requeues, `1.359` stale
snapshot discards, `518.425.788 B` Cache-Payload, `3.964` physische
Auswertungen gesamt und `0/0` publizierte/verwarfene Epochen. Context-/
Evaluation-/Composite-Budgets blieben unverbraucht; kein Portartefakt. Bei
Attempts `1024`, `2048` und `4096` blieb die relevante
Admission-/Stack-Diagnostik bitgenau gleich; die Rohwerte sind wegen der
unterschiedlichen Endpunkte nicht direkt vergleichbar und belegen keine
materielle Produkt-/Performanceverbesserung. Inventory-Provenance-Live-in/
Spill-through ist ein historischer Befund; KR-4981 ist nicht bestanden.

Der Candidate-Domain-Top-Fix macht abgeschnittene begrenzte Candidate-Domains
zum kanonischen absorbierenden Top mit leerem endlichem Praefix. Merge,
Normalisierung, Vergleich, Keys, Persistenz, Consumer und ABI-Promotion sind
darauf abgestimmt; der historische Candidate-Domain-Top-Lauf lief unter
Epoch-Schema `18` und Analyzer-ABI `33`. Der historische Source-Checkpoint ist
separat oben ausgewiesen. Der Lauf
`kr4981-20260809-020628-2bfd8af5` endete nach `343,627 s` durch manuellen
Abbruch bei belegter identischer Nichtkonvergenz; letzte Bewegung Wave `48`,
Peak Root `1.450.078.208 B`, Peak Job `1.618.132.992 B`, keine Publikation und
kein Portartefakt. Bei Wave `39` waren die 16 geprueften Kernzaehler exakt wie
im Vorlauf. Der Fix ist ein Korrektheits-/Persistenzfix, kein belegter
Konvergenzhebel; KR-4981 bleibt offen.

Der abgeschlossene Diagnose-Unterauftrag lief unter
`kr4981-20260809-024141-c4ffdf15`, erreichte das vollständige
`attempts=1024`-Gate und wurde nach `244,549 s` bei Wave `24` gezielt beendet.
`uncategorized=0` für alle Top-8-Funktionen; der erwartbare
`product-exit -1`-Status entstand durch diesen Stop. Peak Root WS:
`1.260.388.352 B`, Peak Job WS: `1.387.151.360 B`; keine Publikation und kein
`game.exe`. `0x8C10E44E` ist mit `20` semantischen Änderungen und `40`
Stack-Widenings ausschließlich SavedEpoch-pending-ABI-Skalaren sowie
unvollständigem Callee-Set-Stackvertrag der dominante Befund.
Der SavedEpoch-Lifecycle-Fix ist source-seitig abgeschlossen. Offen bleibt die
gemeinsame Ordinary-/Registermetadaten-/Alias-/Watcher-/Loss-/MemoryEpoch-
Lifecycle-Ursache; Alias-/Current-Tracking und fail-closed Restore bleiben
erhalten. Die dynamischen Callee-Set-incomplete-Gründe werden danach
weiter geprüft; KR-4981 bleibt offen.

Der SavedEpoch-Lifecycle-Unterauftrag ist source-seitig abgeschlossen:
Current-tracking Pending-ABI-Skalare werden nur an bewiesenen normalen
Call-/Tail-ABI-Gates konsumiert, detached Epochs bleiben unangetastet.
`candidate_payload_lost` ist ein absorbierendes Epoch-Top ueber Normalize,
Merge, Equality, Key, Subsumption, Evidence, Restore und Persistenz; konkrete
Evidence und Nested-/Current-Aliasfakten bleiben erhalten, finite Payload/Slots
verschwinden, detached Top uebernimmt keine fremde Tail-Evidence. Der
historische SavedEpoch-Lifecycle-Stand lief unter Epoch-Schema `17` und
Analyzer-ABI `33`.

Der Produktlauf `kr4981-20260809-031826-0616113a` endete nach `369,171 s` mit
`nonconvergence`/Exitcode `31` durch drei zehnsekundige
Null-Publikations-Amplifikationssamples. Wave `76`, `0` committed/ready/
completed Roots, `272` Contexts, `uncategorized=0` in D1024 und D2048; keine
Publikation und kein `game.exe`. Der alte SavedEpoch-Pending-Blocker ist
beseitigt. Der naechste Root-Analysepunkt ist die gemeinsame Ordinary-/
Registermetadaten-/Alias-/Watcher-/Loss- und MemoryEpoch-Lifecycle-Ursache,
nicht ein weiterer SavedEpoch-Pending-Patch; KR-4981 bleibt fail-closed offen.

Der historische Candidate-Resolution-Source-Stand ist `49b0f72a9f49d60a4eb6e0481460cd57c5625735`.
Er erlaubt retained sticky loss in der strukturellen Contextual-Hybrid-Projektion;
die autoritative Hybridprojektion schliesst Contextual-MAY-Joins und Forward-
Edges erneut vollstaendig.
erkennt SavedEpoch-Slot-Pending-Top fail-closed in allen Truncation-/Publication-
Checks und trennt Provenance-Replay-Capsule-/Keybyte-Limits öffentlich vom
semantischen Evaluation-Limit. Der echte Evaluation-Cap belastet nur den
Evaluation-Zähler; Analyzer-ABI `34`, Epoch-Schema `27` und lokales
In-Process-Evaluation-Cache-Schema `13` sind aktiv.

Der historische PlatformAbi-Produktlauf `kr4981-20260809-091410-2766aaa6` endete nach ca.
`275 s` gesamt (Candidate ca. `221 s`) mit `nonconvergence` nach drei
Amplifikationssamples: `0/1274` Roots, HOL `0`, Wave `107`, `280` Contexts,
`970` Semantic-Lanes, `1.861` physische, `2.526` logische Requests,
Input-Widening `536`, Summary `22`, Forward `123`, stale Requeues `272`,
stale Discards `806`, Cache `589.178.706 B`; keine Budgets erschöpft, keine
Publikation und kein Artefakt bzw. `game.exe`. Der Supervisor schrieb wegen
`taskkill`-Zugriffsverweigerung keine Summary; der Kill-on-close-Job beendete
den Child trotzdem. Admission `1024/1024`, projected context/match jeweils
`0`; `0x8C641202` blieb bei `84/84` Attempts/Semantic Changes und `508`
Ordinary-Stack-Deltas trotz vollständigem Stackvertrag. Der historische P0 ist
die fehlende Wirksamkeit der autoritativen Hybrid-Join-Closure beim
vollstaendigen Stackvertrag/Gate.

Der vorherige Produktlauf `kr4981-20260809-083308-4a3ff9be` endete nach
`286,387 s` (Candidate ca. `232,5 s`) mit `nonconvergence`/Exit `31`: `0/1274`
Roots, Wave `119`, keine Publikation, `280` Contexts, `972` Lanes, `2.011`
physische, `2.814` logische, `203` Cache-Reuses, `2.790` Subscriber,
Provenienz `169.824`, stale Discards `922`, Frontier `43` (max `250`), Cache
`610.295.241 B`, kein Artefakt. Admission `1024/1024`, projected context/match
jeweils `0`; der P0 liegt intra-context bei Ordinary-Stack und lokalen
Stackkoordinaten.

Der Lauf `kr4981-20260809-041945-21b70ade` endete nach `425,924 s` bei Wave
`60`, `0/1194` Roots, `758` Lanes, `984` physischen und `1.398` logischen
Auswertungen, `248` Input-, `102` stale-Requeues und `347` Discards; Peak Root
`1.606.066.176 B`, Peak Job `1.814.822.912 B`, kein Portartefakt.
Der Vergleichslauf `kr4981-20260809-050420-3f47fd65` endete nach `322,632 s`
(Candidate `237,116 s`) bei Wave `39`, `0/1194` Roots, `272` Contexts,
`549` Lanes, `630` physischen, `894` logischen Auswertungen, `181` Input-,
`10` Summary-, `76` stale-Requeues, `226` Discards und Provenienz `31.713`;
kein `game.exe`. Das `attempts=1024`-Gate war gegenüber `9baea88` bitgleich
(`admission_success=999`, projected changed/match jeweils `0`), also korrekt
geändert, aber kein Konvergenzhebel. Der offene P0 ist Inventory-Provenance-
Live-in/Spill-through (r12/SavedEpoch), nicht SavedEpoch-Pending oder Budgetarbeit.

Der verbindliche aktuelle Pfad lautet:

```text
D9 beendet fail-closed; kein Portartefakt und kein Produkterfolg
```

KR-4988 bis KR-4991 bleiben inaktiv. KR-4994 ist source-seitig abgeschlossen;
der historische P0 ist die fehlende Wirksamkeit der autoritativen Hybrid-Join-
Closure beim vollstaendigen Stackvertrag/Gate.
Candidate-Resolution-Gesamtzeit,
Limitfreiheit, terminale IncompleteRoot-/Retentionwerte, Coverage und G1
sind ohne vollstaendigen schweren Root und den historischen Root 1 nicht
entscheidbar. D2/G2 ist abgeschlossen und negativ; ein positiver
Schedulerhebel ist nicht belegt. KR-4981 bleibt das globale
Produktgate; der aktuelle D-Lauf ist abgeschlossen und nicht bestanden. Ein
weiterer Lauf ist nicht automatisch freigegeben. Ein zweiter D1-Lauf gehoert
nicht zu diesem Dokumentationspass.

D1 und D2 sind ausdruecklich freizugebende Sonic-Diagnoseexporte, keine
Testmatrix. Der vollstaendige KR-4993-Source-Endreview ist abgeschlossen; das
Analyzer-ABI-Finding ist geschlossen; der aktuelle Analyzer-ABI ist `34`. KR-4994 ist source-seitig
abgeschlossen, aber die autoritative Hybrid-Join-Closure ist beim vollstaendigen
Stackvertrag/Gate noch nicht wirksam. Es gibt kein
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
