# SH-4 Alpha-ISA-Vertrag

Der versionierte Alpha-ISA-Vertrag trennt Decoderabdeckung von tatsaechlich
behaupteter Semantik. `katana-recomp isa-report --json` ist die kanonische,
maschinenlesbare Darstellung. Vertragsversion 1 umfasst alle 65.536
16-Bit-Encodings, jede decodierte Instruktionsart und die vier Schichten
Decoder, Katana-IR, C++-Backend und Runtime.

## Zustandsmodell

- `supported`: Die benannte Semantik ist durch alle vier Schichten definiert.
- `restricted`: Die Instruktion ist decodier- und generierbar, besitzt aber die
  im Bericht benannte Runtime-Grenze.
- `rejected`: Es wird keine Semantik behauptet; Ausfuehrung darf nicht als
  erfolgreicher No-op fortgesetzt werden.

Eine Familie darf nur dann als `supported` erscheinen, wenn alle vier
Schichten denselben Zustand tragen. Jede eingeschraenkte oder abgelehnte
Familie nennt ihre Grenze und jede Familie besitzt eine stabile
Testanforderung.

## Aktuelle Matrix

| Familie | Decoder | IR | Backend | Runtime | Gesamt |
|---|---|---|---|---|---|
| Integer-Arithmetik und -Logik | supported | supported | supported | supported | supported |
| Speichertransfers und Adressierung | supported | supported | supported | supported | supported |
| Direkter und indirekter Kontrollfluss | supported | supported | supported | supported | supported |
| Status, Exceptions und Systemkontrolle | supported | supported | supported | restricted | restricted |
| Cache, Prefetch und Store Queues | supported | supported | supported | restricted | restricted |
| Fliesskomma | supported | supported | supported | restricted | restricted |
| Unbekannte/nicht implementierte Encodings | rejected | rejected | rejected | rejected | rejected |

Die konkreten Grenzen sind derzeit:

- Privilegverletzungen und der vollstaendige `SLEEP`-/Wakeup-Pfad fehlen noch.
- Operand-Cache-RAM und vollstaendige Cachekohaerenz sind nicht aktiviert.
- Vollstaendige SH-4-FPU-Ursachen-, Enable- und Sticky-Flag-Semantik fehlt.

KR-4502 und KR-4503 duerfen einen Zustand nur zusammen mit Decoder-, IR-,
Backend-, Runtime- und Erfolg/Grenze/Fehler-Tests anheben. Eine sinkende Zahl
unbekannter Opcodes allein ist kein Nachweis fuer Alpha-Unterstuetzung.

## Befehle

```powershell
.\build-current\katana-recomp.exe isa-report
.\build-current\katana-recomp.exe isa-report --json
```

Die JSON-Ausgabe enthaelt Familienzustand, Schichtzustand, Semantikvertrag,
Einschraenkung und Testanforderung sowie die Zuordnung jeder decodierten
Instruktionsart. Sie enthaelt keine titelbezogenen Daten oder Hostpfade.
