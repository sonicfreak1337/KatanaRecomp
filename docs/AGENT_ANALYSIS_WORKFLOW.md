# Agentischer Native-Disc-Analyseworkflow

Der agentische Workflow trennt beweisende statische Analyse, beobachtete
Produktruntime-Frontiers und den eigentlichen Portexport. Er erzeugt keinen
Interpreter-, JIT- oder Runtime-SH-4-Pfad.

## Analyse starten

```powershell
katana-recomp analyze-port .\disc\game.gdi `
  --output .\private\analysis-session `
  --target-name game `
  --game-project .\private\game.katana-game-project `
  --native-port-definition .\private\game.katana-native-port
```

Der Lauf publiziert transaktional:

- `materialization-world.katana-world`: maschinenlesbare, gebundene Welt;
- `materialization-world.json`: begrenzte menschlich lesbare Sicht;
- `native-disc-analysis.json`: Analysebericht und Agentenentscheidung;
- optional `native-disc-analysis.katana-analysis`: identitaetsgebundenes
  Analysearchiv, falls der vollstaendige positive Vertrag publizierbar ist;
- `.katana/agent/session.jsonl`: Session-Ledger Schema 3.

Das Ledger besitzt einen terminalen Commitrecord und bindet SHA-256 aller
publizierten Artefakte. `--resume` akzeptiert nur eine vollstaendige letzte
Transaktion mit exakt passenden Dateien und Identitaeten:

```powershell
katana-recomp analyze-port .\disc\game.gdi `
  --output .\private\analysis-session `
  --target-name game `
  --game-project .\private\game.katana-game-project `
  --native-port-definition .\private\game.katana-native-port `
  --resume
```

Legacy-Ledger duerfen fuer historische Zeitwerte gelesen werden, sind aber
nicht resumierbar. Ein fehlender Commit, veraenderte Artefakte, fremde Disc-,
Projekt-, Native-Port-, Analyzer- oder Codegenidentitaet brechen fail-closed
ab. Bis der neue Ledger-Commit sichtbar ist, bleiben ersetzte Artefakte als
explizite Rollbackgeneration gesichert; ein Fehler bei World, Report, Archiv
oder Ledger stellt die vorherige committed Generation wieder her.

## Naechste Arbeitseinheit und Evidenz

```powershell
katana-recomp next-analysis-task `
  --analysis-artifact .\private\analysis-session\materialization-world.katana-world `
  --format agent-json

katana-recomp explain `
  --analysis-artifact .\private\analysis-session\materialization-world.katana-world `
  --frontier <stabile-ID> `
  --format agent-json

katana-recomp diff-analysis `
  --before .\private\before\materialization-world.katana-world `
  --after .\private\after\materialization-world.katana-world `
  --format agent-json
```

Stabile IDs sind in derselben Welt kollisionsgeprueft. Unbekannte Enumwerte,
ueberlaufende Budgets, unvollstaendige Beziehungen und widerspruechliche
Identitaeten machen das Artefakt ungueltig.

Die Entscheidung `BuildPort` bedeutet: alle handlungsfaehigen Frontiers sind
durch immutable, identitaetsgebundene statische Evidenz geschlossen oder
explizit verworfen. `ObservedHint` und `RuntimeObservation` sind niemals ein
statischer Closure-Beweis. `ExplicitRejection` ist terminal und wird nicht
erneut als Aufgabe angeboten.

## Runtime-Frontier importieren

Ein Produktstop darf genau eine gebundene Frontier liefern. Der Log beginnt
mit `KATANA_RUNTIME_FRONTIER_BINDING` und enthaelt danach
`KATANA_RUNTIME_FRONTIER`. Die Bindung umfasst Content-, Bootbyte-, Projekt-,
Analysearchiv-, Analysevertrags- und Implementierungsidentitaet. Der Import ist
nur zusammen mit einem validierten Resume erlaubt:

```powershell
katana-recomp analyze-port .\disc\game.gdi `
  --output .\private\analysis-session `
  --target-name game `
  --game-project .\private\game.katana-game-project `
  --native-port-definition .\private\game.katana-native-port `
  --resume `
  --import-runtime-frontier .\private\game.stderr.log
```

Der Import ist streng, begrenzt und transaktional. Er uebernimmt ausschliesslich
die beobachtete Adresse als `ObservedHint`; er erzeugt weder eine CFG-Kante
noch einen AOT-Root oder Hardware-Closure. Erst ein spaeterer statischer,
immutable Identitaetsbeweis darf die Frontier schliessen.

## Cache- und Exportgrenze

Das optionale `.katana-analysis`-Archiv ist derzeit ein privates
Analyseartefakt. Der produktive Whole-Disc-Export ueberspringt die Analyse
nicht auf Grundlage dieses Archivs: Der aktuelle Vertrag kann noch nicht
positiv beweisen, dass keine Callback-, Target- oder Hardware-Owner-Evidenz
ausgelassen wurde. Katana serialisiert deshalb beim normalen Produktlauf auch
kein unbrauchbares 256-MiB-Archiv. Positive Produktwiederverwendung bleibt
fail-closed, bis ein vollstaendiger Completeness-Beweis existiert.

Der eigentliche Export wird erst ausgefuehrt, wenn die Agentenentscheidung
`BuildPort` lautet. Laufzeitbeobachtung ersetzt dieses Gate nicht.
