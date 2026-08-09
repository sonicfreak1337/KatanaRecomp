# KatanaRecomp Roadmap

Status: Pre-Alpha

Aktuelle Phase: `v0.49.0` - Sonic-Adventure-Produktbring-up,
vollstaendiger statischer AOT-Port und produktiver Dreamcast-Handoff

Erster oeffentlicher Release: `v0.50.0` Alpha

## Produktziel

KatanaRecomp ist ein statischer SH-4-Recompiler. KatanaRuntime ist die
gemeinsam installierbare Dreamcast-Laufzeitbibliothek. Ein konkretes Spiel
wird in einem getrennten, hashgebundenen Recomp-Projekt gebaut.

```text
KatanaRecomp
  -> analysiert SH-4 statisch
  -> erzeugt natives C++/Hostprogramm

KatanaRuntime
  -> stellt gemeinsame Dreamcast-Plattformvertraege bereit

SonicAdventureRecomp
  -> bindet generierten Spielcode und lokal installierte Originaldaten
  -> erzeugt die startbare Produkt-EXE
```

KatanaRecomp und KatanaRuntime bleiben im selben Repository, sind aber
getrennte Build- und Installationsprodukte. Titeladressen, Titelhooks,
private Symbole und Installationsprofile gehoeren langfristig in das externe
Spielprojekt. Produktbefunde duerfen im Bring-up dokumentiert werden, aber
nie als Sonic-Sonderfall in generischem Runtime- oder Recompilercode landen.

## Projektweiter Arbeitsvertrag

Fuer jeden Task und jeden Projektbereich gilt ab sofort exakt:

```text
Task implementieren
  -> alle durch den Task betroffenen Pfade reviewen
     und bestaetigte Fehler innerhalb dieses Reviews schliessen
  -> den reviewten Task direkt auf main committen und pushen
  -> naechster Task
```

Die Reviewstufe ist die Fehlerfindungs- und Fixstufe. Sie umfasst den
implementierten Pfad, Aufrufer, Verbraucher, Datenfluss, Verdrahtung,
Fehlerpfade, ABI-, Cache-, Versions-, AOT- und Runtimevertraege sowie alle
unmittelbar betroffenen Schichten. Bestaetigte P0-, P1- und andere fuer den
Task relevante Fehler werden vor dem Push geschlossen.

Es gibt keine zusaetzliche standardmaessige Test-, Verifikations-,
Integrations- oder Fixrunde zwischen Review und Push. Tasks werden direkt auf
`main` bearbeitet und veroeffentlicht. Branches, Pull Requests oder
parallele Integrationszweige entstehen nur auf eine neue ausdrueckliche
Nutzeranweisung.

## Sonic ist der Test

Der reale Sonic-Adventure-PAL-Port ist der projektweit massgebliche Produkt-
und Integrationstest:

```text
realer Export
  -> Installation aus der lokalen Originaldisc
  -> normaler Produktlauf
  -> sichtbarer Boot- und Spielfortschritt
```

Daraus folgen verbindlich:

- keine neuen Unit-Tests, Regressionstests, Testmatrizen, synthetischen
  Fixtures, Stresslaeufe, Testprojekte, Ersatzgates oder
  Konformitaetssuiten als Bestandteil eines Tasks;
- Reviews melden das Fehlen neuer Tests nicht als Finding und verlangen keine
  neue Testabdeckung als Abschlussbedingung;
- vorhandene Tests duerfen auf gebrochene Erwartungen, widerspruechliche
  Semantik oder falsche Testzahlen geprueft und bei Bedarf repariert werden,
  ihr Bestand wird fuer neue Tasks aber nicht erweitert;
- ein Task besitzt keinen eigenen Testbuild als Pushgate;
- Sonic-Laeufe erfolgen an den in dieser Roadmap festgelegten Produktgates
  oder nach ausdruecklicher Nutzeranweisung, nicht nach jedem einzelnen Task;
- mehrere zusammenhaengende, reviewte Tasks duerfen vor dem naechsten
  Sonic-Produktlauf auf `main` landen;
- Performance wird am echten End-to-End-Port gemessen, nicht an einer
  synthetischen Matrix oder einer schoenen CPU-Auslastungszahl.

## Unverhandelbare Produktgrenzen

- kein allgemeiner SH-4-Interpreter im normalen Produktport;
- kein JIT;
- kein Emulationsfallback;
- keine stillen No-op-Stubs oder erfundenen Hardwareerfolge;
- keine Sonic-spezifischen Adresshacks im generischen Katana-Kern;
- keine Retail-, BIOS- oder Assetdaten im Repository oder verteilbaren Paket;
- kein aus kommerziellen Dateien kopierter oder ungebunden verteilter Code;
- Flycast und XenonRecomp sind Referenzen, keine Codequellen;
- das echte erzeugte Produkt bleibt die Boot-, Integrations- und
  Performanceabnahme;
- Produktlaeufe werden nach gleicher Gastarbeit verglichen, nicht nach einer
  beliebigen Hostzeit.

## Aktueller Evidenzstand

Die folgenden Staende bleiben getrennt:

```text
letzte reale Produktevidenz:
  historische NativeDisc-/DirectBoot-Ports mit aelteren ABI-Vertraegen

funktionale Sourcebasis:
  21c24d3c374bcf2d01e523a4c32b4e9f4b0aba3b
  plus das reviewte SavedEpoch-Lifecycle-Delta dieses Tasks
  Analyzer-ABI 33
  Function-Analysis-Epoch-Schema 17

aktueller realer Diagnosebefund:
  D-Lauf, 460,6 s gesamt; Candidate Resolution ca. 325,8 s
  manuelles Beenden des identifizierten Kindprozesses nach belegter Nichtverbesserung
  0/1194 Resolution-Roots committed, HOL 0, Wave 103
  272 Contexts, 1.044 Semantic-Lanes, 1.029 contextual physical evaluations
  2.430 contextual logical requests, 1.359 Input-Widening-, 29 Summary- und
  733 stale-Dependency-Requeues, 1.359 stale snapshot discards
  518.425.788 B Cache-Payload, 3.964 physische Auswertungen gesamt
  0/0 publizierte/verwarfene Epochen, kein Portartefakt, keine game.exe
```

Der aktuelle Lauf nach dem Candidate-Domain-Top-Fix (`kr4981-20260809-020628-2bfd8af5`)
endete nach `343,627 s` durch manuellen Abbruch bei belegter identischer
Nichtkonvergenz. Die Voranalyse bis zum Candidate-Start dauerte etwa `146 s`
einschliesslich des Gesamtstarts; die letzte Bewegung war Wave `48`. Peak Root
lag bei `1.450.078.208 B`, Peak Job bei `1.618.132.992 B`; es gab keine
kanonische Publikation und kein Portartefakt. Bei Wave `39` waren die 16
geprueften Kernzaehler exakt identisch zum Vorlauf (darunter Frontier `177`,
Contexts `272`, Semantic-Lanes `606`, physische Auswertungen `645`, exakte
Subscriber `870`, Provenienz `21.355`, Input-Widening `263`, Summary `10`,
Forward `123`, stale `95`, stale Discards `299`, semantische Widenings `553`
und provenance-only `382`). Der Top-Fix ist damit ein Korrektheits- und
Persistenzfix, kein belegter Konvergenzhebel; KR-4981 bleibt offen.

Der abgeschlossene Diagnose-Unterauftrag im Lauf
`kr4981-20260809-024141-c4ffdf15` erreichte das vollständige
`attempts=1024`-Gate und wurde nach `244,549 s` bei Wave `24` gezielt beendet;
der daraus erwartbare Supervisorstatus `product-exit -1` ist kein Fehler- oder
Hängerbefund. Peak Root lag bei `1.260.388.352 B`, Peak Job bei
`1.387.151.360 B`; es gab keine Publikation und kein `game.exe`.
`uncategorized=0` galt für alle Top-8-Funktionen. Bei `0x8C10E44E` waren alle
`20` semantischen Änderungen und `40` Stack-Widenings ausschließlich
SavedEpoch-pending-ABI-Skalare; der Stackvertrag war am Callee-Set unvollständig.
`0x8C09859C` zeigte `28` Änderungen mit gemischten Domänen und ebenfalls
unvollständigem Callee-Set; `0x8C64E55E` zeigte `48` Änderungen bei vollständigem
Stackvertrag, darunter `reg_epoch_pending=180`. Der Diagnose-Unterauftrag ist
damit abgeschlossen, KR-4981 und das globale Sonic-Produktgate bleiben offen.
Nächster Fixpfad ist die noch nicht implementierte absorbierende Top-Semantik
für `candidate_payload_lost`/Unresolved-SavedEpoch über Normalize, Merge,
Equality, Subsumption, Key und Persistenz; Alias-/Current-Tracking und
fail-closed Restore bleiben erhalten.

Der SavedEpoch-Lifecycle-Unterauftrag ist source-seitig abgeschlossen. Current-
tracking SavedEpoch-Pending-ABI-Skalare werden nur an bewiesenen normalen
Call-/Tail-ABI-Gates konsumiert; detached Epochs bleiben unangetastet.
`candidate_payload_lost` ist ein physisch und semantisch absorbierendes
Epoch-Top ueber Normalize, Merge, Equality, Key, Subsumption, Evidence,
Restore und Persistenz. Konkrete Evidence sowie Nested-/Current-Aliasfakten
bleiben erhalten, finite Payload/Slots verschwinden; detached Top uebernimmt
keine fremde Tail-Evidence. Das Epoch-Schema ist `17`, Analyzer-ABI `33`
bleibt ohne oeffentliche Layoutaenderung.

Der Lauf `kr4981-20260809-031826-0616113a` endete nach `369,171 ms`
(`6:09`) mit Status `nonconvergence`, Exitcode `31`, nach drei zehnsekundigen
Null-Publikations-Amplifikationssamples. Wave `76`, `0` committed/ready/
completed Roots, `272` Contexts; Semantic-Lanes `846 -> 863 -> 886`,
physische Auswertungen `1.135 -> 1.164 -> 1.213`, Frontier `101 -> 88 -> 131`
und stale Discards `395 -> 396 -> 415`. Peak Root lag bei `1.663.037.440 B`,
Peak Job bei `1.895.583.744 B`; keine Publikation und kein `game.exe`.
D1024 und D2048 hatten `uncategorized=0`. Der alte SavedEpoch-Pending-Blocker
ist damit zielgenau beseitigt; der naechste Root-Analysepunkt ist die gemeinsame
Ordinary-/Registermetadaten-/Alias-/Watcher-/Loss- und MemoryEpoch-Lifecycle-
Ursache, nicht ein weiterer SavedEpoch-Pending-Patch. KR-4981 bleibt fail-closed
offen.

Historische Ports belegen keinen aktuellen Sourcezustand. Der v56-Lauf ist
Diagnoseevidenz und kein Produktnachweis, weil kein Produkt entstand.

Der gemeinsame Candidate-Resolution-Explosionsfix erfuellt KR-4985 und
KR-4986 source-seitig: Full-State-Semantic-Lanes sind kollisionssicher,
exakte Provenienzabonnenten werden privat replayt, und das Budget wird nur bei
neuer semantischer Lane belastet. Die D1-Telemetrie ist explizit opt-in und
bleibt ohne Detailtelemetrie vollstaendig aus dem Progress-Hotpath.

Der einzige freigegebene D1-Lauf lieferte valide nichtterminale Root-0-
Transport- und Fortschrittsevidenz, erreichte aber weder den historisch
limitierenden Root 1 noch einen vollstaendigen schweren Root. Nach einem
privaten Supervisor-I/O-Fehler war die temporaere JSONL bis `185,586 s`
lesbar/gespuelt, aber ohne terminalen Datensatz und ohne atomare Publikation.
D1/G1 ist daher strikt fail-closed und unentschieden; KR-4987 bis KR-4991
bleiben inaktiv.

## Aktueller P0: Candidate-Resolution / ungeklaerte Context- und Requeuekosten

Null Eviction-Recomputes liefern keinen Beleg fuer Cache-Eviction als
Hauptursache. Der gemeinsame Source-Fix schliesst die zuvor unguenstige
Budgetdomane: Exakte Provenienzrequests teilen kollisionssichere
Full-State-Semantic-Lanes, waehrend private Evidence-Replays die exakte
Provenienz erhalten. Das Produktgate bleibt wegen der unvollstaendigen D1-
Evidenz offen; als Kostenmodell gilt weiterhin:

```text
echte semantische Contextmenge
  x
Kosten je Context
  x
ueberwiegend serieller kritischer Scheduling-Span
```

Die v56-Zahlen wurden in unterschiedlichen Zaehldomaenen ausgegeben. Das
Per-Function-Budget von `65.536` darf daher weder von den laufweiten `27.872`
physischen Auswertungen subtrahiert noch durch die laufweiten `25.728`
Contexts dividiert werden. Diese Werte bleiben historische Evidenz und
werden nicht als gemeinsamer Root behauptet.

Eine blosse Erhoehung des 65.536er-Budgets, mehr Cache oder mehr Threads ist
kein Fix. Der aktuelle D-Lauf zeigt gegenueber dem vorherigen Fehlerlauf bei
gleicher Gesamtzeit (~459,6 s) hoeheren Durchsatz (`wave 103` statt `67`,
`1.044` statt `722` Lanes, `1.029` statt `713` contextual physical
evaluations, `733` statt `839` stale requeues und `518.425.788` statt
`444.266.838` B Cache-Payload), entfernt den semantischen Lane-Treiber aber
nicht. Bei Attempts `1024`, `2048` und `4096` waren die relevanten
Admission-/Stack-Diagnosezaehler bitgenau identisch. Candidate-Resolution
bleibt deshalb offen; KR-4981 ist nicht bestanden.

Der Detailvertrag steht in
[`docs/P0_ROOT0_CANDIDATE_RESOLUTION_PERFORMANCE.md`](docs/P0_ROOT0_CANDIDATE_RESOLUTION_PERFORMANCE.md),
der uebergeordnete Kaltbuildvertrag in
[`docs/P0_NATIVE_DISC_COLD_BUILD_PERFORMANCE.md`](docs/P0_NATIVE_DISC_COLD_BUILD_PERFORMANCE.md).

## Aktueller Taskpfad

| ID | Aufgabe | Ergebnis |
|---|---|---|
| KR-4985 | Candidate-Resolution-Phasen- und Kardinalitaetstelemetrie | [x] source-seitig abgeschlossen; produktive D1-Telemetrie explizit opt-in, Produktgate wegen unvollstaendigem Lauf unentschieden |
| KR-4986 | Semantische Context-Lanes und exakte Provenienzabonnenten | [x] source-seitig abgeschlossen; Full-State-Semantik und exakte Contribution-/Evidence-Provenienz getrennt |
| KR-4987 | Read-Lens-projizierte Context-Identitaet | [x] source-seitig abgeschlossen; vollstaendige Key-Bytes, konservativer FullState-Fallback und exakte Provenienz/Restore; D9 beendet fail-closed, kein Erfolg behauptet |
| KR-4988 | Internierte AbstractStates und Summaries | nur bei positivem Kostengate werden unveraenderliche States/Summaries kanonisch wiederverwendet |
| KR-4989 | Indexierte exakte Context-Bindings | nur bei positivem Kostengate vermeiden exakte Treffer den linearen Scan |
| KR-4990 | Inkrementelle Contextual-Dependency-Views | nur bei positivem Kosten-/Reusegate werden unveraenderte View-Shards behalten |
| KR-4991 | Versionierte monotone Context-Worklist | nur bei positivem G2 startet kausal freigesetzte Arbeit ohne globale Jacobi-Barriere |
| KR-4993 | Abschlussreview der Candidate-Resolution-Pfade | [x] vollstaendiger Source-Endreview wiederverwendet; das Analyzer-ABI-Finding wurde im vorherigen Fixcommit mit ABI 32 geschlossen, Produktlimits bleiben KR-4981 vorbehalten |
| KR-4981 | Einmaliges Sonic-Produktzeitgate | globales Produktgate; der aktuelle D-Lauf bestand es nicht; kein weiterer Lauf ohne ausdrueckliche Freigabe; vollstaendiger 24-Thread-Kaltport und realer Lauf |
| KR-4992 | Begrenzte Spekulation spaeterer Roots | nur nach einem verfehlten KR-4981 und positivem Restkosten-/RAM-Gate |
| KR-4994 | Begrenzter identitaetserhaltender unresolved Stack-/Context-Candidate-Carrier | [x] source-seitig abgeschlossen; begrenzter Pending-Carrier plus kanonisches absorbierendes Top fuer abgeschnittene Candidate-Domains ueber Merge/Normalisierung/Key/Persistenz/ABI-Promotion und Harvest; der semantische Lane-Treiber bleibt im Produktgate offen |

Die Reihenfolge ist normativ:

```text
KR-4985/KR-4986/KR-4993/KR-4987/KR-4994 source-seitig abgeschlossen
  -> D-Lauf beendet durch manuelles Beenden nach belegter Nichtverbesserung
  -> Candidate-Resolution bleibt offen; KR-4981 ist nicht bestanden
```

D1 und D2 sind reale, begrenzte Sonic-Diagnoseexporte, keine neue Testmatrix.
D1/G1 bleibt historisch unentschieden; D2/G2 wurde nicht ausgefuehrt. D9 ist
beendet und Root 0 konvergierte fail-closed ohne Portartefakt oder
Produkterfolg. KR-4988 bis KR-4991 bleiben inaktiv. KR-4994 ist source-seitig
abgeschlossen; der semantische Lane-Treiber bleibt offen. KR-4981 bleibt das
globale Produktgate und ist nicht bestanden. KR-4982 und KR-4983 bleiben
gestrichen.

## Weiterer v0.49-Kritischer Pfad

Nach erfolgreicher Candidate-Resolution gilt:

1. **NativeDisc-AOT-Produktnachweis**
   - aktueller Port wird vollstaendig erzeugt;
   - bekannte historische AOT-Grenzen werden durch statisches natives AOT
     passiert oder durch einen engeren typisierten Blocker ersetzt;
   - kein Interpreter, JIT oder Runtime-Dekoder uebernimmt fehlenden Code.

2. **Produktgate und sichtbarer Fortschritt**
   - relative Post-Entry-Gastarbeit;
   - typisierte Fehlercodes;
   - echter sichtbarer Frame statt technischer Hilfsmetrik.

3. **CompletePlatform-/DirectBoot-Paritaet**
   - frischer ABI-passender Handoff;
   - atomarer Prepare-/Commitvertrag;
   - VMU-/Save-Autoritaet bleibt erhalten;
   - NativeDisc und DirectBoot stimmen am Game Entry normativ ueberein.

4. **Externes Spielprojekt und inkrementeller Build**
   - generierter Titelcode und Originaldaten bleiben lokal;
   - Runtime-only- und Hook-only-Aenderungen vermeiden unnoetigen
     SH-4-Neuexport;
   - kein Sonic-Sonderfall im Katana-Kern.

5. **Xenon-artiger Hotpath**
   - statisches AOT ohne Materializerarbeit im Normalfall;
   - direkte native Calls und Returns ueber bewiesene Grenzen;
   - IR-basierte Registerlokalisierung;
   - ereignisgetriebene Safepoints;
   - mindestens 200 MHz, Zielreserve 250 MHz unpaced.

## Produktmeilensteine

### B0 - Game Entry korrekt

- Haupt-Executable aus der eigenen GDI identifiziert und hashgebunden;
- vollstaendiger Post-IP.BIN-Handoff;
- NativeDisc und DirectBoot besitzen gleiche normative Subsystemdigests.

### B1 - Soundworker fortgeschritten

- Gastwriter beziehungsweise engerer allgemeiner Blocker belegt;
- kein direktes Hostsetzen eines Completion-Flags;
- AICA-/G2-/DMA-/IRQ-/Schedulerkante bleibt typisiert.

### B2 - Sichtbares Spielbild

- Haupt-Executable erzeugt Direct-FB- oder TA-Ausgabe;
- aktiver Scanout liest den echten Gastbereich;
- Host praesentiert den Frame;
- ein uniformer Clear- oder Fehlerframe gilt nicht als Spielfortschritt.

### B3 - Echtzeit

- mindestens 200 MHz effektive Gastgeschwindigkeit;
- Zielreserve mindestens 250 MHz unpaced;
- billige Produktdiagnose verwendet denselben schnellen Pfad.

### B4 - Titelbild und Eingabe

- Titelbild oder erster interaktiver Spielscreen;
- Controller im real gestarteten Spiel;
- mehrminuetiger stabiler Lauf.

## Arbeitsregeln

- Jeder Task folgt dem Dreischritt Implementierung, Review der betroffenen
  Pfade mit unmittelbarer Findingschliessung, Push auf `main`.
- Fehlende neue Tests sind kein Finding.
- Keine neue breite oder schmale Testsuite, keine Matrix und kein
  synthetisches Ersatzgate.
- Vorhandene Tests werden nur repariert, wenn sie selbst konkret falsch oder
  gebrochen sind.
- Sonic-Produktlaeufe folgen an den dokumentierten Gates oder nach
  ausdruecklicher Nutzeranweisung.
- Keine Controller-, GUI-, Paketierungs- oder Komfortarbeit vor B2.
- Kein Hardwareausbau auf Verdacht.
- Adressen aus dem Bring-up duerfen dokumentiert werden, aber nie
  titelbezogene Sonderfaelle im generischen Code erzeugen.

## Nicht auf dem aktuellen P0-Pfad

- vollstaendige Dreamcast-Kompatibilitaet fuer weitere Titel;
- umfassendes Replay jeder Gastinstruktion;
- neue PVR-/AICA-Features ohne Sonic-Produktbefund;
- GPU-Offload der Analyse;
- GUI-Politur;
- oeffentliche Releasepaketierung;
- neue Test-, Konformitaets- oder Threadmatrizen;
- weitere Controller-Haertung.

## v0.49 Definition of Done

`v0.49.0` ist erst abgeschlossen, wenn:

- Recompiler, Runtime und externes Spielprojekt getrennt gebaut werden
  koennen;
- die Haupt-Executable aus der Original-GDI lokal installiert wird;
- Candidate-Resolution ohne Context-/Evaluationslimit und ohne
  `incomplete-root` abschliesst;
- DirectBoot einen vollstaendigen, produktsicheren Post-IP.BIN-Handoff nutzt;
- Soundfortschritt und erster sichtbarer Spiel-/TA-Frame erreicht werden;
- NativeDisc und DirectBoot ab Game Entry normativ uebereinstimmen;
- der normale Produktport mindestens 200 MHz erreicht;
- VMU/Saves durch den Handoff nicht zurueckgesetzt werden;
- keine Sonic-Sonderfaelle oder Retailbytes im generischen Katana-Kern
  liegen;
- keine neue Testsuite oder Testmatrix den Sonic-Produktnachweis ersetzt.
