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
- `native-disc-analysis.katana-analysis`: identitaetsgebundener
  Analysecheckpoint, sobald die primaere statische Analyse cachebar ist;
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
nicht resumierbar. Ein fehlender Commit, veraenderte Artefakte oder eine
abweichende analysewirksame Disc-, Projekt-, Native-Port-, Payload-,
Latent-Root-, Image-, IR-, Analyzer-, Cache- oder ABI-Identitaet brechen
fail-closed ab. Katana materialisiert die aktuelle GDI erneut, prueft die
gebundenen Eingaben und IR-Identitaeten und fuehrt Produktadmission,
Hardwareclosure, World-Projektion und Agentenentscheidung unter dem aktuellen
Code erneut aus. Reine Agenten-/World-Projektionsaenderungen duerfen deshalb
den gebundenen Analysecheckpoint wiederverwenden, ohne alte Admission als
autoritative Wahrheit zu uebernehmen. Bis der neue Ledger-Commit sichtbar
ist, bleiben ersetzte Artefakte als explizite Rollbackgeneration gesichert;
ein Fehler bei World, Report, Archiv oder Ledger stellt die vorherige
committed Generation wieder her.

Der Resume-Aufruf wiederholt ausschliesslich die analysewirksamen CLI-Eingaben
des Checkpoints. Aus dem Native-Port-Manifest abgeleitete AOT-Resume-Entries
werden nicht zusaetzlich als `--native-aot-resume-entry` angegeben: explizite
und manifestabgeleitete Entries koennen dieselbe finale Rootmenge erzeugen,
besitzen aber absichtlich verschiedene Analysevertragsidentitaeten.

Eine ausschliessliche Native-Port-/Provideraenderung darf den statischen
CFA-/FVA-Checkpoint admission-only weiterverwenden. Katana rekonstruiert dazu
den alten Analysevertrag mit den gespeicherten Native-Port-Identitaeten und
verlangt fuer alle uebrigen Vertragsfelder exakte Gleichheit. Danach werden
aktuelle GDI, Image-Key, IR, externe Roots und Latent-Sourcebindungen erneut
geprueft. Der analyzergebundene kombinierte Hardware-Audit bleibt Teil des
validierten Checkpoints; Native-Port-Programindex, Closure, Gaps,
Replacement-Reachability, World und Entscheidung werden dagegen unter der
neuen Definition frisch berechnet. Der alte World-/Ledger-Key ist dabei nur
die gebundene Eingabegeneration. Publiziert wird stets eine neue Generation
mit der aktuellen Native-Port-Identitaet. Feldabweichungen werden als
feldgenaue `admission-replay-*`-Rejects gemeldet.

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

`next-analysis-task` Schema 2 behaelt den hoechst priorisierten Primary-Task
und liefert zusaetzlich einen bounded `tasks`-Batch mit bis zu drei
Frontiers. Batch-Eintraege besitzen verschiedene Owner, damit parallele
Agenten nicht dieselbe Provider-/Ownergrenze editieren. Die Reihenfolge bleibt
deterministisch; echte offene Aufgaben schlagen blockierte Aufgaben. Innerhalb
desselben Frontierzustands werden echte Read-Rollen priorisiert, waehrend eine
blosse Same-Owner-Beziehung keine Abhaengigkeit erfindet.

Stabile IDs sind in derselben Welt kollisionsgeprueft. Unbekannte Enumwerte,
ueberlaufende Budgets, unvollstaendige Beziehungen und widerspruechliche
Identitaeten machen das Artefakt ungueltig.

Hardwaretasks werden bounded und semantisch homogen gruppiert. Eine Aufgabe
nennt den eindeutigen Owner, die konkrete Operation samt Registerfamilie, den
erwarteten Native-Providervertrag und den fehlenden Proof. Wenn eine
identity-bound Whole-Owner-Ersetzung zulaessig ist, enthaelt die Aufgabe
zusaetzlich Guest-Entry, exakte Boundarygroesse, Boundary-Proof, Code-SHA-256,
vorgeschlagenes Hooksymbol, aktuelle Bindung und jede Site-zu-Hook-Zuordnung.
Fehlt eine exakte Boundary, wird dies explizit als Blocker ausgegeben; Katana
raet weder Groesse noch Providersemantik. Ebenso bleibt ein fehlender
ABI-/Register-/State-/Side-Effect-Beweis als
`provider-result-or-state-proof=missing` sichtbar; ein Agent darf daraus
keine Providersemantik erfinden.

Ein Whole-Owner-Hook ohne exakte Boundary, Codeidentitaet oder geschlossene
Site-Coverage ist `Blocked`, nicht `Open`. Sein `missing_proof` benennt die
fehlende Admissionsvoraussetzung. Dadurch priorisiert `next-analysis-task`
keine scheinbar offene Providerimplementierung, die Katana anschliessend
selbst nicht identitaetsgebunden zulassen koennte.

Dasselbe gilt, wenn der vollstaendige Owner-Semantikvertrag selbst `partial`
oder `unavailable` ist. Katana darf weder einen neuen Whole-Owner-Hook noch das
Upgrade eines vorhandenen identity-bound Bring-up-Providers als direkt
implementierbaren Auftrag ausgeben: Ohne vollstaendigen Whole-Owner-Vertrag
waere die geforderte Providergleichheit nicht statisch nachpruefbar. Der
Frontier bleibt mit `whole-owner-semantic-contract-partial` beziehungsweise
`native-provider-owner-semantic-contract-partial` sichtbar, wird aber als
`Blocked` hinter tatsaechlich offenen Aufgaben eingeordnet.

Jede Hardwareaufgabe enthaelt hoechstens vier Sites. Fuer jede Site liefert
Katana einen bounded Rueckwaertsslice der tatsaechlichen
Registerproduzenten: maximal vier CFG-Pfade, acht Bloecke und 24 relevante
Instruktionen. PC-relative Literale bleiben mit ihrer Snapshot-Autoritaet
gekennzeichnet. Alle statischen Vorgaengerpfade werden getrennt ausgewiesen;
input-erhaltende Loop-Backedges duerfen auf den bewiesenen nichtzyklischen
Ingress zurueckgefuehrt werden. Ein wertveraendernder Zyklus, Owner-Live-in,
offener Kontrollfluss oder Budgetueberlauf bleibt dagegen ausdruecklich
`partial`. So erhaelt ein kleiner Agent konkrete Werte und Reihenfolge, ohne
dass Katana fehlende Disassembly als Unerreichbarkeits- oder
Completeness-Beweis missversteht.

Bei einem identity-bound Provider, der den Produktvertrag noch nicht
schliesst, markiert Katana statisch klassifizierte Hardware-Reads als
`native-provider-input-read`. Der Agentselektor priorisiert diese bounded
Result-/State-Aufgaben vor anderen gleich schweren Hardwaretasks. Weitere
Operationen desselben Owners erhalten lediglich einen unverbindlichen
Prioritaetshinweis: Ohne einen eigenen Datenflussbeweis werden sie weder als
abhaengig noch als `Candidate` blockiert. Die Readrolle ist typisiert aus dem
Hardware-Audit abgeleitet; Adressen oder Titelnamen beeinflussen die Prioritaet
nicht.

Protokollregister werden nicht automatisch als API des nativen Hostbackends
missverstanden. Fuer einen `PVR.SOFTRESET`-Write benennt Katana beispielsweise
den title-seitigen Native-Grafikadapter als Provider-Layer und
`NativePortGraphicsDevice` nur als GPU-Hostdienst fuer Vertices, Texturen und
Draw-State. Die Aufgabe fordert getrennt den bounded Render-Reset-State-Proof
und den guest-sichtbaren Hook-Result-/CPU-State-Proof. Sie verbietet zugleich,
dem nativen Grafikdienst ein PVR-Registermodell oder einen TA-Paketparser
hinzuzufuegen. Ein vorhandener `BringUpProbe` bleibt sichtbar und darf erst
nach diesen Beweisen zu `Required` hochgestuft werden.

Fuer bounded, linear geschlossene Owner mit hoechstens 16 Bloecken und 64
Instruktionen liefert die Aufgabe zusaetzlich eine fail-closed symbolische
Stateprojektion. `owner-state-transfer` beschreibt die nachweisbaren
Register-/Statusausgaenge, `owner-write-effects` die geordneten 32-Bit-Writes
einschliesslich der vom Hardware-Audit belegten Region und Registerfamilie.
Snapshot-Literale bleiben dabei explizit revalidierungspflichtig. Nichtlineare
Kontrollfluesse, unbekannte Operationen oder ueberschrittene Textbudgets
erzeugen `owner-state-projection=unavailable` statt einer geratenen Semantik.

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

## Nachgewiesene inkrementelle Analyse

Der erste Sonic-Adventure-PAL-v1.003-Nachweis fuer `v0.49.2` lief kalt in
`342,230 s`. Derselbe gebundene Output wurde anschliessend mit `--resume` in
`45,758 s` aktualisiert. Der Report belegt dabei
`analysis_artifact_cache_hit=true`, `boot_analysis_cache_hit=true` und
`boot_analysis_pipeline_runs=0`; CFA/FVA wurden nicht erneut ausgefuehrt.
Der Ledger misst fuer den Analyseanteil `336.588 -> 39.925 ms`. Der
resumierte Stand umfasst `5.667` primaere und `6.284` kombinierte Funktionen,
ein Latent-Modul, `89` externe Roots, `5` native Resume-Entries, `240`
Hardware-Sites und `167` offene Hardware-Gaps. Die Entscheidung bleibt
ehrlich `continue_static_iteration`; es wurde kein Port exportiert.

Nach dem ersten identity-bound Providerdelta lief die admission-only
Neubewertung in `46,150 s` weiterhin mit
`boot_analysis_pipeline_runs=0`. Sie behielt `5.667`/`6.284` Funktionen und
`240` Hardware-Sites bei, reduzierte die offenen Hardware-Gaps auf `161` und
die handlungsfaehigen Frontiers auf `285`. Damit misst der Workflow
Providerfortschritt inkrementell, ohne den statischen Whole-Disc-Analyzer kalt
neu zu starten.

## Cache- und Exportgrenze

Das `.katana-analysis`-Archiv ist derzeit ein privater Analysecheckpoint fuer
`analyze-port --resume`. Der produktive Whole-Disc-Export ueberspringt die
Analyse nicht auf Grundlage dieses Archivs: Der aktuelle Vertrag kann noch
nicht positiv beweisen, dass keine Callback-, Target- oder Hardware-Owner-
Evidenz ausgelassen wurde. Positive Produktwiederverwendung bleibt
fail-closed, bis ein vollstaendiger Completeness-Beweis existiert.

Der eigentliche Export wird erst ausgefuehrt, wenn die Agentenentscheidung
`BuildPort` lautet. Laufzeitbeobachtung ersetzt dieses Gate nicht.
