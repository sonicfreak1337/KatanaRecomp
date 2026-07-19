# KatanaRecomp: Kontrollfluss-, Sonic-Harness-, Controller- und GUI-Review

**Pruefstand:** `main` auf Commit `b846e46b22b32bd2a25799e4f62a8692dbfaa9b0`
**Reviewdatum:** 18. Juli 2026
**Prioritaeten:** Genauigkeit, bestehende Funktionserhaltung, Laufzeit, Analyse- und Buildgeschwindigkeit

## 1. Umfang und Grenzen

Geprueft wurden:

- rekursive Codeentdeckung und Basic-Block-Bildung
- lokaler, CFG-basierter und interprozeduraler Kontrollfluss
- Nutzerhinweise, Overrides, Zielvalidierung und Jump Tables
- Kontrollflussberichte, Testvertraege und Fixpunktstrategie
- privater Sonic-Adventure-Harness
- Runtime-, Checkpoint-, Metrik-, Budget- und Datenschutzvertraege
- vorhandene Keyboard-/Maple-Eingabekette und fehlende Gamepadintegration
- GUI-Modell, Jobereignisse, Windows-Shell, Layout und Aktualisierungspfad
- Roadmap, Taskabhaengigkeiten und kollidierende Task-IDs

Die Review ist statisch gegen den genannten GitHub-Stand. Es wurden keine
Retaildaten, Spielbytes, Spieladressen, Disassemblies, Screenshots oder
sonstigen proprietaeren Inhalte uebernommen. Alle vorgeschlagenen Regressionen
muessen synthetisch oder frei lizenziert sein.

## 2. Gesamturteil

Die Analyse besitzt bereits gute konservative Grundlagen:

- ungueltige Ziele werden sichtbar abgelehnt
- beschreibbare Jump Tables werden nicht als unveraenderlich eingefroren
- partielle Tabellen werden nicht als vollstaendige Zielmenge eingespeist
- Ergebnisreihenfolgen werden deterministisch sortiert
- private Daten bleiben ausserhalb der oeffentlichen Berichte

Trotzdem existieren mehrere Stellen, an denen ein unsicheres Ergebnis zu einer
zu starken Aussage aufgewertet werden kann. Diese Fehlerklasse ist fuer einen
statischen Recompiler besonders gefaehrlich: Ein Crash ist sichtbar, eine zu
kleine Zielmenge kann dagegen korrekt aussehen, bis der Gast einen nicht
generierten Pfad nimmt.

Der Harness besitzt gute Pfad- und Repositoryschutzpruefungen, vermischt aber
noch Build, deterministischen Lauf und interaktive Sitzung. Die GUI hat ein
brauchbares Ereignismodell als Fundament, zeigt davon jedoch fast nichts an.
Das Windows-Fenster ist faktisch eine feste Formularseite mit zwei Balken und
einem grossen Textfeld.

## 3. Kontrollfluss: P0-Genauigkeitsbefunde

### CF-01: Hint-Direktiven werden als statischer Beweis behandelt

Bei einem bisher offenen indirekten Sprung setzt der Hintpfad den Status auf
`Resolved`, traegt das Ziel ein und verwendet `user-hint` als Grund. Auch
Funktionshinweise werden als beweisende Seeds eingereiht.

Ein Hint ist aber nur eine Suchhilfe. Er darf:

- Decode und Analyse eines Kandidaten anstossen
- Diagnostik und Vergleich mit spaeteren Beweisen ermoeglichen
- niemals `unresolved` schliessen
- niemals den dynamischen Default entfernen
- niemals Exportvollstaendigkeit herstellen

Erforderlich ist eine getrennte Evidenzklasse `HintCandidate`.

### CF-02: Delay-Slot-Zustand wird global nur nach Adresse gespeichert

Die rekursive Analyse speichert Delay Slots in einer globalen Adressmenge.
Wird dieselbe Adresse auf einem Pfad als normale Instruktion und auf einem
anderen als Delay Slot erreicht, kann nur eine globale Darstellung existieren.
Sobald eine Instruktion als Delay Slot markiert ist, wird ihre
Kontrollflusswirkung vollstaendig uebersprungen.

Der Workitem-Schluessel muss mindestens enthalten:

- Instruktionsadresse
- optionalen Delay-Slot-Owner
- Beweisklasse
- eingehenden Kontrollflusskontext

Kontrollinstruktionen in einem Delay Slot muessen als Slot-Illegal- oder
sichtbar nicht unterstuetzter Fall diagnostiziert werden, nicht kommentarlos
verschwinden.

### CF-03: Basic Blocks koennen Fallthrough ueber Adressluecken erzeugen

`build_basic_blocks` verwendet fuer Fallthrough schlicht den naechsten Block in
sortierter Reihenfolge. Bei getrennten Segmenten oder einer nicht entdeckten
Luecke kann dadurch eine Kante in einen voellig anderen Adressbereich
entstehen.

Ein Fallthrough darf nur erzeugt werden, wenn der Zielblock exakt an der
architekturgemaessen Folgeadresse beginnt:

- normale Instruktion: `PC + 2`
- verzweigte Instruktion mit Delay Slot: `PC + 4`

Fehlt diese Adresse, ist der Pfad unvollstaendig. Der naechste sortierte Block
ist keine semantische Aussage, nur eine Sortierreihenfolge. Computer sind
bekanntlich erstaunlich gut darin, beides zu verwechseln, wenn Menschen ihnen
dabei helfen.

### CF-04: Delay-Slot-Zuordnung in der Blockbildung prueft keine Kontiguitaet

`terminal_index` nimmt bei einer Instruktion mit Delay Slot das naechste Element
des sortierten Instruktionsarrays. Es wird nicht geprueft, ob dieses Element
wirklich bei `PC + 2` liegt und als Slot desselben Owners markiert ist.

Die Blockbildung muss Owner und Slot explizit paaren. Ein fehlender oder
widerspruechlicher Slot ist ein Analysefehler und darf keinen fremden Befehl in
den Block ziehen.

### CF-05: Vollstaendigkeit wird auf ein `guarded`-Bit pro Kante reduziert

Die CFG-Kante besitzt nur `guarded`. Der Basic-Block-Pfad entfernt seinen
dynamischen Nachfolger, sobald irgendeine passende Kante unguarded ist. Das ist
nicht ausreichend.

Vollstaendigkeit ist eine Eigenschaft der gesamten Site, nicht einer einzelnen
Kante. Benoetigt wird ein Sitevertrag mit:

- `ProvenComplete`
- `GuardedComplete`
- `GuardedPartial`
- `ForcedOverride`
- `HintCandidate`
- `RuntimeOnly`
- `Unresolved`

Nur die beiden vollstaendigen Klassen duerfen den Runtime-Default entfernen.

### CF-06: Unbekannte Caller werden bei Kandidateneingaben ignoriert

Die interprozedurale Kandidatenzusammenfuehrung ueberspringt unbekannte oder
leere Callerwerte. Ein anderer bekannter Caller kann dadurch eine scheinbar
vollstaendige endliche Zielmenge erzeugen.

Ein unbekannter relevanter Caller muss die Eingabe auf `Incomplete` setzen.
Bekannte Teilmengen duerfen fuer Diagnostik und Guardkandidaten erhalten
bleiben, aber nicht als vollstaendig gelten.

### CF-07: Kontextabhaengige Zielmengen werden nur nach Siteadresse dedupliziert

Die abschliessende Deduplizierung vergleicht nur die Instruktionsadresse.
Abweichende Zielmengen oder Unsicherheiten verschiedener Callkontexte werden
nicht vereinigt, sondern eine Variante bleibt uebrig.

Alle Kontexte derselben Site muessen konservativ vereinigt werden:

- Vereinigungsmenge aller Ziele
- `complete` nur, wenn jeder Kontext vollstaendig ist
- `guarded` bei mindestens einem dynamischen Kontext
- Erhalt aller Evidenz-IDs

### CF-08: Endliche indirekte Callees nutzen ihre Rueckgabesummaries nicht

Bei einem indirekten Call werden Kandidatencallees fuer Eingabebeobachtungen
erfasst. Die Rueckgabesummary wird aber nur fuer einen einzelnen direkt
bekannten Callee angewandt.

Fuer eine endliche Callmenge darf R0 vereinigt werden, wenn:

- jeder Kandidat eine vollstaendige Summary besitzt
- keine unbekannte externe Wirkung besteht
- die vereinigte Wertmenge im Budget bleibt

Fehlt nur eine Summary, bleibt das Ergebnis unvollstaendig.

### CF-09: Dynamische Herkunftsklassen beruhen auf einem linearen Acht-Befehle-Fenster

Stack-, Parameter-, VTable- und Rueckgabeklassen werden aus maximal acht
vorherigen linearen Instruktionen abgeleitet. Dominatoren, Joins, alternative
Vorgaenger, Callgrenzen und Aliase werden nicht einbezogen.

Die Klassifikation sollte einen begrenzten SSA-/Def-Use-Backward-Slice benutzen.
Das Ergebnis muss Beweis, Kandidat und unbekannte Ursache getrennt halten.

### CF-10: Ein gueltiges Codeziel ist noch kein bewiesener Instruktionsanfang

Die Zielvalidierung prueft gerade Adresse, Code-Segment, Execute-Berechtigung und
committete Bytes. Sie beweist nicht, dass die Adresse ein bereits bestaetigter
Instruktionsanfang ist. Bei Dateninseln in einem Code-Segment kann ein
ausgerichteter Wert sonst als Codekandidat erscheinen.

Zwei Vertrage sind zu trennen:

- `valid_decode_candidate`
- `proven_instruction_boundary`

Ein Kandidat darf rekursiv decodiert werden. Ein statischer Beweis braucht
zusaetzliche Boundary- oder Kontrollflussevidenz.

### CF-11: Guarded Callziele werden zu harten Funktionsgrenzen

Funktionsentdeckung behandelt Zielkanten als bekannte Entries, verliert aber
deren Beweisklasse. Ein nur bewachter Kandidat kann dadurch die Blockzuordnung
einer bewiesenen Funktion veraendern.

Funktionskandidaten brauchen:

- Proof-Klasse
- Eigentumsstatus fuer Bloecke
- Shared-/Tail-Merge-Unterstuetzung
- getrennte Call- und Tail-Jump-Semantik

### CF-12: Tests binden sich an die aktuelle Fixpunktimplementierung

Einige Tests erwarten eine exakte Zahl von Fixpunktiterationen. Das blockiert
spaetere inkrementelle oder SCC-basierte Solver, obwohl das semantische Ergebnis
identisch und schneller waere.

Tests sollen pruefen:

- identische Zielmengen und Provenienz
- deterministische Ausgabe
- Terminierung unter einem oberen Budget
- keine exakte interne Worklistreihenfolge

## 4. Kontrollfluss: Geschwindigkeitsverbesserungen

### 4.1 Delta-Fixpunkt statt Ganzprogrammanalyse

Der Hauptfixpunkt decodiert, baut CFGs und berechnet lokale Aufloesungen nach
jedem neuen Seed erneut fuer das gesamte Programm.

Vorgeschlagen:

1. unveraenderliche Instruktionsarena
2. Address-to-Instruction- und Site-Indizes
3. inkrementelle neue Seeds
4. lokale Blockinvalidierung
5. SCC-basierter Callgraphfixpunkt
6. Neuberechnung nur ab geaenderten Ingresswerten

### 4.2 Typisierte Evidenz statt wachsender Herkunftsstrings

Herkunft sollte als internierte DAG oder kompakte Evidence-ID gespeichert
werden. Die menschenlesbare Zeichenkette wird erst fuer den Bericht erzeugt.
Das reduziert Kopien, Speicher und quadratische Stringverkettungen.

### 4.3 Immutable Blocks mit Spans

Basic Blocks kopieren derzeit Disassemblyzeilen. Besser sind Indexbereiche in
einer stabilen Instruktionsarena. Das beschleunigt Blockaufbau,
Funktionsanalyse und wiederholte Fixpunkte.

### 4.4 Gemeinsame Indizes

Ein zentraler Analyseindex sollte enthalten:

- Instruktion nach Adresse
- Block nach Start und Kontrollinstruktion
- ausgehende/eingehende Kanten
- Site nach Instruktionsadresse
- Funktionen und SCC
- Segment-/Page-Index
- Tabellen-Snapshotcache

Determinismus wird durch sortierte Ausgabe gesichert, nicht durch `std::map` und
`std::set` in jedem Hotpath.

### 4.5 Allgemeinere Jump Tables ohne Genauigkeitsverlust

Der aktuelle Patternmatcher ist sicher, aber sehr eng. Der neue Slice-Solver
soll erkennen:

- absolute 32-Bit-Ziele
- signed/unsigned 8-, 16- und 32-Bit-Offsets
- base-relative Tabellen
- PIC-/Registerbasis
- unterschiedliche CMP/BT/BF-Guardformen
- Thunks und endliche Calltabellen

Eine Tabelle gilt nur als vollstaendig, wenn Bounds, Dominanz, Snapshot und alle
Eintraege bewiesen sind. Teilziele bleiben diagnostische Kandidaten mit
Runtime-Default.

### 4.6 Messbare Budgets

Getrennt messen:

- Decodezeit
- CFG-Aufbau
- lokale Wertanalyse
- Funktionssummaries
- Jump-Table-Slices
- Berichtserzeugung
- Peak-Speicher
- Anzahl wiederverwendeter Bloecke/Summaries

## 5. Sonic-Adventure-Harness: P0-Befunde

### H-01: Der Harness startet `game.exe` automatisch

Nach einem erfolgreichen Build wird eine vorhandene `game.exe` ohne
Ausfuehrungsmodus gestartet. Das widerspricht dem v0.47-Vertrag
"bauen, aber nicht starten".

Der Configvertrag braucht explizite Modi:

- `build-only`
- `runtime-probe`
- `interactive`

`build-only` muss Prozessstart technisch verbieten und
`game_executable_started=false` beweisen.

### H-02: Das Runtimeartefakt wird ueber einen festen Pfad statt den Jobartefakten gewaehlt

Der Harness nimmt `$outputRoot\game.exe`. Damit ist nicht bewiesen, dass die
Datei vom aktuellen Job stammt und nicht ein stale Artefakt ist.

Der auszufuehrende Pfad muss aus dem aktuellen `JobResult.artifacts` kommen,
dessen Hash geprueft und dessen Staginggeneration an die aktuelle
Projektidentitaet gebunden werden.

### H-03: Manifest und separate GDI koennen voneinander abweichen

Die Konfiguration besitzt `manifest_path` und `gdi_path` separat. Vor dem Build
und Lauf muss bewiesen werden, dass beide dieselbe Source-Snapshotidentitaet
beschreiben.

### H-04: Checkpoint `KR_RETAIL_ANALYSIS_CONTINUES` wird heuristisch erfunden

Mehr als eine Funktion und mehr als vier analysierte Bytes sind kein
semantischer Checkpoint. Checkpoints duerfen nur aus versionierten
Job-/Runtimeevents stammen.

Oeffentliche Kernereignisse sollten generisch sein. Der private Harness darf sie
lokal auf Sonic-Bezeichnungen abbilden, ohne Adressen oder Spielinhalte zu
speichern.

### H-05: Textregexe ersetzen einen Metrikvertrag

Metriken werden mit allgemeinen Regexen wie `frames=(\d+)` aus kombiniertem
stdout/stderr gelesen. Das kann falsche oder doppelte Werte akzeptieren.

Erforderlich:

- genau ein versioniertes JSON-Metrikobjekt
- JSONL-Ereignisse mit Sequenz und Gastzyklus
- strikte Schemavalidierung
- Ablehnung doppelter Abschlussmetriken
- getrennte stdout-/stderr-Kanaele

### H-06: stdout und stderr verlieren ihre zeitliche Reihenfolge

Beide Streams werden separat vollstaendig gelesen und anschliessend
aneinandergehaengt. Checkpoint- und Fehlerreihenfolge kann dadurch falsch sein.

Ein strukturierter Eventkanal oder ein einzelner geordneter Pipevertrag ist
erforderlich.

### H-07: Host- und Gastfortschritt werden verwechselt

`frames_presented` kann ein synthetischer Hostframe sein.
`input_events_consumed` zaehlt eingespeiste Hostevents, nicht vom Gast gelesene
Maple-Zustaende.

Metriken muessen mindestens trennen:

- `host_present_calls`
- `guest_pvr_frames`
- `host_input_state_changes`
- `maple_get_condition_reads`
- `guest_observed_input_changes`
- `guest_aica_buffers`
- `host_audio_buffers`

### H-08: Budgets sind vermischt

Build und Runtime verwenden denselben Hosttimeout jeweils separat.
`budget_exhausted` vermischt Hosttimeout und moegliche Gastbudgets.

Getrennte Felder:

- globales Harnessdeadline
- Analysebudget
- Buildtimeout
- Runtimehosttimeout
- Gastzyklusbudget
- Schedulerereignisbudget
- Blockbudget
- Ursache des ersten Budgetendes

### H-09: Prozessargumente und Prozessbaumbehandlung sind fragil

Argumente werden manuell gequotet. Der Timeoutpfad ruft `taskkill.exe`
unabhaengig vom zuvor teilweise plattformneutralen Pfadcode auf.

PowerShell `ArgumentList` und eine klar Windows-spezifische oder sauber
abstrahierte Prozessbaumbehandlung sind erforderlich.

### H-10: Berichte werden nicht atomar veroeffentlicht

Der Bericht wird direkt mit `Set-Content` geschrieben. Ein Abbruch kann einen
teilweisen Bericht hinterlassen.

Private Berichte brauchen:

- Tempdatei plus atomaren Rename
- restriktive Dateirechte
- explizite Redaktions-Allowlist
- keine Rohlogs, Speicherinhalte, Adressen oder Tracknamen im Repository
- private Rohdiagnosen nur ausserhalb des Repositorys

## 6. Harness: Geschwindigkeitsverbesserungen

- Eingabesnapshot und SHA-256 genau einmal pro Harnesslauf erfassen und an alle
  Stufen weiterreichen
- Analyse-, Codegen-, Configure-, Compile- und Runtimecache getrennt
  content-addressieren
- CMake-/Ninja-Konfiguration nur bei geaendertem Tool-, SDK- oder Profilhash
  wiederholen
- finalen Source-Identity-Check niemals durch Cache ersetzen
- Stufenzeit, Cachehit, Peak-Speicher und erzeugte Dateimenge berichten
- zwei deterministische Runtimeprobes in einem Modus ausfuehren und normalisierte
  Kernmetriken vergleichen
- interaktive Sitzung niemals als deterministischen Gatebeweis verwenden

## 7. Controllerunterstuetzung vor dem Alpha-Gate

Die Runtime besitzt bereits:

- ein generisches `HostInputBackend`
- einen injizierbaren Controllerzustand
- Replayinput
- Maple-Controllerantworten mit aktiv-niedrigen Buttons
- Trigger und zwei Analogachsen

Der generierte Port fuehrt den Gast jedoch synchron bis zum Ende aus und pollt
Fenster-/Tastaturereignisse erst danach. Damit kann die aktuelle Eingabe den
laufenden Gast nicht beeinflussen.

### Erforderlicher Laufvertrag

1. Gast bis zum naechsten Safepoint, Schedulerdeadline oder Quantum ausfuehren.
2. Hostevents pollten, ohne Gastsemantik an Hostzeit zu binden.
3. Controllerzustand nur bei Aenderung als gastzeitgestempeltes Event einspeisen.
4. Maple `GetCondition` liest den letzten Zustand mit
   `event.guest_cycle <= transaction_cycle`.
5. Video, Audio, Scheduler und Eingabe in derselben Runtime-Schleife fortsetzen.
6. Deterministischer Modus verwendet Replayevents.
7. Interaktiver Modus ist lokal und nicht Gateevidenz.

### Alpha-Zielplattform

Fuer Windows:

- dynamisch geladenes XInput-Backend oder gleichwertige stabile Hostabstraktion
- Keyboardfallback
- Hotplug
- Deadzones und Triggernormalisierung
- konfigurierbares Mapping
- Fokusverlust neutralisiert beziehungsweise friert Eingabe kontrolliert ein
- standardmaessig nur Controller 1; weitere Maple-Ports spaeter

### Tests

- synthetische Achsen-/Trigger-/Buttonvektoren
- aktiv-niedrige Maple-Payloads
- Hotplug und Fokus
- Replaygleichheit
- keine Race Condition zwischen Hostpoll und Maple-Read
- frei lizenziertes Controller-Homebrew
- keine Retaildaten im Testkorpus

## 8. GUI- und QOL-Befunde

### GUI-01: Die Modellseiten sind im Windowsfenster faktisch unsichtbar

Das Modell besitzt Dashboard, Project, Source, Jobs, Diagnostics, Results und
Settings. Das Fenster rendert aber immer dieselbe Zusammenfassungszeile, zwei
Pfadfelder, zwei Balken und ein Log. F6 aendert die Modellseite, nicht die
sichtbare Ansicht.

### GUI-02: Fortschritt ist zu grob

Vorhanden sind Sequenz, Stage, Prozent, Schrittzaehler und Laufzeit. Es fehlen:

- stabile Stage-ID und Vertragsversion
- Einheit des Schritts
- konkrete Analysezaehler
- Cachehits
- Rate und ETA
- Kontrollflussklassen
- Warnstatus und erster Blocker

Prozent darf nur erscheinen, wenn ein belastbarer Nenner existiert.

### GUI-03: Polling kopiert zu viel Zustand

Alle 100 ms wird ein Snapshot abgefragt. Dieser kopiert Log, Ereignisse,
Diagnosen und Artefakte unter einem Mutex. Mit wachsendem Verlauf blockiert das
UI und Jobthread zunehmend.

Erforderlich:

- eventgetriebene `WM_APP`-Updates
- zusammengefasste Updates mit begrenzter Frequenz
- Metadatensnapshot statt Vollkopie
- virtueller/paginierter Log- und Diagnosespeicher
- kein Dateisystem-I/O unter dem UI-Mutex

### GUI-04: Layout ist hart kodiert

Das Fenster verwendet feste Pixelkoordinaten und eine feste Contenthoehe.
Navigation, Statusleiste, Detailpaneele und echte responsive Gruppen fehlen.

### Vorgeschlagene Informationsarchitektur

- linke Navigation: Projekt, Quelle, Analyse, Build, Diagnostik, Ergebnisse,
  Einstellungen
- obere Befehlsleiste: Quelle, Ausgabe, Profil, Start, Abbruch
- mittlere Stage-Timeline mit Statuskarten
- zentrale Kennzahlen:
  - Bytes/Instruktionen/Funktionen
  - resolved/guarded/runtime-only/unresolved
  - aktuelle Partition und Compilerfortschritt
  - Cachehits
  - Zeit und Speicher
- untere Tabs: Details, Diagnosen, Log, Artefakte
- Statusleiste: Toolversion, Runtime-ABI, Buildmodus, Determinismus, Cachezustand

### QOL-Funktionen

Prioritaet P0/P1:

- Preflight vor Jobstart
- Fehler mit Recoveryaktion und betroffener Stage
- echte Results-/Diagnostics-Seiten
- Controllerstatus und Mappingtest
- Runvergleich und Determinismusdiff
- redigiertes Diagnosepaket
- Open Output / Copy Command / Rerun Failed Stage
- Logsuche, Filter, Autoscroll-Schalter
- Artefaktliste mit Rolle, Groesse und Hash
- Drag-and-drop fuer GDI und Manifest
- letzte Projekte und Profile
- klare Modi `Accuracy`, `Balanced`, `Diagnostics`; kein Modus darf Genauigkeit
  abschalten
- Barrierefreiheit, Tastaturpfad, DPI und High Contrast

## 9. Task-ID-Kollision

Die IDs `KR-4801` bis `KR-4805` und `KR-4901` wurden nach der
Roadmapverdichtung mit neuer Bedeutung wiederverwendet.

Die Korrektur lautet:

| ID | Unveraenderliche urspruengliche Bedeutung |
|---|---|
| KR-4801 | Versioniertes Runtime-SDK fuer externe Port-Projekte |
| KR-4802 | Gemeinsamer CLI-/GUI-Portexport und Buildworkflow |
| KR-4803 | Out-of-Tree-`game.exe`-Integration |
| KR-4804 | v0.48 Gate-Vorbereitung |
| KR-4805 | v0.48 interne Meilenstein-Freigabe |
| KR-4901 | Alpha-CI-Konfiguration fuer Windows und Linux |

Die faelschlich zugewiesenen neueren Bedeutungen werden migriert:

| Faelschlich wiederverwendete Bedeutung | Neue ID |
|---|---|
| Runtimebeobachtung, Replay und Fehlerpakete | KR-4911 |
| Dynamische Codebereiche, Module und Overlays | KR-4912 |
| CPU-/Plattform-Bring-up bis `KR_GUEST_PROGRAM_ENTERED` | KR-4913 |
| Gast-PVR bis `KR_FIRST_GUEST_FRAME` | KR-4915 |
| Menue, Eingabe und spielbare Szene | KR-4916 |
| Breite Alpha-Haertung, Paketierung, CI und Audit | auf KR-4901, KR-4902 und KR-4903 aufgeteilt |

Neue Regel:

- Eine Task-ID ist ab dem ersten Merge unveraenderlich.
- Titel duerfen praezisiert, aber nicht semantisch ersetzt werden.
- Entfallene Tasks bleiben als `retired` oder `superseded` in der Registry.
- Ersatzarbeit erhaelt eine neue ID.
- Ein Roadmaplinter prueft Registry, aktive Tasks und Migrationen.

## 10. Neue Roadmapreihenfolge

```text
v0.47:
Core-Korrektheit
-> Kontrollflusssoundness
-> Performance/Build
-> Retail-Kontrollfluss
-> privater Build-only-Nachweis

v0.48:
Runtime-SDK und gemeinsamer Export
-> Harnessmodi und strukturierte Evidenz
-> Controller/Maple
-> GUI-Telemetrie, Layout und QOL
-> Out-of-Tree-Port-Gate

v0.49:
Runtimebeobachtung
-> dynamische Module
-> KR_GUEST_PROGRAM_ENTERED
-> interaktive private Sitzung und Gast-PVR
-> Menue und spielbare Szene
-> CI, Pakete, Audit und Gate-Automatisierung

v0.50:
oeffentliches Alpha-Gate
```

## 11. Priorisierte Umsetzung

### Sofort, P0

1. KR-4614 um die neuen Kontrollflussinvarianten erweitern.
2. KR-4617 um exhaustive CFG-/Delay-Slot-/Hint-Regressionen erweitern.
3. KR-4719 mit technisch erzwungenem `build-only`-Harness abschliessen.
4. Task-ID-Registry einfuehren und IDs korrigieren.

### Danach, P1

5. KR-4622 auf Delta-/SCC-Fixpunkt und gemeinsame Indizes umstellen.
6. v0.48 Runtime-SDK, Harness, Controller und GUI-Telemetrie.
7. GUI-Layout und eventgetriebene Darstellung.
8. Erst danach private v0.49-Runtimeprobes.

## 12. Nicht verhandelbare Schutzregeln

- kein Spielcode im Repository
- keine Retailbytes in Fixtures
- keine privaten Adressen, Hashes, Tracknamen oder Rohlogs in oeffentlichen
  Berichten
- Retailbefunde nur als allgemeine Beschreibung
- jede Reparatur durch synthetische oder frei lizenzierte Regression
- keine Genauigkeitsreduktion fuer einen Fastmodus
- interaktive Sitzung ist nie deterministischer Gatebeweis
