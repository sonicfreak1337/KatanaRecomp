# Task KR-XXXX - Titel

## Tasktyp

- [ ] Implementierungs-Task
- [ ] letzter Gate-Vorbereitungstask
- [ ] Phasen-Release-Gate

## Ziel

Kurze Beschreibung des fachlichen Ergebnisses.

## Abhaengigkeiten

- KR-XXXX

## Betroffene Schichten

- [ ] Decoder
- [ ] Disassembly
- [ ] Analyse
- [ ] IR
- [ ] Codegenerator
- [ ] Runtime
- [ ] Loader
- [ ] CLI
- [ ] Dokumentation

## Umfang

- Punkt 1
- Punkt 2
- Punkt 3

## Nicht im Umfang

- Punkt 1
- Punkt 2

## Akzeptanzkriterien

- [ ] Kriterium 1
- [ ] Kriterium 2
- [ ] Kriterium 3

## Tests

Bei einem Implementierungs-Task werden hier nur die spaeteren Anforderungen
gesammelt; Tests und Build werden noch nicht erstellt oder ausgefuehrt.

- [ ] erforderlicher Decoder-Test beschrieben
- [ ] erforderlicher IR-Test beschrieben
- [ ] erforderlicher Codegenerator-Test beschrieben
- [ ] erforderlicher End-to-End-Test beschrieben
- [ ] Grenzfall beschrieben
- [ ] Fehlerfall beschrieben

Nur beim letzten Gate-Vorbereitungstask:

- [ ] alle gesammelten Tests der Phase umgesetzt
- [ ] genau ein frischer Build in `build-current/` erfolgreich
- [ ] vollstaendige Regression und vorgesehene Audits erfolgreich
- [ ] reproduzierbarer Gate-Bericht erstellt
- [ ] vor dem Phasen-Release-Gate fuer das Nutzerreview gestoppt

Nur beim Phasen-Release-Gate:

- [ ] unveraenderter Gate-Bericht liegt vor
- [ ] ausdrueckliche Nutzerfreigabe liegt vor
- [ ] keine neue Semantik, Tests oder Builds im Gate-Task

## Dokumentation

- [ ] `docs/TASKS.md`
- [ ] `CHANGELOG.md`
- [ ] relevante Architektur- oder API-Dokumentation

## Abschluss

- dokumentierte Testanforderungen:
- Gate-Vorbereitung, falls zutreffend: Build-/Testergebnis und Gate-Bericht:
- Nutzerfreigabe, falls Release-Gate:
- bekannte Einschraenkungen:
- naechster Task:
