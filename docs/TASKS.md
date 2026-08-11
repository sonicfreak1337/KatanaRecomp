# KatanaRecomp Task-Katalog

Dieses Dokument enthaelt die aktiven `v0.49`-Produktaufgaben. Historische
Aufgaben und fruehere Detailstaende bleiben in Git und in
`TASK_ID_REGISTRY.md` nachvollziehbar.

## Repositoryweiter Taskvertrag

Fuer jeden Task gilt ohne zusaetzliche Zwischenstufe:

```text
Task implementieren
  -> alle betroffenen Pfade reviewen
     und bestaetigte Fehler innerhalb des Reviews schliessen
  -> den reviewten Task direkt auf main committen und pushen
  -> naechster Task
```

Verbindlich ist dabei:

- `AGENTS.md` gilt fuer jeden Task und jeden automatisierten Bearbeiter;
- ein Task wird direkt auf `main` bearbeitet und gepusht;
- Branches oder Pull Requests entstehen nur auf ausdrueckliche
  Nutzeranweisung;
- die Reviewstufe umfasst Implementierung, Aufrufer, Verbraucher, Datenfluss,
  Verdrahtung, Fehlerpfade, ABI-, Cache-, Versions-, AOT-, Runtime- und
  Produktvertraege der Aenderung;
- bestaetigte Fehler im Taskscope werden vor dem Push geschlossen;
- es gibt keine separate standardmaessige Test-, Verifikations-, Fix- oder
  Integrationsrunde zwischen Review und Push;
- erst der Push des reviewten Tasks gibt den naechsten Task frei;
- der Push ist die Freigabe; der naechste ungegatete Task benoetigt keine
  weitere Nutzeranweisung;
- ein Review darf ausserhalb des Taskscopes liegende Beobachtungen notieren,
  daraus aber nicht eigenmaechtig neue Tasks oder Scope ableiten.

## Sonic ist der Test

Oberste Prioritaet ist ein lauffaehiger Sonic-Adventure-PAL-Produktport. Der
reale Sonic-Port ist der massgebliche Produkt- und Integrationstest:

```text
Export -> Installation aus der Originaldisc -> normaler Lauf ->
sichtbarer Fortschritt
```

Daher gilt projektweit:

- keine neuen Unit-Tests, Regressionstests, Testmatrizen, synthetischen
  Fixtures, Stresslaeufe, Testprojekte, Ersatzgates oder
  Konformitaetssuiten;
- das Fehlen neuer Tests ist kein Review-Befund;
- Reviews verlangen keine neue Testabdeckung als Abschlussbedingung;
- vorhandene Tests duerfen auf gebrochene Erwartungen, falsche Testzahlen
  oder widerspruechliche Semantik geprueft und bei Bedarf repariert werden,
  ihr Bestand wird aber nicht erweitert;
- ein Task startet keinen eigenen Testbuild und keine Matrix als Pushgate;
- Sonic-Laeufe erfolgen an den unten festgelegten Produktgates oder auf eine
  ausdrueckliche Nutzeranweisung, nicht nach jedem Task;
- mehrere zusammenhaengende, reviewte Tasks duerfen vor dem naechsten
  Produktlauf auf `main` landen;
- DirectBoot besitzt keinen Sega-Screen als Pflichtmeilenstein, weil dieses
  Bild zu IP.BIN gehoert;
- kein Interpreter, JIT oder Emulationsfallback im normalen Produktpfad;
- keine Sonic-Adressen, Titelhooks, Retailbytes oder kommerziellen Inhalte im
  generischen Katana-Code.

## Lauf- und Ressourcenvertrag

- Kein Prozess und keine einzelne Phase laeuft laenger als 15 Minuten, ausser
  der Nutzer hebt die Grenze fuer genau einen benannten Lauf auf.
- Jeder potenziell lange Prozess besitzt spaetestens alle zehn Sekunden einen
  belastbaren Fortschrittsindikator.
- Liveness ohne kanonischen Fortschritt ist kein Erfolg. Stalls und
  Nichtkonvergenz werden nach dem Vertrag in `AGENTS.md` beendet.
- Das Per-Function-Budget von `65.536` Contextual-Return-Evaluationen wird
  nicht als Performancefix erhoeht. Der aktuelle P0 muss durch weniger
  notwendige Arbeit geschlossen werden.

## Verbindlicher v0.49.1-Native-Portpfad

Der Produktport ist kein Emulator. Statisches SH-4-AOT wird an validierten
Spiel-/SDK-Grenzen mit nativer PC-Grafik, -Audio/-Movie, -Datei-, -Eingabe-
und Savefunktion verbunden. ARM7-Interpreter, CPU-PVR-Softwarerasterizer und
vollstaendige Dreamcast-Geraetemodelle duerfen nicht in das Produktbinary
gelangen. Details: `NATIVE_PORT_PRODUCT_CONTRACT.md`.

```text
KR-5000  native Produktgrenze und Linkisolation
  -> KR-5001  private statische Spiel-/SDK-Hookkarte
  -> KR-5002  nativer Audio-/Moviepfad
  -> KR-5003  nativer GPU-Pfad
  -> KR-5004  native Disc-/Eingabe-/Save-Dienste
  -> KR-5005  No-Skip-Sonic rein nativ bis Hauptmenue; v0.50.0 Alpha
```

`001f3c2` und `24,2926 MHz` bleiben historische Bring-up-Evidenz fuer AOT-
Abdeckung, Adressen und den erwarteten Lebenszyklus. Interpreter- und
Softwarerasterizeroptimierung gehoeren nicht mehr zum aktiven P0. Dreamcast-
MHz sind kein Produkt- oder Versionsgate des nativen Ports.

## [ ] KR-5000 - Native Produktgrenze und Linkisolation

Prioritaet: P0 Architektur

Status: aktiv. Der Dokumentationsvertrag ist gesetzt; Buildprofil,
Linkgrenze und typisierter Missing-Native-Hook-Fehler sind noch umzusetzen.

Abschluss: Ein `native-port`-Artefakt kann keine ARM7-/SkyEmu-, CPU-PVR- oder
Diagnoseinterpreter-Symbole linken und besitzt keinen Laufzeitfallback auf
diese Pfade.

## [ ] KR-5001 - Statische Spiel-/SDK-Hookkarte

Prioritaet: P0 Bring-up

Status: vorbereitet durch die private Adresskarte. Audio/Movie wird vor dem
AICA-Kommandoring und Grafik vor dem PVR-Geraeteprotokoll gebunden; die
hoechste belegbare Grenze gewinnt. Private Adressen bleiben ausserhalb des
Repositorys.

## [ ] KR-5002 - Nativer Audio-/Moviepfad

Prioritaet: P0 Produkt

Status: wartet auf KR-5001. Kein ARM7- oder AICA-Firmwarepfad im Produkt;
Opening und Ton laufen vollstaendig ueber native Hostdienste, ohne Skip oder
erzwungenen Playerstatus.

## [ ] KR-5003 - Nativer GPU-Pfad

Prioritaet: P0 Produkt

Status: wartet auf KR-5001. Renderarbeit laeuft ueber eine native GPU-API;
der CPU-PVR-Softwarerasterizer ist aus dem Produktlink entfernt. Diese Aufgabe
ist kein optionaler GPU-Offload des alten Emulationspfads.

## [ ] KR-5004 - Native Disc-, Eingabe- und Save-Dienste

Prioritaet: P0 Produkt

Status: wartet auf KR-5001. Originaldaten bleiben lokal; Dateizugriff,
Controller und atomare Speicherstaende verwenden native PC-Dienste.

## [ ] KR-5005 - Nativer No-Skip-Sonic-Produktlauf

Prioritaet: P0 Alpha-Gate

Status: wartet auf KR-5000 bis KR-5004. Abnahme: korrektes Opening mit Bild
und Ton, 60-Hz-PAL-Pfad, Memory-Card-Screen und Hauptmenue ueber denselben
rein nativen Pfad sowie native Eingabe. Erst dann wird `v0.50.0 Alpha`
freigegeben.

## Historischer RuntimeOnly-Bring-up

Der opt-in Modus `port --analysis-mode runtime-only` ist nur fuer den
vollstaendigen NativeDisc-Produktport mit `--game-project` zulaessig; der
Default bleibt `platform`. RuntimeOnly setzt `GuestCallAbi::Unknown`, umgeht
die blockierende SuperHC-FunctionValue-/Candidate-Resolution, erzeugt nativen
AOT-Code und nutzt RuntimeOnly-Dispatch ueber eine exakte statische
Guest->Host-Tabelle. Stop-on-miss und typed abort bleiben aktiv; kein
Interpreter, JIT, Runtime-Decoder oder geratener Zielpfad. Der Whole-Export-
Cache ist modegebunden.

Der aktuelle RuntimeOnly-Build-/Export-Unterauftrag und der natuerliche
No-Skip-Audio-/Videopfad bis `FirstVisibleGameFrame` sind abgeschlossen.
Der letzte Lauf brachte `341` Renderrequests/-completions/-frames, `15.680`
YUV-Makrobloecke und `470` Audiopuffer mit `345.450` Audiobildern. Die
identische Vergleichsreihe stieg von `23,7959 MHz` ueber `24,1885 MHz` und
`24,2825 MHz` auf `24,2926 MHz` (`+0,4967 MHz`, `+2,09 %`).
`100 MHz`, der fail-closed Identity-Miss `0x8C054008 -> 0x8C9000E8` und
Memory-Card-Screen/Hauptmenue bleiben offen; der PlatformAbi-Default bleibt
erhalten.

## Historischer RuntimeOnly-Produktstand

Der vorherige bereinigte Source-Checkpoint hob Runtime-ABI `87` auf `88` und
PlatformServices-ABI `13` auf `14`. Der funktionale Checkpoint `efc531b` hebt
Runtime-ABI wegen der PVR-Completion- und TA-Metrikvertraege weiter auf `89`;
`e1d8ade` hebt ihn wegen der oeffentlichen AICA-/ARM7-Fortsetzung weiter auf
`90`; Backend-Interface-ABI `13` und PVR-State-Contract `3` bleiben aktuell.

Die PVR-Fullevidenz endete nach vier bewiesenen Frames mit `1.228.800`
geaenderten Pixeln; der Audiohash `8399287713367543391` blieb zwischen
YUV-Lauf und Audio-Umbau identisch. Der Hostprozess nutzte nur etwa `1,64`
Kerne beziehungsweise `6,8 %` der 24-Thread-Kapazitaet; der aktuelle P0 ist
der serielle Runtime-/Dispatch-Overhead bis mindestens `100 MHz`.

## Historische Candidate-Evidenz

```text
letzte reale Produktevidenz:
  historische NativeDisc-/DirectBoot-Ports mit aelteren ABI-Vertraegen

Aktueller funktionaler Source-Stand:
  aktueller Runtime-Performance-Checkpoint
  Runtime-ABI 90, PlatformServices-ABI 14, Backend-Interface-ABI 13,
  PVR-State-Contract 3
  Analyzer-ABI 34, Function-Analysis-Epoch-Schema 27,
  lokales In-Process-Evaluation-Cache-Schema 13
  Native-AOT-Emissionsprofil 25, AOT-Partitionsschema 5

historischer Diagnosebefund:
  Sonic-v56 endete nach 1:28:24 mit Exitcode 5
  1/1191 Roots committed
  65.536 Contextual-Return-Evaluationen einer Funktion ausgeschoepft
  Context-Limit nicht erreicht
  25.728 eindeutige Contexts
  27.872 physische Auswertungen
  0 Eviction-Recomputes
  Epoch-Retention: incomplete-root
  kein Portartefakt aus diesem alten PlatformAbi-Lauf, keine game.exe
```

Der v56-Lauf ist Diagnoseevidenz, kein Produktnachweis. Historische Ports sind
kein Beweis fuer den aktuellen Source. Die `65.536` Evaluationen sind ein
Per-Function-Budget, `25.728` Contexts und `27.872` physische Auswertungen
sind laufweite Aggregate. Bis KR-4985 dieselben Root-, Funktions- und
Zaehlscope-Dimensionen ausgibt, duerfen daraus weder `37.664` vermeintlich
physiklose Evaluationen noch `2,547` logische Evaluationen je Context als
Messwert abgeleitet werden.

## Historische RuntimeOnly-Reihenfolge

```text
KR-4985, KR-4986, KR-4993, KR-4987, KR-4994 und KR-4995: source-seitig abgeschlossen
  -> RuntimeOnly-Build-/Export-Gate bestanden
  -> No-Skip-Sonic-Audio-/Videopfad bis FirstVisibleGameFrame, 24,2926 MHz
  -> historische RuntimeOnly-Performancezielmarke mindestens 100 MHz
  -> post-filmischen Identity-Miss 0x8C054008 -> 0x8C9000E8 schliessen
  -> sichtbarer Start bis mindestens Memory-Card-Screen/Hauptmenue
```

Jeder Task in dieser Kette folgt einzeln:

```text
implementieren -> betroffene Pfade reviewen und Findings schliessen -> main
```

D1 und D2 sind begrenzte reale Sonic-Diagnoseexporte, keine Testmatrix. Der
einzige freigegebene D1-Lauf war nichtterminal; D1/G1 bleibt fail-closed und
unentschieden. D2/G2 ist abgeschlossen und negativ, ohne positiven
Schedulerhebel. D9 ist historisch beendet und Root 0 konvergierte fail-closed
ohne Portartefakt oder Produkterfolg. KR-4988 bis KR-4991 bleiben inaktiv;
KR-4994 und KR-4995 sind source-seitig abgeschlossen. Die PlatformAbi-Candidate-Resolution
bleibt deferred und ist kein RuntimeOnly-Buildblocker. KR-4981 bleibt als
historisches RuntimeOnly-Gate dokumentiert und wird durch das native Alpha-
Gate KR-5005 abgeloest. KR-4982 und KR-4983 bleiben als alte optionale
Offload-Aufgaben gestrichen; der native GPU-Produktpfad ist die neue,
semantisch getrennte Aufgabe KR-5003.

---

## [ ] KR-4972 - Hashgebundene Shared-Callback-/Thunk-AOT-Coverage

Prioritaet: P0 Boot

Status: Der allgemeine Guarded-AOT-Entry- und
Exportvollstaendigkeitsvertrag ist quellseitig vorhanden. Fuer den historischen
PlatformAbi-Pfad fehlt ein Produktnachweis, weil Candidate-Resolution vor dem
Portexport endet; der aktuelle RuntimeOnly-Build-/Export-Gate ist separat
bestanden und erreichte den ersten sichtbaren SEGA-Screen. Wegen des
anschliessenden fail-closed `missing-aot` bleibt die vollstaendige AOT-Coverage
und damit dieser Task offen.

### Noch offen

- Candidate-Resolution ohne Context-/Evaluationslimit und ohne
  `incomplete-root` abschliessen;
- danach die bekannten historischen indirekten Ziele durch validiertes
  statisches AOT passieren oder einen engeren typisierten Blocker belegen;
- keine titelbezogene Adresse als generischen Fix verwenden;
- kein Interpreter, JIT, Runtime-Dekoder oder Emulationsfallback.

### Abschluss

Der Task bleibt bis zum erfolgreichen KR-4981-Produktlauf offen. Die
Quellpfade werden in den Tasks KR-4985 bis KR-4993 geschlossen und reviewt.

---

## [ ] KR-4979 - Priorisierter Analyseexecutor und Speicherhaushalt

Prioritaet: P0 Performance

Status: Der gemeinsame Executor ist quellseitig implementiert. Die historische
v56-Ausgabe besass noch keinen gemeinsamen Scope fuer Contextidentitaet,
Wiederzulassung, Per-Context-Kosten und kritischen Span; die Source-Trennung
ist durch KR-4985/KR-4986 abgeschlossen, waehrend D1/G1 unentschieden bleibt.

### Abschlussbedingungen

- kein Root endet am Contextual-Evaluationslimit oder an `incomplete-root`;
- reale unabhaengige Arbeit nutzt den Executor, echte Breite-1-Ketten werden
  als kritischer Span und nicht als Executorfehler berichtet;
- RAM-Druck erzeugt kein semantisches Truncation;
- Produktwirkung wird in KR-4981 am Sonic-Port gemessen.

---

## [ ] KR-4980 - Schichtweiser persistenter NativeDisc-Buildcache

Prioritaet: P0 Performance

Status: Quellseitig implementiert. v56 belegt, dass weitere Cachekapazitaet
ohne belegte Eviction-Recomputes keine begruendete Hauptloesung ist. Die
historische v56-Ausgabe besass noch keinen gemeinsamen Scope fuer die
Budgetzaehldomaene; KR-4985 ist abgeschlossen, D1/G1 bleibt unentschieden.

### Abschlussbedingungen

- fehlende, alte oder beschaedigte Shards bleiben sichere Misses;
- lokale Aenderungen invalidieren nur semantisch gebundene Ebenen;
- Produktwirkung und Warmexportzeit werden erst im Sonic-Produktpfad
  bewertet.

---

## [x] KR-4985 - Candidate-Resolution-Phasen- und Kardinalitaetstelemetrie

Prioritaet: P0 Performance-Diagnose

Abhaengigkeiten: KR-4974, historischer funktionaler Source-Checkpoint
`099ae2cb2dfe7699f90338e9df0bad24a7888823`

Status: Source-seitig abgeschlossen durch den gemeinsamen Candidate-Resolution-
Explosionsfix. D1-Telemetrie ist explizit opt-in produktiv; die begrenzte
Produktevidenz blieb wegen vorzeitigem Supervisor-Abbruch unvollstaendig und
entscheidet weder D1/G1 positiv noch negativ.

### Ziel

Den limitierenden Root und die limitierte Funktion identifizieren und die
Kosten von Snapshot, Cache-Key, physischer Auswertung, `apply_call()`, Merge,
Evidence und Commit getrennt sichtbar machen.

### Umfang

- Rootindex, Rootadresse, Funktionsadresse, Wellenindex und Frontierbreite;
- logische Evaluationen, physische Auswertungen, Cache-Reuse ohne physische
  Arbeit, Context- und Evaluationsbudgets;
- neue, verbreiterte und erneut zugelassene Lanes;
- Requeue-Ursachen getrennt nach initial root, neuer exakter Lane,
  Input-Widening, Summary-Aenderung, Forward-Edge-Insert/Widening und stale
  Dependency-Version;
- Snapshot-, Key-, Auswertungs-, Merge-, Evidence- und Commitzeit;
- Bindingzahl, Hitposition, Equality-/Copy-/Mergearbeit und Stategroesse;
- aggregierte Full-State-, Projected-Lens- und Provenienz-Digests;
- keine Rohstates, Gastwerte oder per-Lane-Retaillogs.
- D1-sichere Resultatannahme: Dependency-/Snapshot-Version wird vor
  `item.error` und vor jeder terminalen Publikation geprueft; ein veraltetes
  Batchresultat wird verworfen und gezielt neu eingeplant, statt den Root
  mit seinem inzwischen gegenstandslosen Fehler zu beenden;
- Cancellation-, Fehler- und Stale-Reihenfolge in allen Resultatpfaden.

### Review- und Abschlussvertrag

- Telemetrie darf die kanonische Semantik nicht veraendern;
- Drop-, Vollstaendigkeits- und Budgetpfade werden im Quellreview verfolgt;
- stale oder gecancelte Resultate koennen weder Summary/Evidence publizieren
  noch ueber `item.error` einen aktuellen Root terminal beenden;
- D1 wurde einmal freigegeben, erreichte aber weder den historisch
  limitierenden Root noch einen vollstaendigen schweren Root; D1/G1 bleibt
  daher unentschieden;
- Historisch wurden KR-4987 bis KR-4991 durch diesen unvollstaendigen Lauf nicht aktiviert;
- keine neue Telemetrie-Testmatrix oder synthetische Ersatzabnahme.

---

## [x] KR-4986 - Semantische Context-Lanes und exakte Provenienzabonnenten

Prioritaet: P0 Korrektheits-Enabler

Abhaengigkeit: KR-4985

Status: Source-seitig abgeschlossen durch den gemeinsamen Explosionsfix. Die
Full-State-Semantik bleibt autoritativ; exakte Provenienz wird getrennt
replayed. Keine Read-Lens-Aktivierung und keine reduzierte Analyseabdeckung
wurden daraus abgeleitet.

### Ziel

Physische Fixpunktsemantik von exakten Contributions, Callsites, Callees und
Evidence-Tokens trennen, zunaechst mit unveraenderter Full-State-Identitaet.

### Umfang

- semantische Lane plus kanonische Menge exakter Provenienzabonnenten;
- stabiler Full-State-Key aus Funktion, Evaluation-Lens, Contract und
  Ingress-Content;
- Summary- und Dependency-Versionen dienen der Stale-Validierung, nicht der
  Erzeugung immer neuer Lane-Identitaeten;
- getrenntes Accounting fuer logische Requests, semantisch eindeutige Lanes,
  physische Auswertungen und Provenienzlinks;
- exaktes Replay isolierter, forwarded und rootkorrelierter Evidence.

### Review- und Abschlussvertrag

- normalisierte Summaries, Resolutions, Guarded Inventory, Evidence und
  Budgetflags bleiben unveraendert;
- unvollstaendige Rootarbeit wird nie als wiederverwendbare Epoche
  publiziert;
- keine Read-Lens-Projektion vor belegter Full-State-Paritaet im Review;
- keine neue Worker-, Cache- oder Differentialtestmatrix.

---

## [x] KR-4987 - Read-Lens-projizierte Context-Identitaet

Prioritaet: P0 Performance

Abhaengigkeit: KR-4986; source-seitige Aktivierung durch den freigegebenen
KR-4987-Fixpass

Status: Source-seitig abgeschlossen am historischen funktionalen Source-Checkpoint
`099ae2cb2dfe7699f90338e9df0bad24a7888823`; Analyzer-ABI `34`, Function-Analysis-Epoch-Schema `19`.
Der gezielte `katana-recomp`-Build war laut Sol-Review in `42,4 s` erfolgreich.
D9 ist beendet und wird nur als fail-closed, nicht erfolgreicher Lauf
dokumentiert; kein Produkt- oder G1-Erfolg wird behauptet.

D9-Beobachtung: `20,331 s`, Root 0 bis Wave `184`, Frontier `0` (maximal
`216`), `288` admitted contexts, `2.724` admitted evaluations/Semantic-Lanes,
`4.349` logical requests, `3.739` physical evaluations, `2.497` input-
widening und `932` stale-dependency requeues, `1.740` stale snapshot
discards, `939` semantic und `2.377` provenance-only widenings. Der Lauf
endete fail-closed am unvollstaendigen Root; kein Portartefakt und kein
Produkt-, G1- oder Limit-Erfolg wird behauptet.

Der korrekte VsDevCmd-Incremental-Build von `katana-recomp --parallel 12`
war in `42,8 s` erfolgreich; es blieben nur bekannte getenv-/Shadowing-
Warnungen. Frueherer D-Lauf: `460,6 s` gesamt, Candidate Resolution ca. `325,8 s`,
manuelles Beenden des identifizierten Kindprozesses nach belegter
Nichtverbesserung, `0/1194` committed Roots, HOL `0`, Wave `103`, `272`
Contexts, `1.044` Semantic-Lanes, `1.029` contextual physical evaluations,
`2.430` contextual logical requests, `1.359` Input-Widening-, `29` Summary-
und `733` stale-Dependency-Requeues, `1.359` stale snapshot discards,
`518.425.788 B` Cache-Payload, `3.964` physische Auswertungen gesamt und
`0/0` publizierte/verwarfene Epochen. Context-/Evaluation-/Composite-Budgets
blieben unverbraucht; kein Portartefakt oder `game.exe`.
Attempts `1024`, `2048` und `4096` hatten bitgenau gleiche relevante
Admission-/Stack-Diagnostik wie der vorherige Fehlerlauf; die Rohwerte sind
wegen der unterschiedlichen Endpunkte nicht direkt vergleichbar und belegen
keine materielle Produkt-/Performanceverbesserung oder einen Konvergenzhebel.
KR-4981 bleibt offen; der historische PlatformAbi-P0 war
Inventory-Provenance-Live-in/Spill-through. Dieser Befund ist kein aktueller
RuntimeOnly-Buildblocker.

### Ziel

Contexts vor der Lane-Erzeugung zusammenlegen, wenn der konkrete Callee
vollstaendig bewiesen denselben semantischen Eingang liest und sich die
Inputs nur in ungelesenem State oder Provenienz unterscheiden.

### Umfang

- SemanticContextKey aus Funktion, Lens, Contract-Fingerprint und
  projiziertem Ingress;
- vollstaendige Key-Bytes fuer kollisionssichere SemanticLane-Identitaet;
- vollstaendige Register-, Stack-, Memory-, Alias-, Output-, Inventory- und
  Fallback-Watcher;
- zwingender FullState-Fallback bei jeder Vertragsluecke;
- FullState bei Truncation und Read-Lens-Fallback;
- Rebucketing bei spaet erweitertem Read-Vertrag;
- stabile Content-Digests plus strukturelle Kollisionspruefung;
- Provenienz bleibt ausserhalb der semantischen Lane-Identitaet.

### Review- und Abschlussvertrag

- gelesene Unterschiede bleiben getrennt, ungelesene duerfen teilen;
- Completeness, Truncation und exakte Evidence bleiben erhalten;
- die Reduktion erfolgt vor Lane-Erzeugung und nicht erst durch einen
  spaeten Cachetreffer;
- exakte Provenienz und Restore sowie Discovery -> Freeze -> Publish bleiben
  unveraendert;
- D9 ist beendet und ersetzt keine Produktabnahme, D2/G2 oder KR-4981.

---

## [x] KR-4994 - Begrenzter identitaetserhaltender unresolved Stack-/Context-Candidate-Carrier

Prioritaet: P0 Candidate-Resolution-Korrektheit

Status: Source-seitig abgeschlossen am historischen funktionalen
Source-Checkpoint `099ae2cb2dfe7699f90338e9df0bad24a7888823`; der historische
Candidate-Resolution-Stand ist `49b0f72a9f49d60a4eb6e0481460cd57c5625735` mit Analyzer-ABI `34`,
Function-Analysis-Epoch-Schema `27` und lokalem In-Process-Evaluation-Cache-
Schema `13`. Der begrenzte Pending-Carrier
bewahrt identitaetsgebundene Payloads ueber Merge, Key/Cache, Lifetime,
ABI-/Summary-Propagation, Stack-may-load, Candidate-Recompute und
contextual/forwarded/stable Harvest sowie Export-Gate. Der historische
Evidence-/Semantic-Zweikanal haelt Evidence-Stale in privaten Replaykapseln;
abgeschnittene begrenzte Candidate-Domains sind als kanonisches absorbierendes
Top mit leerem endlichem Praefix in Merge, Normalisierung, Vergleich, Keys,
Persistenz, Consumern und ABI-Promotion konsistent. Der historische D-Lauf
belegt als PlatformAbi-P0 die fehlende Wirksamkeit der autoritativen Hybrid-Join-Closure
beim vollstaendigen Stackvertrag/Gate.

Der Lauf `kr4981-20260809-020628-2bfd8af5` endete nach `343,627 s` durch
manuellen Abbruch bei identischer Nichtkonvergenz; letzte Bewegung Wave `48`,
Peak Root `1.450.078.208 B`, Peak Job `1.618.132.992 B`, keine Publikation und
kein Portartefakt. Bei Wave `39` waren die 16 geprueften Kernzaehler exakt wie
im Vorlauf (u. a. Frontier `177`, Contexts `272`, Semantic-Lanes `606`,
physisch `645`, exakte Subscriber `870`, Provenienz `21.355` und stale
Discards `299`). Der Fix ist ein Korrektheits-/Persistenzfix, kein belegter
Konvergenzhebel; KR-4981 bleibt offen.

### [x] Abgeschlossener Diagnose-Unterauftrag

Der Lauf `kr4981-20260809-024141-c4ffdf15` erreichte das vollständige
`attempts=1024`-Gate und wurde nach `244,549 s` bei Wave `24` gezielt beendet.
`uncategorized=0` für alle Top-8-Funktionen; kein Fehler, Hänger, Portartefakt
oder Produktgate. Der Hauptbefund `0x8C10E44E` umfasst `20` semantische
Änderungen und `40` Stack-Widenings ausschließlich SavedEpoch-pending-ABI-
Skalare (`92/80/20/20/20` für reg/stack/tail/state_stack/state_memory) bei
unvollständigem Callee-Set-Stackvertrag. `0x8C09859C` zeigt `28` gemischte
Änderungen mit demselben Vertragsgrund, `0x8C64E55E` `48` Änderungen bei
vollständigem Stackvertrag und `reg_epoch_pending=180`.

Dieser Diagnose-Unterauftrag ist abgeschlossen; KR-4981 bleibt offen. Der
SavedEpoch-Lifecycle-Fix ist source-seitig abgeschlossen. Offen bleibt die
gemeinsame Ordinary-/Registermetadaten-/Alias-/Watcher-/Loss-/MemoryEpoch-
Lifecycle-Ursache, mit erhaltenem Alias-/Current-Tracking und fail-closed
Restore. Die dynamischen Callee-Set-incomplete-Sites
werden danach erneut geprüft.

### [x] Abgeschlossener SavedEpoch-Lifecycle-Unterauftrag

Der Source-Fix konsumiert current-tracking SavedEpoch-Pending-ABI-Skalare nur
an bewiesenen normalen Call-/Tail-ABI-Gates und laesst detached Epochs
unangetastet. `candidate_payload_lost` ist als absorbierendes Epoch-Top ueber
Normalize, Merge, Equality, Key, Subsumption, Evidence, Restore und Persistenz
integriert; konkrete Evidence und Nested-/Current-Aliasfakten bleiben,
finite Payload/Slots verschwinden, detached Top erhaelt keine fremde
Tail-Evidence. Der historische SavedEpoch-Lifecycle-Stand lief unter
Epoch-Schema `17` und Analyzer-ABI `33`; kein oeffentliches Layout-Delta in
diesem historischen Fix.

Der Lauf `kr4981-20260809-031826-0616113a` endete nach `369,171 s` mit
`nonconvergence`/Exitcode `31` bei Wave `76`; `0` committed/ready/completed
Roots, `272` Contexts, `uncategorized=0` bei D1024 und D2048, keine Publikation
und kein `game.exe`. Der alte SavedEpoch-Pending-Blocker ist beseitigt. Der
naechste Root-Analysepunkt ist die gemeinsame Ordinary-/Registermetadaten-/
Alias-/Watcher-/Loss- und MemoryEpoch-Lifecycle-Ursache, nicht ein weiterer
SavedEpoch-Pending-Patch; KR-4981 bleibt offen.

### [x] Abgeschlossener 17-Source-Provenienz-/RTS-Unterauftrag und aktueller Laufstand

Der historische Candidate-Resolution-Source-Stand ist `49b0f72a9f49d60a4eb6e0481460cd57c5625735`.
Er erlaubt strukturelle Contextual-Hybrid-Projektion mit retained sticky loss;
die autoritative Hybridprojektion schliesst Contextual-MAY-Joins und Forward-
Edges erneut vollstaendig.
erkennt SavedEpoch-Slot-Pending-Top in allen Truncation-/Publication-Checks
fail-closed und trennt Provenance-Replay-Capsule-/Keybyte-Limits öffentlich
vom semantischen Evaluation-Limit. Ein echter Evaluation-Cap belastet nur den
Evaluation-Zähler; Analyzer-ABI `34`, Epoch-Schema `27` und lokales
In-Process-Evaluation-Cache-Schema `13` sind aktiv.

Der historische PlatformAbi-Produktlauf `kr4981-20260809-091410-2766aaa6` endete nach ca.
`275 s` gesamt (Candidate ca. `221 s`) mit `nonconvergence` nach drei
Amplifikationssamples: `0/1274` Roots, HOL `0`, Wave `107`, `280` Contexts,
`970` Semantic-Lanes, `1.861` physische, `2.526` logische Requests,
Input-Widening `536`, Summary `22`, Forward `123`, stale Requeues `272`,
stale Discards `806`, Cache `589.178.706 B`; keine Budgets erschöpft, keine
Publikation und kein Artefakt. Der Supervisor schrieb wegen `taskkill`-
Zugriffsverweigerung keine Summary; der Kill-on-close-Job beendete den Child
trotzdem. Admission `1024/1024`, projected context/match jeweils `0`.
`0x8C641202` blieb bei `84/84` Attempts/Semantic Changes und `508`
Ordinary-Stack-Deltas trotz vollständigem Stackvertrag.

Der vorherige Produktlauf `kr4981-20260809-083308-4a3ff9be` endete nach
`286,387 s` (Candidate ca. `232,5 s`) mit `nonconvergence`/Exit `31`:
`0/1274` Roots, Wave `119`, `280` Contexts, `972` Lanes, `2.011` physische,
`2.814` logische, `203` Cache-Reuses, `2.790` Subscriber, Provenienz `169.824`,
stale Discards `922`, Frontier `43` (max `250`), Cache `610.295.241 B`, kein
Artefakt. Admission `1024/1024`, projected context/match jeweils `0`; der
P0 liegt intra-context bei Ordinary-Stack und lokalen Stackkoordinaten.

Der vorherige Lauf `kr4981-20260809-041945-21b70ade` endete nach `425,924 s` (Candidate
ca. `341 s`) bei Wave `60`, `0/1194` Roots, `758` Lanes, `984` physischen und
`1.398` logischen Auswertungen, `248` Input-, `102` stale-Requeues und `347`
Discards; kein Portartefakt. Der Vergleichslauf
`kr4981-20260809-050420-3f47fd65` endete nach `322,632 s` (Candidate
`237,116 s`) wegen Nichtverbesserung bei Wave `39`, `0/1194` Roots,
`272` Contexts, `549` Lanes, `630` physischen, `894` logischen Auswertungen,
`181` Input-, `10` Summary-, `76` stale-Requeues, `226` Discards und
Provenienz `31.713`; kein `game.exe`. Das `attempts=1024`-Gate war gegenüber
`9baea88` bitgleich (`admission_success=999`, projected changed/match jeweils
`0`), also korrekt, aber kein Konvergenzhebel. Der offene P0 ist
die fehlende Wirksamkeit der autoritativen Hybrid-Join-Closure beim
vollstaendigen Stackvertrag/Gate. LocalStackCoordinate-/unvollstaendige
Stackvertraege bleiben sekundaer zu pruefen; keine Budget-/Thread-Erhoehung
und kein weiterer SavedEpoch-/Provenienzumbau.

Vertrag:

- einen strikt begrenzten, monotonen und identitaetsgebundenen unresolved
  Stack-/Context-Candidate-Carrier einfuehren;
- ihn vollstaendig in Merge, Key/Cache, Lifetime, ABI-/Summary-Propagation,
  Stack-may-load, Candidate-Recompute sowie contextual/forwarded/stable
  Harvest und Export-Gate integrieren;
- keinen Scheduler- oder Budgetumbau, keinen Fallback, keine Coverage-
  Reduktion und keinen Sonic-spezifischen Hack einfuehren;
- ein neuer Produktlauf bleibt bis zur naechsten ausdruecklichen Freigabe
  nach diesem Review gesperrt.

---

## [ ] KR-4988 - Internierte AbstractStates und Function-Value-Summaries

Prioritaet: bedingtes P1 Performance

Abhaengigkeiten: positives, durch D1 ermitteltes G1-Kostengate, KR-4986

### Ziel

Tiefe Equality-, Copy- und Keyarbeit an unveraenderlichen Snapshots durch
kanonische Identitaeten senken.

### Umfang

- StateId/SummaryId nur fuer immutable Snapshots;
- Content-Hash mit struktureller Kollisionspruefung;
- prozesslokale Identitaet getrennt von persistentem Content-Digest;
- gebundene Lebenszeit, RAM-Haushalt und Retentiontelemetrie.

### Review- und Abschlussvertrag

- mutable Fixpunktzustande werden nicht in-place interniert;
- persistente Keys verwenden nie nur prozesslokale IDs;
- kein falscher Equality-Fastpath und keine ungebundene Retention;
- D2 und Sonic bewerten die Wirkung, keine neue Interning-Testmatrix.

---

## [ ] KR-4989 - Indexierte exakte Context-Bindings

Prioritaet: bedingtes P1 Performance

Abhaengigkeiten: positives, durch D1 ermitteltes G1-Bindinggate, KR-4986

### Ziel

Exakte Binding-Treffer in grossen Familien direkt finden, ohne den
konservativen Join-/Subsumption-Fallback zu veraendern.

### Umfang

- StateId oder kollisionsgepruefter Fingerprint auf kanonische
  Binding-Indizes;
- vollstaendige Equality-Pruefung nach Indexlookup;
- atomare Indexpflege bei Insert, Widening und Generationwechsel;
- linearer Fallback bei Miss und fuer kleine Familien.

### Review- und Abschlussvertrag

- Kollision, Widening, Invalidation und Full-Miss bleiben korrekt;
- Merge-/Subsumptionssemantik bleibt unveraendert;
- D2 und Sonic bewerten die Wirkung, keine neue Binding-Matrix.

---

## [ ] KR-4990 - Inkrementelle Contextual-Dependency-Views

Prioritaet: bedingtes P1 Performance

Abhaengigkeiten: positives, durch D1 ermitteltes G1-Kosten-/Reusegate,
KR-4986

### Ziel

Unveraenderte Adjazenz, Bindings, Summary-Versionen und Evidence-Layouts
nicht bei jeder Reevaluation vollstaendig neu aufbauen.

### Umfang

- generationierte immutable Views nach Funktion/Lane, Edge, Binding und
  Summary-Aggregat;
- nur geaenderte View-Shards ersetzen;
- kanonischer inkrementeller Digest;
- Full-Rebuild bei unbekannter Invalidierung oder hoher Aenderungsrate;
- Referenzen nur in schreibfreier Phase, owning Materialisierung vor
  Besitzuebergang.

### Review- und Abschlussvertrag

- Add/Widen, Summary-only-Aenderung, stale Entfernung und Zyklen
  invalidieren exakt;
- globale und kontextuelle Fallbacks bleiben gebunden;
- D2 und Sonic bewerten die Wirkung, keine neue View-Differentialmatrix.

---

## [ ] KR-4991 - Versionierte monotone Context-Worklist

Prioritaet: bedingtes P0 Performance/Scheduling

Abhaengigkeiten: KR-4986, alle aktivierten KR-4987 bis KR-4990, D2 und
positives G2

### Ziel

Kausal freigesetzte unabhaengige Contextarbeit ohne globale
Jacobi-Wellenbarriere starten, ohne stale oder nichtkanonische Ergebnisse zu
publizieren.

### Umfang

- immutable Worker-Snapshot plus Dependency-Versionen;
- private semantische Deltas und versionierter kanonischer Commit;
- sofortiges Enqueue neu aktivierter Lanes;
- jede Wiederzulassung besitzt genau einen typisierten kausalen Grund;
- unveraenderte Ingress-, Summary-, Edge-, Evidence- und
  Dependency-Versionen erzeugen keinen neuen semantischen Budgetverbrauch;
- stale Ergebnisse werden verworfen und gezielt neu geplant;
- begrenztes Retry-/Ressourcenbudget mit konservativem Jacobi-/Seriell-
  Fallback;
- Jacobi bleibt bis KR-4993 der Rollbackpfad.

### Review- und Abschlussvertrag

- stale Summary oder Evidence kann nie committed werden;
- semantische Budgets bleiben deterministisch und fail-closed;
- echte Breite-1-Ketten werden nicht als parallelisierbar ausgegeben;
- kein Root scheitert allein durch Wiederzulassung unveraenderter Versionen
  am 65.536er-Budget;
- D2 und Sonic bewerten Schedulingwirkung, keine neue Thread- oder
  Worklistmatrix.

---

## [x] KR-4993 - Abschlussreview der Candidate-Resolution-Pfade

Prioritaet: P0, letztes Sourcegate vor KR-4981

Abhaengigkeiten: KR-4985, KR-4986 und alle durch G1/G2 aktivierten Tasks bis
KR-4991

Status: Source-seitig abgeschlossen am historischen funktionalen Source-Checkpoint
`099ae2cb2dfe7699f90338e9df0bad24a7888823`; Analyzer-ABI `34`, Function-Analysis-Epoch-Schema `19`.
Der vollstaendige Sol-Endreview
des unmittelbar vorherigen Explosionsbug-Diffs wurde wiederverwendet; alle
bestaetigten Findings sind geschlossen; das Analyzer-ABI-Finding ist unter dem
aktuellen Analyzer-ABI `34` geschlossen. Nicht aktivierte
KR-4988 bis KR-4991 wurden nicht als geaendert oder reviewpflichtig behauptet.

### Ziel

Der vollstaendige Endreview der aktivierten/geaenderten Context-, Cache-,
Evidence- und Budgetpfade sowie die Pruefung der unveraendert konservativen
FullState-, Binding-, Dependency- und Scheduling-Fallbackgrenzen ist
abgeschlossen; alle vorher bestaetigten Findings sind geschlossen; das
Analyzer-ABI-Finding ist unter dem aktuellen Analyzer-ABI `34` geschlossen.

### Umfang

- semantische Identitaet und exakte Provenienz;
- FullState-Fallback, Digests, Kollisionen und Cache-Key-Schema;
- globale und kontextuelle Fallback-Summaries;
- Invalidierung, Delta-Monotonie, Stale und Evidence-Publikation;
- Cancellation, Budgets, RAM, Retention und Progress;
- logische Zulassung, semantische Lane, physische Auswertung, Cache-Reuse,
  Requeue und Provenienzabonnent als getrennte Zaehldomaenen;
- D1-/D2-Befunde konsistent zusammenfassen.

### Quellseitige Freigabebedingungen

- alle bestaetigten Findings geschlossen und erneut in den betroffenen
  Pfaden reviewt;
- keine reduzierte Analyse-, Resolution-, Guarded-AOT- oder
  Completenessabdeckung;
- Context-, Evaluations- und logische Rootbudgets bleiben autoritativ,
  fail-closed und terminal typisiert;
- `IncompleteRoot` kann weder publiziert noch als wiederverwendbare Epoche
  behalten werden;
- Stale-, Cancellation- und Fehlerreihenfolge kann weder alte Resultate
  publizieren noch aktuelle Arbeit durch einen veralteten Fehler beenden;
- jeder schwere Root ist terminal identifizierbar;
- kein neuer Test, keine Testmatrix und kein Produktlauf in KR-4993;
- KR-4981 bleibt historische RuntimeOnly-Evidenz. Das aktive Produktgate ist
  KR-5005: rein nativer Pfad ohne ARM7/CPU-PVR bis zum Hauptmenue und damit
  Freigabe von `v0.50.0 Alpha`.

D1 und D2 sind begrenzte Diagnoseexporte und decken nicht zwingend alle
`1.191` Roots ab. Die globale Abwesenheit von
`contextual_return_context_limited_functions`,
`contextual_return_evaluation_limited_functions`,
`resolution_root_logical_budget_exhausted` und `IncompleteRoot` ist deshalb
keine beweisbare KR-4993-Bedingung. Sie wird erst in KR-4981 am
vollstaendigen Produktport abgenommen.

---

## [x] KR-4995 - AICA-ARM7-Ausfuehrung und Sound-Interrupt-Lifecycle

Prioritaet: P0 Runtime-Bring-up

Status: Source-seitig abgeschlossen in `e1d8ade`; Runtime-ABI `90`,
AICA-Handoff-Vertrag `2`.

### Ergebnis

- der ARM7TDMI beginnt bei AICA-Resetfreigabe und verwendet den gemeinsamen
  24-Bit-AICA-Bus fuer das gespiegelte 2-MiB-Sound-RAM und die Register;
- Sound-/Main-Interrupts, REG_L/REG_M, Timer, Common-Monitorregister und die
  portable ARM-Fortsetzung teilen einen fail-closed Reset-/Fehlervertrag;
- SkyEmu-Commit `01516d6` ist als MIT-lizenzierter ARM7-Kern mit Provenienz
  eingebunden; Retail-Firmware und Spieldaten bleiben externe Eingaben;
- der vorhandene AICA-Ausfuehrungstest wurde an den realen LLE-Lifecycle
  angepasst und bestand nach dem 24-Thread-Build;
- im No-Skip-Sonic-Lauf laufen Audiotakt, beide Readinesspfade, Player-Status
  `5`, `56.000` YUV-Makrobloecke und die sichtbare Moviepublikation gemeinsam.

### Offene Produktgrenze

Der Film rendert ohne Skip sichtbar und der Player erreicht Status `5`.
KR-4995 ist damit auch produktseitig bis zur Moviepublikation belegt.
KR-4981 bleibt fuer den erst danach auftretenden AOT-Identity-Miss sowie
Memory-Card-Screen und Hauptmenue offen. Ein Movie-Skip, automatischer
FB_W->FB_R-Flip oder titelbezogener Runtime-Hack ist kein Fix.

---

## [ ] KR-4981 - Einmaliges 24-Thread-Sonic-Produktzeitgate

Prioritaet: P0 Produkt- und Performancegate

Abhaengigkeit: RuntimeOnly-Build-/Export- und No-Skip-Movie-Gate bestanden;
der aktuelle Performance-P0 ist der serielle Runtime-/Dispatch-Overhead bei
`24,2926 MHz` im sichtbaren Audio-/Videopfad, mit einem Ziel von mindestens
`100 MHz` ohne Regression. Danach liegt der Runtime-Blocker am Call
`0x8C054008 -> 0x8C9000E8` (`byte-identity-mismatch`). Memory-Card-Screen und
Hauptmenue bleiben offen.

### Abgeschlossene Vorstufe

- [x] `efc531b`: Der historische Vorlauf mit erfolgreichen PVR-Renderabschluessen
  publiziert Video, ISP und
  TSP am selben Gastzyklus; TA-Lifetime-/Resetzaehler bleiben ueber Softresets
  monoton. Der historische 45-Sekunden-Sonic-Lauf blieb nach Presented by Sega schwarz und
  widerlegt diese Fanout-Luecke als alleinige Produktursache.
- [x] `e1d8ade`: echter AICA-ARM7-, Sound-/Main-Interrupt- und Common-
  Monitorpfad; zwei aktive Stimmen und fortschreitender Sofdec-Audiotakt im
  Sonic-Produktlauf. Die verbleibende Bildpublikation liegt danach.

### Umfang

- genau ein frischer privater NativeDisc-Sonic-Kaltport;
- normale 24-Thread-Konfiguration auf der aktuellen Maschine;
- Phasenzeit, CPU, RAM, Cache, Codegen, Hostbuild, Packaging und Gesamtzeit;
- Installation aus der privaten Originaldisc;
- normaler Produktlauf und echter Fensterscreenshot;
- bekannte historische AOT-Grenzen passieren oder engeren typisierten
  Blocker belegen.

### Abschlussbedingungen

- vollstaendiger Port in hoechstens acht Minuten als Ziel;
- kein Context-/Evaluationslimit, kein `incomplete-root` und keine verworfene
  Analyse-Epoche;
- keine reduzierte Funktions-, Block-, Resolution- oder AOT-Abdeckung;
- sichtbarer Fortschritt und terminaler Produktbericht;
- keine 1-/8-/12-/24-Threadmatrix und kein zweiter Build nur fuer Timing.

---

## [ ] KR-4992 - Begrenzte Spekulation spaeterer Resolution-Roots

Prioritaet: bedingtes P1

Abhaengigkeiten: verfehlter KR-4981-Lauf, akzeptable Head-of-Line-Zeit und
positives Restkosten-/RAM-Gate

### Ziel

Erst nach stabilem Candidate-Resolution-Pfad einen kleinen isolierten Anteil
sonst ungenutzter Kerne fuer verwerfbare spaetere Rootarbeit einsetzen.

### Grenzen

- harte Worker-, RAM- und Cache-Reserve fuer den kanonischen Root;
- keine Publikation spaeterer Roots vor ihrer kanonischen Reihenfolge;
- versionierte verwerfbare Resultate;
- keine relevante Verlangsamung des Head-of-Line-Roots;
- bei Cacheverdraengung oder hohem Throwaway-Anteil Pfad deaktivieren;
- danach ein Retry nur auf ausdrueckliche Freigabe.

---

## Weitere offene v0.49-Aufgaben

| ID | Kurzstatus |
|---|---|
| KR-4952 / KR-4953 | frischer ABI-passender CompletePlatform-Handoff, zweiter Capture und normative Paritaet offen |
| KR-4954 / KR-4961 | externes deklaratives Spielprojekt und wiederverwendbares Scaffold offen |
| KR-4955 bis KR-4960 | Funktionsgrenzen, direkte native Calls, Registerlokalisierung, Safepoints und 200-MHz-Hotpath produktseitig abzunehmen |
| KR-4962 | NativeDisc-/DirectBoot-Paritaet am Game Entry offen |
| KR-4963 | inkrementeller Runtime-/Spielbuild und Compilervergleich offen |
| KR-4964 | v0.49-Produktabnahme bis sichtbarem Spielbild und Echtzeit offen |
| KR-4966 bis KR-4970 | relatives Gate, atomarer Handoff sowie AICA/PVR/Maple-Vertraege quellseitig vorhanden; PVR-RenderDone-Fanout und resetfeste TA-Metrik abgeschlossen, sichtbarer Produktnachweis offen |

Auch diese Aufgaben folgen dem repositoryweiten Dreischritt und erzeugen
keine neuen Tests oder Testmatrizen.

## Historisch geplante RuntimeOnly-Produktlaeufe

### Lauf A - KR-4981

- Candidate-Resolution muss vollstaendig abschliessen;
- frischer NativeDisc-Port;
- Originaldisc installieren;
- normaler Lauf mit relativer Gastarbeit;
- realer Screenshot;
- AOT-, Runtime-, Geraete- und Performancebefund dokumentieren.

### Lauf B - DirectBoot-Paritaet

- frischen ABI-passenden CompletePlatform-Zustand erfassen;
- DirectBoot ProductHandoff anwenden;
- Subsystemdigests vergleichen;
- normaler Sonic-Lauf bis zum naechsten Produktmeilenstein.

### Lauf C - historisches sichtbares Spielbild und RuntimeOnly-Echtzeit

- mindestens FirstGameFramebufferWrite oder FirstTaFrame;
- die damalige Zielmarke lag danach bei 200 MHz gleicher Gastarbeit;
- Controller und stabiler mehrminuetiger Lauf folgen erst nach sichtbarem
  Spielfortschritt.

Zwischen diesen Produktgates werden keine neuen Tests, Vollsuiten oder
Matrizen gebaut. Gefixt wird durch Reviews, getestet wird mit Sonic.
