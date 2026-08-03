# Projektstatus

Aktuelle interne Version: `v0.49.0`

## Repositoryweiter Arbeitsvertrag

Fuer jeden Task gilt projektweit:

```text
Task implementieren
  -> alle betroffenen Pfade reviewen
     und bestaetigte Fehler innerhalb dieses Reviews schliessen
  -> den reviewten Task direkt auf main committen und pushen
  -> naechster Task
```

Es gibt keine separate standardmaessige Test-, Verifikations-, Fix- oder
Integrationsphase. Neue Unit-Tests, Regressionstests, Fixtures,
Stresslaeufe, Testprojekte oder Testmatrizen werden nicht gebaut und nicht
als Abschlussbedingung gefordert.

Der reale Sonic-Adventure-PAL-Port ist der massgebliche Produkt- und
Integrationstest. Reviews duerfen fehlende neue Tests nicht als Finding
melden. Vorhandene Tests werden nur geprueft oder repariert, wenn sie selbst
konkret gebrochen, widerspruechlich oder zahlenmaessig falsch sind.

## Evidenztrennung

```text
letzte reale Produktevidenz:
  historische NativeDisc-/DirectBoot-Ports mit aelteren ABI-Vertraegen

funktionaler Source-Checkpoint:
  a521999 / Runtime-ABI 87 / Analyzer-ABI 31 /
  Application-Contract 8 / Portprojektvertrag 75

aktueller realer Diagnosebefund:
  Sonic-v56 terminal nach 1:28:24 mit Exitcode 5
  1/1191 Resolution-Roots committed
  65.536 Contextual-Return-Evaluationen einer Funktion ausgeschoepft
  Context-Limit nicht erreicht
  25.728 eindeutige Contexts
  27.872 physische Auswertungen
  0 Eviction-Recomputes
  Epoch-Retention: incomplete-root
  kein Portartefakt, keine game.exe, kein Screenshot

aktueller Dokumentationsstand:
  repositoryweiter Review->main-Workflow auf main festgeschrieben
```

Source-, Diagnose- und Produktevidenz duerfen nicht als derselbe Fortschritt
ausgegeben werden. Die aktuellen Dokumentationscommits veraendern keine
Recompiler-, Runtime- oder Produktsemantik.

## Aktueller P0

Der terminale v56-Befund schliesst Cache-Churn und die fruehere unnoetige
Deep-Copy-Verstaerkung als verbleibende Hauptursache weitgehend aus.

```text
physische Auswertungen je eindeutigem Context:    1,083
logische Evaluationen je eindeutigem Context:     2,547
logische Evaluationen ohne neue physische Arbeit: 37.664
Anteil physischer Erstberechnungen:                rund 92,3 Prozent
```

Der offene P0 ist eine echte Candidate-Resolution-/Contextual-State-
Explosion mit erheblicher logischer Wiederzulassungsarbeit auf einem
ueberwiegend seriellen kritischen Pfad.

Eine Erhoehung des 65.536er-Budgets, mehr Cache oder mehr Threads ist kein
Fix. Die Arbeit muss semantisch reduziert und kausal korrekt eingeplant
werden, ohne Analyse-, Evidence- oder AOT-Abdeckung zu verlieren.

Der terminale Lauf meldet `1/1191`, daher ist Root 0 nicht mehr als
endgueltig gescheitert belegt. Bis Rootindex, Rootadresse und limitierte
Funktion terminal ausgegeben werden, gilt der Befund allgemein fuer die
ersten schweren Candidate-Resolution-Roots.

## Aktueller kritischer Pfad

```text
KR-4985 implementieren -> betroffene Pfade reviewen -> main
  -> D1 nur nach ausdruecklicher Freigabe
  -> KR-4986 implementieren -> betroffene Pfade reviewen -> main
  -> positiv gegatete KR-4987..KR-4990 jeweils
     implementieren -> reviewen -> main
  -> D2 nur nach ausdruecklicher Freigabe
  -> KR-4991 nur bei positivem G2:
     implementieren -> reviewen -> main
  -> KR-4993 Abschlussreview, Findings schliessen -> main
  -> KR-4981 genau ein voller Sonic-Kaltport und Produktlauf
```

KR-4992 bleibt ein optionaler Folgezweig nach einem verfehlten KR-4981 und
positivem Restkosten-/RAM-Gate. KR-4982 und KR-4983 bleiben gestrichen.

D1 und D2 sind reale Sonic-Diagnoseexporte, keine Testmatrix. KR-4993 ist ein
Quellpfadreview ohne neue Tests und ohne Produktlauf. KR-4981 ist der erste
naechste vollstaendige Produkt- und Integrationstest.

## Quellseitig vorhandene Hauptvertraege

Der aktuelle funktionale Source enthaelt unter anderem:

- statische Guarded-AOT-Einstiege und fail-closed
  Exportvollstaendigkeitsvertraege;
- getrennte semantische und inventorybezogene Analysepfade;
- inkrementelle ProgramGraph-, SCC-, ABI-, Summary- und Candidate-
  Strukturen;
- gemeinsame Analyseexecutor- und Speicherhaushaltsvertraege;
- schichtweise Analyse-, IR-, Codegen- und Hostbuildcaches;
- exakte Latent-AOT-Hints und Multi-Extent-SourceBindings;
- baseline- und bildinhaltsgebundene sichtbare Frameklassifikation;
- relatives Post-Entry-Produktgate und typisierte Fehlerausgaenge;
- vorbereiteten atomaren CompletePlatform-Apply;
- save-erhaltendes ProductHandoff-Profil;
- statische native Produktmaterialisierung ohne Interpreter oder JIT.

Diese Sourcevertraege sind fuer den aktuellen Stand nicht produktseitig
abgenommen, weil v56 kein Portartefakt erzeugte.

## Offene Produktabnahmen

- Candidate-Resolution ohne Context-/Evaluationslimit und ohne
  `incomplete-root`;
- vollstaendiger aktueller NativeDisc-Port;
- bekannter historischer Missing-AOT-Pfad durch statisches AOT passiert oder
  engerer typisierter Blocker;
- korrekter terminaler Produktbericht und Child-Exitcode;
- frischer ABI-passender CompletePlatform-Capture und ProductHandoff;
- NativeDisc-/DirectBoot-Paritaet am Game Entry;
- sichtbarer Spielframe statt technischer Hilfsmetrik;
- vollstaendiger Kaltport in hoechstens acht Minuten;
- mindestens 200 MHz im normalen Produktpfad;
- externes Spielprojekt ohne Retaildaten oder Sonic-Sonderfaelle im
  Katana-Kern.

## Test- und Reviewstatus

Projektweit gilt ab jetzt:

- Gefixt wird mit Reviews der vollstaendigen betroffenen Pfade.
- Getestet wird mit Sonic an den geplanten Produktgates.
- Keine neuen Tests, Testmatrizen, synthetischen Fixtures oder Ersatzgates.
- Fehlende neue Tests werden in Reviews nicht beanstandet.
- Vorhandene Tests und Testzahlen werden nur bei konkretem Fehler repariert.

Historische Angaben zu frueher ausgefuehrten Tests bleiben historische
Evidenz und erzeugen keine neue Pflicht fuer den aktuellen Arbeitsablauf.

## Naechster Task

```text
KR-4985 - Candidate-Resolution-Phasen- und Kardinalitaetstelemetrie
```

Der Task wird implementiert, seine betroffenen Progress-,
Function-Value-, Cache-Key-, Binding-, Budget-, Retention- und Terminalpfade
werden reviewt, bestaetigte Findings werden geschlossen und der Task wird
direkt auf `main` gepusht. Erst danach darf D1 auf ausdrueckliche Freigabe
laufen.
