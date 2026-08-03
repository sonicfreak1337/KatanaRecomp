# Verbindliche Arbeitsregeln fuer das gesamte Repository

Diese Regeln gelten fuer jeden automatisierten Bearbeiter, jeden Task, jede
Phase und jeden Teilbereich dieses Repositories. Sie sind keine Empfehlung.
Widersprechende aeltere Prozessbeschreibungen in Roadmap-, Task-, Status-,
Handoff- oder Performance-Dokumenten werden durch diesen Vertrag ersetzt.

## Projektweiter Taskablauf

Fuer jeden Task gilt ab sofort genau diese Reihenfolge:

```text
Task implementieren
  -> alle durch den Task betroffenen Pfade reviewen
     und bestaetigte Fehler innerhalb desselben Reviewdurchlaufs schliessen
  -> den reviewten Task direkt auf main committen und pushen
  -> naechster Task
```

- Die Reviewstufe ist die Fehlerfindungs- und Fixstufe. Sie umfasst den
  implementierten Pfad, seine Aufrufer und Verbraucher, Verdrahtung,
  Datenfluss, Fehlerpfade, ABI-/Cache-/Versionsvertraege sowie alle weiteren
  unmittelbar betroffenen Schichten.
- Bestaetigte Korrektheits-, Boot-, Vollstaendigkeits- und relevante
  Performancefehler im Taskscope werden vor dem Push geschlossen. Eine
  separate nachgelagerte Fix-, Verifikations- oder Testphase wird daraus
  nicht erzeugt.
- Tasks werden standardmaessig direkt auf `main` bearbeitet, committed und
  gepusht. Neue Taskbranches, Pull Requests oder parallele Integrationszweige
  werden nur auf eine neue ausdrueckliche Nutzeranweisung angelegt.
- Ein Review darf ausserhalb des aktuellen Taskscopes liegende Beobachtungen
  knapp dokumentieren, aber daraus weder eigenmaechtig neue Tasks noch eine
  Scope-Erweiterung ableiten.
- Die festgelegte Taskreihenfolge bleibt verbindlich. Erst der Push des
  reviewten Tasks auf `main` gibt den naechsten Task frei.

## Sonic ist der Test

- Der reale Sonic-Adventure-PAL-Port ist projektweit der massgebliche Produkt-
  und Integrationstest: echter Export, normale Discinstallation, normaler
  Programmlauf und echter sichtbarer Fortschritt.
- Es werden keine neuen Unit-Tests, Regressionstests, Testmatrizen,
  synthetischen Fixtures, Stresslaeufe, Testprojekte, Ersatzgates oder
  Konformitaetssuiten als Bestandteil eines Tasks gebaut oder gefordert.
- Reviews duerfen das Fehlen neuer Tests niemals als Finding melden und keine
  neuen Tests als Abschlussbedingung verlangen. Gefixt wird anhand der
  Quellpfadreviews; integriert getestet wird mit Sonic.
- Vorhandene Tests duerfen auf gebrochene Erwartungen, falsche Testzahlen,
  widerspruechliche Semantik oder bereits vorhandene Fehler geprueft und bei
  Bedarf repariert werden. Ihr Bestand wird aber nicht erweitert, nur um eine
  neue Aenderung mit weiterer Testinfrastruktur zu umgeben.
- Regulaere Tasks starten keine Testmatrix und besitzen keinen eigenen
  Testbuild als Pushgate. Bereits vorhandene automatische Checks koennen
  beobachtet werden, ersetzen aber weder das Review noch den Sonic-
  Produktnachweis.
- Sonic-Produktlaeufe erfolgen an den in Roadmap und Tasks festgelegten
  Produktgates oder nach einer ausdruecklichen Nutzeranweisung, nicht nach
  jedem einzelnen Task. Mehrere zusammenhaengende reviewte Tasks duerfen vor
  dem naechsten Sonic-Lauf auf `main` landen.
- Performance wird am realen End-to-End-Produktpfad gemessen. Synthetische
  Zeiten, gruene Testmatrizen oder technische Hilfsframes sind kein Ersatz
  fuer Kaltbuildzeit, vollstaendigen Export und sichtbaren Sonic-Lauf.

## Unveraenderte Produktgrenzen

- KatanaRecomp bleibt ein statischer SH-4-Recompiler.
- Kein allgemeiner Interpreter, kein JIT und kein Emulationsfallback im
  normalen Produktpfad.
- Keine Sonic-spezifischen Adresshacks, Retailbytes oder aus kommerziellen
  Dateien kopierten beziehungsweise ungebunden erzeugten Inhalte im
  generischen Katana-Kern.
- Das reale Produkt und sein Bootfortschritt bleiben autoritativ; ein Review
  darf keine fehlende Produktabdeckung durch erfundene Erfolge oder stilles
  Weglassen von Arbeit kaschieren.

## Laufzeit und Ressourcen

- Kein gestarteter Prozess und keine einzelne Phase laeuft laenger als
  15 Minuten. Nur eine ausdrueckliche Nutzerfreigabe fuer genau einen benannten
  Lauf hebt diese Grenze voruebergehend auf.
- Ein abgelaufener oder abgebrochener Prozess wird mitsamt seinem Prozessbaum
  quiesziert, bevor ein Nachfolger startet.
- Produktive Arbeit nutzt die verfuegbaren Hostressourcen parallel;
  Ein-Kern-Ausfuehrung ist kein akzeptabler Default.
- Potenziell lange Produktphasen melden spaetestens alle zehn Sekunden
  belastbaren Fortschritt beziehungsweise einen Heartbeat.
- Lange Prozesse werden so gestartet, dass ihre Ausgabe live sichtbar ist;
  ein nur am Ende ausgegebener gepufferter Log ist unzulaessig.
- Ein wiederholter Heartbeat ohne Aenderung von Phase, geplant, queued, aktiv,
  fertig oder kanonisch publiziert ist nur Liveness und kein Fortschritt.
  Bleibt ein Prozess 60 Sekunden ohne nachweisliche Arbeitsbewegung, wird er
  vor der 15-Minuten-Obergrenze als Stall beendet und sein Prozessbaum
  quiesziert.
- Steigende CPU-Last, Evaluation-, Cache-, Miss-, Requeue- oder
  Kontextzaehler gelten fuer sich niemals als Produktfortschritt. Bleiben
  abgeschlossene beziehungsweise kanonisch publizierte Arbeit und der
  Head-of-Line-Fortschritt stehen, waehrend interne Arbeit wiederholt neu
  erzeugt, invalidiert oder verdraengt wird, ist der Lauf nach kurzer
  Gegenprobe als Konvergenz- beziehungsweise Requeue-Fehler abzubrechen. Das
  gilt auch bei einer ausdruecklich aufgehobenen Zeitgrenze.
- Fuer Phasen mit `planned > 0` und `canonical == 0` ist die First-Publish-Zeit
  des letzten gesunden Produktlaufs die verbindliche Vergleichsbasis. Ist sie
  erreicht und bleiben danach drei aufeinanderfolgende 10-Sekunden-Samples
  trotz fertiger/ready Arbeit, steigendem internem Churn und unbewegtem
  Head-of-Line weiterhin bei null, ist der Lauf als Nichtkonvergenzfehler zu
  beenden. Fehlt eine gesunde Vergleichsbasis, ist spaetestens nach drei
  Minuten ohne erste kanonische Publikation eine explizite Fehlerentscheidung
  anhand dieser Signale Pflicht.

Die ausfuehrlichen Projektvertraege in `ROADMAP.md`, `docs/CODEX_HANDOFF.md`,
`docs/TASKS.md` und den aktiven Performanceplaenen gelten zusaetzlich, soweit
sie diesem repositoryweiten Arbeitsvertrag nicht widersprechen. Eine
aktuelle ausdrueckliche Nutzeranweisung hat Vorrang.

## Aktueller Bring-up-Pfad

- Der dokumentierte Candidate-Resolution-Pfad KR-4985 bis KR-4991 und KR-4993
  bleibt die aktuelle Taskreihenfolge; KR-4992 bleibt der bedingte Folgezweig
  nach einem verfehlten KR-4981.
- Fuer jeden dieser Tasks gilt der projektweite Dreischritt:
  **implementieren -> betroffene Pfade reviewen und Findings schliessen ->
  direkt auf main pushen**.
- D1 und D2 bleiben ausdruecklich freizugebende reale Sonic-Diagnoseexporte.
  Sie sind Produktdiagnose, keine neue Testmatrix.
- KR-4993 bleibt ein abschliessender betroffener-Pfade-Gesamtreview vor dem
  naechsten Sonic-Produktgate. Er darf keine neue Testsuite, Matrix oder
  synthetische Ersatzabnahme erzeugen.
- KR-4982 und KR-4983 bleiben gestrichen und duerfen nur durch eine neue
  ausdrueckliche Nutzeranweisung reaktiviert werden.
