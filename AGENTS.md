# Verbindliche Arbeitsregeln fuer das gesamte Repository

Diese Regeln gelten fuer jeden automatisierten Bearbeiter und fuer jeden Task
in diesem Repository. Sie sind keine Empfehlung.

## Produkt vor Testinfrastruktur

- Das echte Endprodukt, der massgebliche Test und die einzige Produktabnahme
  ist fuer diesen Bring-up immer Sonic selbst: ein real erzeugter Port, sein
  normaler Programmlauf und ein echter Screenshot jenseits von SEGA -> Schwarz.
- Zwischenimplementierungen erhalten keine eigenen Testbuilds, Unit-Testlaeufe,
  synthetischen Fixtures, Stresslaeufe oder Testmatrizen. Ihre Fehler werden in
  den vorgeschriebenen P0/P1-Reviews der vollstaendigen betroffenen Pfade
  gefunden und gebuendelt geschlossen.
- Gebaut und ausgefuehrt wird der Sonic-Produktpfad. Ein anderer Lauf ist nur
  nach einer neuen ausdruecklichen Nutzerfreigabe fuer genau diesen benannten
  Lauf zulaessig.
- Performance wird primaer am realen End-to-End-Produktpfad gemessen.
  Synthetische Zeiten oder gruene Testmatrizen sind kein Ersatz fuer eine reale
  Kaltbuildzeit und keinen sichtbaren Programmlauf.

## Laufzeit und Ressourcen

- Kein gestarteter Prozess und keine einzelne Phase laeuft laenger als
  15 Minuten. Nur eine ausdrueckliche Nutzerfreigabe fuer genau einen benannten
  Lauf hebt diese Grenze voruebergehend auf.
- Ein abgelaufener oder abgebrochener Prozess wird mitsamt seinem Prozessbaum
  quiesziert, bevor ein Nachfolger startet.
- Fokussierte Builds und produktive Arbeit nutzen die verfuegbaren
  Hostressourcen parallel; Ein-Kern-Ausfuehrung ist kein akzeptabler Default.
- Potenziell lange Produktphasen melden spaetestens alle zehn Sekunden
  belastbaren Fortschritt beziehungsweise einen Heartbeat.
- Lange Prozesse werden so gestartet, dass ihre Ausgabe live sichtbar ist;
  ein nur am Ende ausgegebener gepufferter Log ist unzulaessig. Insbesondere
  darf `ctest --output-on-failure` nicht allein fuer einen potenziell langen
  Lauf verwendet werden.
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
  gilt auch bei einer ausdruecklich aufgehobenen Zeitgrenze; eine
  Timeoutfreigabe erlaubt keinen nachweislich divergierenden Lauf.
- Fuer Phasen mit `planned > 0` und `canonical == 0` ist die First-Publish-Zeit
  des letzten gesunden Produktlaufs die verbindliche Vergleichsbasis. Ist sie
  erreicht und bleiben danach drei aufeinanderfolgende 10-Sekunden-Samples
  trotz fertiger/ready Arbeit, steigendem internem Churn und unbewegtem
  Head-of-Line weiterhin bei null, ist der Lauf als Nichtkonvergenzfehler zu
  beenden. Ein Zustand wie `0/1191` nach 146 Minuten darf niemals als bloss
  langsame Arbeit weiterlaufen; er haette lange vorher abgebrochen werden
  muessen. Fehlt eine gesunde Vergleichsbasis, ist spaetestens nach drei
  Minuten ohne erste kanonische Publikation eine explizite Fehlerentscheidung
  anhand dieser Signale Pflicht.

Die ausfuehrlichen Projektvertraege in `docs/CODEX_HANDOFF.md` und
`docs/TASKS.md` gelten zusaetzlich. Bei einer ausdruecklichen aktuellen
Nutzeranweisung hat diese Vorrang.

## Verbindlicher Abschlussvertrag fuer den aktuellen Bring-up

- Die bestehende Roadmap wird ohne Scope-Erweiterung in ihrer festgelegten
  Reihenfolge abgearbeitet. Der vom Nutzer am 3. August 2026 ausschliesslich
  als Planung angeforderte Root-0-Kernpfad KR-4985 bis KR-4991 und KR-4993 ist
  die einzige vorgesehene neue Arbeit auf dem aktuellen P0-Pfad; KR-4992 ist
  nur der bedingte Folgezweig nach einem verfehlten KR-4981. Dieser
  Planungsauftrag
  genehmigt noch keine Implementierung, keinen Build und keinen Lauf; jeder
  Task benoetigt eine neue ausdrueckliche Nutzeranweisung. Weitere nicht
  genehmigte Nebenarbeiten, Tasks und Architekturumbauten sind verboten.
- Fuer jeden Roadmap-Task gilt genau diese Reihenfolge:
  **implementieren -> ausschliesslich P0/P1 der betroffenen Pfade reviewen ->
  alle bestaetigten P0/P1 gebuendelt fixen -> fokussiert verifizieren ->
  committen und pushen -> naechster Task**.
- Refactoring ist im gesamten Repository verboten, solange der Nutzer es nicht
  fuer einen konkret benannten Umbau ausdruecklich freigibt. Die in KR-4985
  bis KR-4993 exakt beschriebenen Context-, Interning-, Dependency- und
  Worklist-Umbauten werden erst mit einer neuen taskbezogenen Nutzeranweisung
  zu freigegebenem Scope; der Plan ist kein allgemeines Refactoringmandat. Ein
  Review darf sonstige Befunde fuer spaeter notieren, aber daraus weder
  Arbeit noch neue Tasks ableiten.
- KR-4982 und KR-4983 (die beiden GPU-Analyseaufgaben) sind vorerst gestrichen
  und gehoeren nicht zum aktuellen Bring-up-Pfad. Sie duerfen nur durch eine
  neue ausdrueckliche Nutzeranweisung reaktiviert werden.
- KR-4984 bleibt der historische Sourcegate-Abschluss vor dem fehlgeschlagenen
  v56-Performancebeleg. Nach den nun geplanten Umbauten uebernimmt KR-4993 die
  neue abschliessende Root-0-P0/P1-Gesamtpruefung. Refactoring, neue Features
  und neue Folgetasks ausserhalb des dokumentierten Plans sind auch dort
  verboten.
- Vor KR-4993 sind nur D1 in KR-4985 und D2 zu Beginn von KR-4991 als jeweils
  separat vom Nutzer freizugebende und auf Root 0 beziehungsweise die
  allgemeinen Zeit-/Stallgrenzen beschraenkte Diagnoseexporte zulaessig. Sie
  sind keine Produktnachweise.
- Sobald die Soundness nach KR-4993 belegt ist, folgt ohne weitere
  Zwischenumbauten der erste vorgesehene KR-4981-Sonic-Produktbuild und -lauf.
  KR-4992 darf nur nach dessen verfehltem Acht-Minuten-Gate und positivem
  Restkosten-/RAM-Gate aktiviert werden; danach sind KR-4993 und ein separat
  autorisierter KR-4981-Wiederholungslauf erneut Pflicht.
- Vorgesehene Reihenfolge nach jeweils neuer ausdruecklicher
  Implementierungsfreigabe: D1/KR-4985, KR-4986, nur positiv gegatete
  KR-4987 bis KR-4990, D2/KR-4991 und nur bei positivem G2 dessen
  Schedulerumbau, danach KR-4993 und der erste KR-4981-Produktlauf. KR-4992
  folgt nur ueber den oben beschriebenen Fehlgate-Zweig. Die Einzelheiten und
  negativen Stop/Go-Gates stehen in
  `docs/P0_ROOT0_CANDIDATE_RESOLUTION_PERFORMANCE.md`.
