# Verbindliche Arbeitsregeln fuer das gesamte Repository

Diese Regeln gelten fuer jeden automatisierten Bearbeiter und fuer jeden Task
in diesem Repository. Sie sind keine Empfehlung.

## Produkt vor Testinfrastruktur

- Das echte Endprodukt ist der massgebliche Test und die einzige
  Produktabnahme. Fuer den aktuellen Bring-up ist das der reale erzeugte Port
  mit seinem normalen Programmlauf.
- Builds, Unit-Tests, synthetische Fixtures, Stresslaeufe und Testmatrizen sind
  nur eng begrenzte Diagnosewerkzeuge. Sie ersetzen niemals den Produktlauf.
- Ausserhalb des Endprodukts wird nur der kleinste bereits vorhandene Compile-
  oder Regression-Check ausgefuehrt, der zum Absichern einer konkret
  geaenderten oder nachweislich fehlerhaften Stelle zwingend erforderlich ist.
- Keine breite Suite, vorsorgliche Regression, synthetische Matrix, Fuzz-
  Kampagne oder neue Testinfrastruktur ohne einen konkret belegten
  Produktblocker, den ein kleinerer Check nicht abdecken kann.
- Pro Implementierungsschritt gibt es hoechstens einen fokussierten Build und
  einen engen Check. Eine Wiederholung ist nur nach einem echten Fehler dieses
  Checks zulaessig und bleibt auf den fehlgeschlagenen Pfad beschraenkt. Danach
  geht die Arbeit sofort zum Produkt- beziehungsweise Performancepfad zurueck.
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
