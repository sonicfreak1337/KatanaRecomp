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

Die ausfuehrlichen Projektvertraege in `docs/CODEX_HANDOFF.md` und
`docs/TASKS.md` gelten zusaetzlich. Bei einer ausdruecklichen aktuellen
Nutzeranweisung hat diese Vorrang.

## Verbindlicher Abschlussvertrag fuer den aktuellen Bring-up

- Die bestehende Roadmap wird ohne Scope-Erweiterung in ihrer festgelegten
  Reihenfolge abgearbeitet. Nicht genehmigte Nebenarbeiten, neue Tasks und
  Architekturumbauten sind verboten.
- Fuer jeden Roadmap-Task gilt genau diese Reihenfolge:
  **implementieren -> ausschliesslich P0/P1 der betroffenen Pfade reviewen ->
  alle bestaetigten P0/P1 gebuendelt fixen -> fokussiert verifizieren ->
  committen und pushen -> naechster Task**.
- Refactoring ist im gesamten Repository verboten, solange der Nutzer es nicht
  fuer einen konkret benannten Umbau ausdruecklich freigibt. Ein Review darf
  sonstige Befunde fuer spaeter notieren, aber daraus weder Arbeit noch neue
  Tasks ableiten.
- KR-4982 und KR-4983 (die beiden GPU-Analyseaufgaben) sind vorerst gestrichen
  und gehoeren nicht zum aktuellen Bring-up-Pfad. Sie duerfen nur durch eine
  neue ausdrueckliche Nutzeranweisung reaktiviert werden.
- KR-4984 bleibt ein eigener abschliessender Gesamtreview-Task. Er prueft und
  schliesst ausschliesslich P0/P1-Bugs; Refactoring, neue Features und neue
  Folgetasks sind auch dort verboten.
- Sobald die Soundness nach KR-4984 belegt ist, folgt ohne weitere
  Zwischenumbauten genau der vorgesehene Sonic-Produktbuild und -lauf.
- Aktuelle ausdrueckliche Reihenfolge: Nach Abschluss und Push von KR-4979
  folgt sofort ein Sonic-Produktbuild und -lauf; KR-4980 wartet bis danach.
