# Externe SH-4-Konformität

KatanaRecomp kann das unabhängig erzeugte Binärkorpus
`SingleStepTests/sh4` als externes Semantikorakel verwenden. Der Harness bleibt
ein statischer AOT-Pfad: Das Korpus wird beim Build gelesen, jede deduplizierte
Codeform durchläuft Decoder, Kontrollflussanalyse, IR-Lowering, Optimierung,
Verifikation, Produktpartitionierung und C++-Backend. CMake kompiliert die
deterministischen Quellshards anschließend in den Runner. Zur Testlaufzeit
werden weder SH-4-Opcodes interpretiert oder decodiert noch C++-Quellen
kompiliert.

## Gepinnte Referenz

- Repository: `https://github.com/SingleStepTests/sh4`
- Commit: `48975cb1a9569abb5a0cba587013ea54edf79100`
- Root-Tree: `b5ca6e7a14976dd3eae6ebb5a06b305da221ae09`
- Dateien: 233 × 500 Vektoren = 116.500 Vektoren
- Kanonischer Manifest-SHA-256:
  `155ddb446f00e6e4985ea0bb978cef8984e7835c864134b33d99e33af47b46c7`

Die Pin- und Smoke-Auswahl steht in
`tests/sh4_sst/corpus.lock.json`. Bekannte eng begrenzte Referenzlücken stehen
in `tests/sh4_sst/waivers.json`; jeder Eintrag ist an Commit, Datei und konkrete
Testindizes gebunden. Ein anderer Corpus-Commit, ein anderer Manifest-Hash,
eine beschädigte Datei oder ein stale Waiver beendet den Lauf als
Infrastruktur-/Corpusfehler.

Der gepinnte Corpus enthält 43 exakt indexierte `DIV1 Rn,Rn`-Fälle aus einer
alten Reicast-Referenzimplementierung, die das aliasierende `Rm` erst nach der
Verschiebung von `Rn` liest. Das widerspricht der Reihenfolge in Renesas
SH-4 Software Manual Rev.6.00, Abschnitt 9.19. Flycast korrigierte denselben
geerbten Fehler mit Commit `d92790e69f4033fe1f35d72be2b9b539457d5e5b`.
Diese Fälle bleiben deshalb sichtbar als
`not-applicable-reference-known-bug`; Katana übernimmt nicht die fehlerhafte
Oracle-Semantik.

Der Corpus ist MIT-lizenziert, Copyright (c) 2024 SingleStepTests. Er wird
nicht in KatanaRecomp kopiert. Lizenz- und Redistributionshinweise stehen in
`THIRD_PARTY_NOTICES.md`.

## Lokalen Checkout konfigurieren

Das Beschaffen des Corpus ist ein expliziter Vorbereitungsschritt außerhalb
von CMake und CTest. Danach arbeiten Build und Tests vollständig offline.

```powershell
git clone https://github.com/SingleStepTests/sh4 C:\src\SingleStepTests-sh4
git -C C:\src\SingleStepTests-sh4 checkout 48975cb1a9569abb5a0cba587013ea54edf79100

cmake -S . -B build-sst-smoke -G Ninja `
  -DCMAKE_BUILD_TYPE=Release `
  -DKATANA_ENABLE_SH4_SST=ON `
  -DKATANA_SH4_SST_ROOT=C:\src\SingleStepTests-sh4 `
  -DKATANA_SH4_SST_SCOPE=smoke
cmake --build build-sst-smoke --target katana-sh4-sst-runner
ctest --test-dir build-sst-smoke -R katana-sh4-sst --output-on-failure
```

`KATANA_ENABLE_SH4_SST` ist standardmäßig `OFF`. `smoke` verwendet eine feste
Auswahl repräsentativer Dateien und Indizes. `full` verarbeitet alle 116.500
Vektoren; dafür sollte ein eigener Release-Build verwendet werden:

```powershell
cmake -S . -B build-sst-full -G Ninja `
  -DCMAKE_BUILD_TYPE=Release `
  -DKATANA_ENABLE_SH4_SST=ON `
  -DKATANA_SH4_SST_ROOT=C:\src\SingleStepTests-sh4 `
  -DKATANA_SH4_SST_SCOPE=full
cmake --build build-sst-full --target katana-sh4-sst-runner
.\build-sst-full\katana-sh4-sst-runner.exe `
  --corpus-root C:\src\SingleStepTests-sh4 `
  --profile native-product-memory `
  --report-json .\build-sst-full\katana-sh4-sst-conformance.json
```

Der vollständige `native-product-memory`-Lauf ist das primäre Release-Gate.
Ein separater `flat-semantic-memory`-Lauf kann zusätzliche IR-/Backendsemantik
prüfen, bleibt aber ausdrücklich eine andere Evidenzklasse.

Der Runner unterstützt außerdem `--file`, `--case`, `--opcode`, `--family`,
`--profile`, `--fpu`, `--shard`, `--shard-count`, `--fail-fast` und
`--report-json`. CTest startet wenige stabile Prozessshards, niemals einen
Prozess oder eine Hostkompilierung pro Vektor.

## AOT- und Speichervertrag

Die Profile `Product` und `ExternalConformance` stammen aus derselben
versionierten Factory. Optimierungsoptionen, Partitionierungsgrenzen,
Single-Block-Ausführung, externe dynamische Dispatchgrenze und lokales
Block-Chaining sind semantisch identisch. Nur Namen, Beobachtungshooks und
Quellsharding unterscheiden sich.

Die ersten vier Corpus-Opcodes bilden das normale Vier-Instruktionsfenster.
Jede tatsächlich erwartete externe Fetch-Adresse erhält einen eigenen,
vorab kompilierten Catch-all-Block mit `opcodes[4]`. `opcodes[4]` liegt nie
pauschal bei `PC+8`. Ein nicht gebundenes Ziel endet als typisierter
`fail-unbound-target`.

Wenn der materialisierte Code direkt hinter dem vierten Referenz-Fetch
weitergeht, fügt ausschließlich der SST-Generator dort einen zusätzlichen
Basic-Block-Leader ein. Dieser Leader ist keine Funktionsevidenz. Weil das
Konformitätsprofil lokales Block-Chaining an der Plattformgrenze ablehnt, kehrt
der native Block damit exakt am Oracle-Horizont zurück; es werden weder
Instruktionen abgeschnitten noch Ausnahmen zur Ablaufsteuerung erfunden.

`cycles[].fetch_addr` ist das PC-, Kontrollfluss- und Delay-Slot-Orakel; nativer
Code liest zur Laufzeit keine Opcodebytes aus Gast-RAM. Der echte
`GuestMemoryAccessSink` beobachtet ausschließlich Datenzugriffe während der
Ausführung. Das Upstream-Traceformat speichert einen Datenzugriff im
Cycle-Eintrag nach dem Fetch der ausführenden Instruktion. Der Harness ordnet
ihn deshalb der unmittelbar vorherigen `fetch_addr` zu; daraus wird keine
Hardware-Pipeline- oder Taktgenauigkeit abgeleitet.

Die Ergebnisse der Speicherprofile bleiben getrennt:

- `native-product-memory` verwendet Produktadressübersetzung, strikte
  Ausrichtung und nur direkt anwendbares Dreamcast-Haupt-RAM. Nur dieses Profil
  ist externe Runtimeevidenz.
- `flat-semantic-memory` bildet das flache Referenzmodell für
  Adressberechnung, Breite, Sign-Extension und Registerupdates ab. Es belegt
  ausdrücklich keine MMU-, MMIO-, Alias-, Privileg- oder
  Alignment-Exception-Semantik. Der echte Katana-Gastspeicherpfad projiziert
  Segmentaliase auf physische Adressen. Sobald ein Vektor zwei verschiedene
  virtuelle Bytes über diese Projektion unterscheiden könnte, wird er deshalb
  geschlossen als `not-applicable-access-shape` klassifiziert statt als
  Flachspeicher-Evidenz ausgegeben.

8-Byte-FMOV-Zugriffe werden nicht aus zwei 32-Bit-Ereignissen
zusammenphantasiert, sondern als `not-applicable-access-shape` ausgewiesen.
FPU-Familien bleiben unabhängig vom Ergebnis `restricted`; ein
`upstream-compatible` Vergleich hebt keinen ISA-Status an.

## Interpreterfreier Link und Bericht

Der Runner linkt ausschließlich `katana_runtime_core`, die SST-Unterstützung
und die generierten nativen Objekte. Decoder und Diagnoseinterpreter sind nur
im Buildzeit-Generator beziehungsweise in getrennten Targets vorhanden. Ein
Member-, Symbol- und finaler Linker-Map-Audit lässt den Build bei Decoder-,
Interpreter-, Fallback- oder Diagnoseinterpreter-Symbolen scheitern. Die
Negativkontrolle linkt absichtlich einen Interpreter-Einstieg in ein separates
End-Executable und beweist, dass dessen Symbol beziehungsweise Map-Eintrag
erkannt wird.

Schon die SST-Konfiguration und vor jedem Generator- oder Runner-Build prüft ein
eigener Source-Identity-Guard den eingebetteten Katana-Commit gegen `HEAD` und
verlangt einen vollständig sauberen Index und Worktree. Ein Commitwechsel nach
der Konfiguration oder ein beliebiger Commit-Override kann dadurch keine
scheinbar aktuelle externe Evidence erzeugen. Normale Builds mit deaktiviertem
SST-Harness bleiben auch aus einem Source-Archiv ohne Git möglich; sie betten
dann einen Null-Commit ein, und der ISA-Importer markiert externe Evidence mit
`untrusted-build-source` als stale.

Der JSON-Bericht enthält Commit- und ABI-Basis, Compiler, Buildtyp,
Backendprofilversion, Scope, Speicher-/FPU-Profil, vollständige Nenner,
Klassifikationsgründe, Waiver, konkrete externe Opcodes und strukturierte erste
Gegenbeispiele. Er ist Evidenz für genau diesen Scope, nicht für vollständige
SH-4-Korrektheit.

`selection.complete_scope=true` ist nur bei einem ungefilterten, nicht vorzeitig
beendeten Lauf über exakt 65 Smoke- beziehungsweise 116.500 Full-Vektoren
zulässig. Datei-, Fall-, Opcode-, Familien- und Shardfilter sowie `--fail-fast`
werden im Bericht festgehalten; solche Reports bleiben reproduzierbar, werden
vom ISA-Importer aber mit `incomplete-scope` als stale markiert.

Ein bestandener vollständiger Bericht kann explizit in den ISA-Bericht
eingebunden werden:

```powershell
.\build-sst-full\katana-recomp.exe isa-report --json `
  --external-evidence .\build-sst-full\katana-sh4-sst-conformance.json
```

Die externe Evidenz bleibt getrennt vom deklarierten Vier-Schichten-Status und
wird bei abweichendem Katana-/Corpus-Commit, Manifest, ABI oder Backendprofil
als `stale` markiert. `docs/SH4_ALPHA_ISA.md` wird durch keinen Testlauf
automatisch geändert; eine Dokumentationsaktualisierung ist ein bewusster
separater Schritt nach einem vollständigen bestandenen Bericht.
