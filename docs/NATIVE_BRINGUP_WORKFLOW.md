# Native Bring-up und strikter Produktworkflow

Status: verbindlicher Entwicklungsvertrag.

KatanaRecomp trennt zwei Schleifen, die unterschiedliche Fragen beantworten:

- `strict-product` beweist, dass ein auslieferbarer Port vollstaendig und
  fail-closed ist.
- `native-bringup` findet mit demselben vorkompilierten Gastprogramm schnell
  den naechsten realen Produktfehler.

Der Bring-up-Modus ist kein lockeres Produktprofil. Er lockert ausschliesslich
die geforderte statische Vollstaendigkeit, niemals die Sicherheit der
ausgefuehrten Maschine. Es gibt in beiden Modi keinen Interpreter, keinen JIT,
keine Runtime-Dekodierung, keine Gast-CPU- oder PVR-Emulation und kein Raten
von Zieladressen.

## Artefaktgrenze

Der Entwicklungsstand wird in zwei unabhaengig erneuerbare Teile getrennt:

```text
game.aotpack
  vorkompilierte SH-4-Bloecke und Funktionen
  Modul-/Overlay-Identitaeten und Generationen
  Block-, Funktions- und Guest-PC-Index
  Relocations, Sourcemap und ABI-Identitaet
  native Bring-up-Allowlist

katana-runtime + title-adapter + title-manifest
  native Grafik, Audio, Movie, Datei, Eingabe und Save
  titelgebundene Provider, Hooks und Patches
  Replay-, Witness- und Crashdiagnostik
```

Der AOT-Pack bleibt unveraendert, solange Disc-/Imagebytes, analysierte
Funktionsgrenzen, CFG, Overlaymenge, Patchstellen oder eine fuer generierten
Code relevante ABI unveraendert bleiben. Runtime-, Adapter-, Renderer-,
Provider- und Diagnostikaenderungen bauen nur ihren inkrementellen Teil neu.
Jedes Artefakt traegt eine stabile Inhaltsidentitaet; ein Lauf protokolliert
Pack-, Runtime-, Adapter-, Manifest- und Replay-Identitaet.

## Cycle-Manifest und Freeze

Jeder Makro- und Mikrozyklus wird vor seiner Implementierung durch ein
maschinenlesbares Manifest eingefroren. Das Manifest bindet mindestens:

```json
{
  "cycle": "rNNN-product-M",
  "workflow": "macro|micro",
  "world_sha256": "...",
  "aot_pack_sha256": "...",
  "runtime_commit_or_source_identity": "...",
  "replay_set_sha256": "...",
  "replays": ["r1", "r2", "r3", "r4", "r5", "r6"],
  "performance_target": "...",
  "graphics_root_cause_cluster": "...",
  "product_build_budget": 1,
  "analysis_required": false
}
```

Nur Findings mit passender World-, Pack-, Source- und Replay-Reachability
duerfen in diesen Batch aufgenommen werden. Nach dem Freeze eintreffende
Arbeit gehoert in den naechsten Zyklus. Dadurch bleibt das Ein-Build-Budget
beweisbar und der Batch kann nicht waehrend der Implementierung unbegrenzt
wachsen.

## Deterministisches Authoring-Artefakt

Die private Allowlist wird nicht aus einem Runtime-Log gelernt. Ein
versioniertes JSONL-Authoring-Dokument enthaelt genau einen Header und danach
explizit reviewte Evidence-Records. Der Header bindet mindestens Projekt-ID,
Projektversion, committed Analysis-Generation, die SHA-256-Identitaet des
vollstaendigen AOT-Packs und dessen nicht-null Generation. Jeder Record bindet
Source-Owner, terminalen Source-Block, Callsite samt Delay-Slot, Transferart,
Fortsetzung, Target-Block, beide Owner, exakte Codeidentitaeten sowie Image-,
Modul- und Generationsidentitaeten.

Die executable v1-Implementierung ist absichtlich enger als das allgemeine
Artefaktformat: Source und Target muessen beide dem unveraenderlichen
`primary`-Image und seiner Bootidentitaet angehoeren. PRS-, Overlay-,
RuntimeImage- und Loaded-AOT-Records werden exportseitig abgelehnt, bis der
Runtime-Request eine aktive ModuleInstance-Identitaet samt Lifecycle-
Generation gegen den jeweiligen Binder validieren kann. Die globale
AOT-Packgeneration darf diese Modulinstanzpruefung nicht ersetzen.

Das kanonische private Binaerartefakt wird deterministisch erzeugt mit:

```text
katana-recomp author-native-bringup <Authoring.jsonl> \
  --output <private.katana-native-bringup>
```

Der Writer sortiert Records kanonisch, verwirft Duplikate und Konflikte und
publiziert exklusiv und atomar ohne Symlink-/Reparse-Following. Derselbe
Record-Satz erzeugt unabhaengig von der JSONL-Reihenfolge dieselbe
Artefaktidentitaet. Der Bring-up-Export akzeptiert das Artefakt nur zusammen
mit einer committed Analysis-Generation und nur wenn Projekt-ID,
Projektversion, Analysis-, Authoring- und Whole-Pack-Identitaet sowie die
Packgeneration exakt passen:

```text
katana-recomp port <Quelle.gdi> ... \
  --analysis-generation <committed-generation> \
  --native-execution-profile native-bringup \
  --native-bringup-allowlist <private.katana-native-bringup>
```

`strict-product` verbietet die Allowlist bereits an der CLI-Grenze und sieht
sie weder im Export noch in einer Cacheidentitaet. `Candidate`-Records werden
nur dann in die nicht releasefaehige executable Sicht uebernommen, wenn der
Exporter alle oben genannten Execution-Safety-Bindungen unabhaengig gegen den
aktuellen ProgramIndex, die IR-Bloecke und den unveraenderten AOT-Pack
revalidiert. Ihr `missing_proof` bleibt offen. Nur `Proven` darf zusaetzlich
`strict_proof_admitted=true` tragen.

## Der Makrozyklus: `strict-product`

```text
vollstaendige statische Analyse
  -> Closure und Providervertraege
  -> AOT-Code und bekannte Overlays
  -> game.aotpack + native Allowlist
  -> Produktbuild und Produktgate
```

Die grosse Schleife ist erforderlich, wenn mindestens eine der folgenden
Bedingungen eintritt:

- `UnknownCompiledTarget` oder ein Ziel ausserhalb des aktiven AOT-Packs;
- unbekannte Image-, Modul- oder Overlayidentitaet;
- fehlender Block, Function-Entry, Funktionsumfang oder CFG-Pfad;
- geaenderte Disc-/Imagebytes, Funktionsgrenzen, Jump-/Switchtabellen,
  Roots, Overlaydefinitionen oder AOT-wirksame Patches;
- geaenderte Instruktionssemantik, Codegen-ABI oder AOT-Datenformat;
- eine bestaetigte Evidence-Promotion, die AOT-Abdeckung oder Closure
  veraendert;
- Vorbereitung eines strikten Produkt- oder Releasegates.

Ein teurer Analyzerlauf wird nicht allein deshalb gestartet, weil Runtime,
Adapter, Grafik, Audio, Movie, Eingabe, Save, Dateisystem, Hosttiming oder
Diagnostik geaendert wurden.

Jede tatsaechlich gestartete und erfolgreich publizierte grosse Analyse endet
mit einem verpflichtenden read-only Frontier-Gate: Der Orchestrator erzeugt
aus der neuen World einen deterministischen `next-analysis-task`-Pool. Die
Positionen werden nach Owner, Provider, Frontierart und betroffenen Dateien
geclustert und in exklusive Pakete von hoechstens sechs Positionen auf die
bestehenden Egg-Fleet-Tasks verteilt. Dieselbe Ownerfamilie bleibt moeglichst
beim selben Task; nicht zusammenhaengende Poolpositionen sind erlaubt, ihre
Authority und Frontier-IDs bleiben aber explizit und verlustfrei. Die Fleet
vergleicht die neue Authority mit ihrem letzten Handoff und meldet Proof-,
Scope-, Kollisions- und Multi-Close-Befunde zurueck. Replay-Sammlung darf
parallel laufen; der eine Produktbuild wartet auf die Reconciliation aller
Handoffs und relevanten A-Fixes. Das Gate ersetzt weder den ersten Replay-
Stop als konkreten Implementierungsauftrag noch erlaubt es Runtime-Evidence
als statische Closure.

Der Fleet-Handoff ist maschinenlesbar und bindet World-/Pack-SHA, Task-IDs,
Root-Cause-Key, betroffene Dateien, Collision-Key, Replay-Reachability,
Multi-Close-Set, Klassifikation und Acceptance:

- `A`: aktuell bewiesen und im eingefrorenen Zyklus erreichbar; sofort
  umsetzen.
- `B`: bestaetigt, aber fuer den aktuellen Freeze nicht erreicht oder durch
  eine andere Voraussetzung blockiert; nicht Teil des Batches.
- `C`: stale, duplicate, bereits geschlossen, falsch interpretiert,
  unzureichend bewiesen oder ausserhalb des aktuellen Produkts; nicht
  implementieren.

## Der Mikrozyklus: `native-bringup`

```text
identischen AOT-Pack laden
  -> vorhandenes Replay ausfuehren
  -> erste Divergenz oder ersten typisierten Stop bestimmen
  -> kleinsten konkreten Task ableiten
  -> Runtime/Adapter/Manifest inkrementell bauen
  -> mit demselben AOT-Pack dasselbe Replay wiederholen
```

Die kleine Schleife ist fuer Produkt-Bring-up verbindlich. Ein spaeter
Folgecrash ist nachrangig, sobald eine fruehere Zustandsabweichung bekannt ist.
Tasks entstehen aus der ersten reproduzierbaren Divergenz, einem exakten
Contract-Stop oder einem Witness, nicht automatisch aus der gesamten offenen
Analyzer-Frontier.

Jeder Zyklus liefert genau eine kleine, isoliert reviewbare und ausgefuehrte
Produktruntime-Performanceverbesserung. Reine Instrumentierung sowie
Analyzer-, Export-, Graph-, Cache-, Ninja- oder Buildsystemarbeit erfuellt
diese Pflicht nicht. Der Fix wird mit dem normalen Batch gebaut, mit denselben
sechs Replays gemessen und nur angenommen, wenn kein Replay regressiert. Es
gibt weder einen Zusatzbuild noch eine reine Messrunde; Proof-Gates und
Fail-closed-Semantik bleiben unveraendert.

Jeder Zyklus schliesst ausserdem mindestens einen kausal gebuendelten
Grafik-Root-Cause-Cluster: fehlende Submission, verschwindende Geometrie,
falsches Asset, falscher Renderstate, falscher Shadervertrag oder falsche
Reihenfolge. Ein Dual-Close darf Grafik- und Performancepflicht gemeinsam
erfuellen.

Crashsammlung nutzt hoechstens zwei parallele Produktprozesse. Frametiming-
und Performancewerte stammen ausschliesslich aus nicht konkurrierenden
Laeufen mit Grafikdiagnostik `Off`; Breadcrumbs werden nur fuer den ersten
relevanten Grafikpfad aktiviert.

### Priorisierte Produktruntime-Folge

Solange die Replayaggregate keinen klar groesseren Hotspot zeigen, gilt fuer
die naechsten Zyklen diese Reihenfolge:

1. exportseitig indexierter NativeBringup-Preflight ohne globale lineare
   Allowlistsuche, optional mit kleinem generationgebundenem monomorphem
   Cache;
2. Immutable-Write-Page-Filter mit billigem negativem Seitentest vor der
   exakten Rangepruefung;
3. Fog-LUT nur fuer Lookup-Modi validieren und Drawstate/Fogtable versiegeln;
4. Frame-Upload-Arena fuer wenige grosse Vertex-/Index-Maps bei unveraenderter
   Drawreihenfolge;
5. bei Smooth Shading nur Small-Triangle-Indizes filtern und persistente
   Flat-Last-Varianten einmalig erzeugen;
6. Constant-Buffer-Ring beziehungsweise begrenzten Dynamic-Buffer-Pool statt
   einzelner Updates pro Draw verwenden.

Jeder Punkt wird einzeln im Cycle-Manifest benannt, mit dem ohnehin
erforderlichen Produktbinary ausgefuehrt und anhand der sechs Replays
akzeptiert oder verworfen.

Replays sind stille, unsichtbare, native Produktlaeufe. Redundante Szenarien,
die denselben frueheren Checkpoint langsamer erreichen, gehoeren nicht in den
Standardgurt. Die Suite misst mindestens:

- weitesten stabilen Produktmeilenstein und Frame;
- erste Divergenz gegen eine gebundene Baseline;
- neue oder bekannte Crash-/Stop-Signatur;
- Laufzeit sowie die Identitaeten aller verwendeten Artefakte;
- ob eine AOT-Luecke die Rueckkehr in die grosse Schleife erzwingt.

### Billige Grafikdiagnostik

Der Renderer startet immer im Modus `Off`. Fuer die schrittweise Eingrenzung
einer Grafikabweichung stehen vier explizite Modi zur Verfuegung:

```text
KATANA_NATIVE_GRAPHICS_DIAGNOSTICS_MODE=off
KATANA_NATIVE_GRAPHICS_DIAGNOSTICS_MODE=digest
KATANA_NATIVE_GRAPHICS_DIAGNOSTICS_MODE=breadcrumbs
KATANA_NATIVE_GRAPHICS_DIAGNOSTICS_MODE=armed-capture
```

`Digest` fuehrt pro Draw nur feste Integer-Mixes aus und publiziert den
laufenden Wert im `NativePortGraphicsSnapshot`. `Breadcrumbs` und
`ArmedCapture` benoetigen
`KATANA_NATIVE_GRAPHICS_DIAGNOSTICS_DIRECTORY`; ihre vorallokierte Ringgroesse
wird optional mit `KATANA_NATIVE_GRAPHICS_DIAGNOSTICS_CAPACITY` begrenzt.
Breadcrumbs enthalten ausschliesslich numerische Origin-, Intent-, Modell-,
Material-, Texlist-, Resolver-, Epoch-, Last-Writer- und Resource-Provenienz.
Materialisierte Texturen tragen zusaetzlich eine einmalig beim Decode/Upload
berechnete SHA-256-Identitaet des versionierten RGBA8-Top-Level-und-Mip-
Payloads sowie Source-Format, Extent, Mip-Anzahl und Archivordinal. Diese
Identitaet wird im Breadcrumb nur kopiert bzw. in den bestehenden Integer-
Digest gemischt; es gibt weder Hashing noch Datei-I/O im Draw-Hotpath.
Der Ring wartet bei Ueberlauf nicht, sondern ueberschreibt den aeltesten Record
und zaehlt den Drop.

Breadcrumb-Persistenz darf den Renderthread nicht periodisch synchron auf
Dateischreib-, Flush-, Close- oder atomare Replace-Operationen warten lassen.
Der Renderthread uebergibt hoechstens einen vorallokierten Snapshot an einen
freien Worker-Slot; alternativ wird nur bei Crash, typisiertem Stop,
Replayende oder explizitem Flush persistiert. `ArmedCapture` verwendet einen
begrenzten Staging-/Query-Ring. GPU-Completion wird auf dem Renderthread nur
abgefragt; Pixelkonvertierung und Dateiausgabe laufen ausserhalb des
Draw-Hotpaths.

Entscheidungen, die bereits im Titeladapter vor Erzeugung eines
`NativePortDrawPacket` verwerfen, brauchen einen separaten rein numerischen
Diagnosesink. Er bindet Origin-, Modell-, Material- und Primitive-Identitaet
sowie `Skipped` und einen festen Reason-Code, ist vorallokiert, besitzt keine
Admissionwirkung und enthaelt weder Strings noch Titeladressen im
oeffentlichen Core. Nur so koennen fehlende Submissions von spaeteren
Rendererfehlern unterschieden werden.

Nur `ArmedCapture` aktiviert zusaetzlich den vorhandenen schweren Drawstream
fuer maximal drei ausgewaehlte Frames. Start, Ende, Intervall und Drawbudget
werden mit den bestehenden `KATANA_NATIVE_GRAPHICS_CAPTURE_*`-Variablen
gesetzt. Das allein aktiviert keine Screenshots und keinen GPU-Readback; der
alte Screenshotpfad benoetigt weiterhin explizit sein eigenes Capture-
Verzeichnis. Der Binaerring wird nach dem Lauf offline dekodiert:

```powershell
python tools/decode-native-graphics-breadcrumbs.py `
  <diagnostics>\graphics-breadcrumbs-v2.bin `
  --output <diagnostics>\graphics-breadcrumbs-v2.jsonl
```

Der stille Sechs-Routen-Runner kann denselben Modus pro Zyklus direkt und
pro Route getrennt aktivieren, ohne Fenster, Audioausgabe oder Screenshots:

```powershell
private\logs\run-background-replays.ps1 `
  -Exe <port>\game.exe `
  -Revision rNNN `
  -GraphicsDiagnosticsMode breadcrumbs `
  -GraphicsDiagnosticsCapacity 32768
```

`off` bleibt der Default. `armed-capture` gilt als schwere Telemetrie und
wird nur fuer einen eng begrenzten, vorher begruendeten Drawstream-Lauf
verwendet; fuer die regulaere Replay-Matrix genuegen `digest` oder
`breadcrumbs`.

Die Texture-Binding-Provenienz des Titeladapters und die vom Renderer
validierte Texture-Resource-Provenienz sind absichtlich getrennt. Ein Runtime-
Handle oder ein beobachteter Texlist-Index darf dadurch weder eine
Archive-Identitaet erfinden noch ein statisches Proof-Gate schliessen.

## Evidence-Zustandsmodell

```text
Observed
   -> Candidate
      -> Proven -----------\
      -> RuntimeContract ---+-> Strict Product
      -> Unresolved --------X
```

### `Observed`

Ein Runtime-Witness beweist, dass ein Verhalten in einem gebundenen Lauf
tatsaechlich vorkam. Er ist ein Existenz- oder Gegenbeweis, aber niemals ein
Vollstaendigkeitsbeweis. Er darf keine statische Kante schliessen.

### `Candidate`

Katana hat den Witness mit Binary, CFG, Eigentuemern, Modulidentitaet und
Manifest korreliert und kann die fehlende Proofbedingung benennen. Ein
Candidate ist ein gerichteter Beweisauftrag und bleibt im strikten Export
unzulaessig.

### `Proven`

Eine allgemeine statische Regel oder ein vollstaendiger, exakter,
identity-bound Titelvertrag beweist Zielmenge, Eigentum, Bytes, Grenze und
erforderliche Semantik. Nur dieser Zustand darf statische Closure schliessen.

### `RuntimeContract`

Der Kontrollfluss ist semantisch wirklich dynamisch. Die Site darf im
strikten Produkt nur ueber einen vorab definierten RuntimeOnly-Vertrag zu
einem exakten, aktiven, vorkompilierten Block dispatchen. Der Vertrag bindet
Site, Dispatchklasse, Modul, Generation, Blockanfang, ABI und
Fortsetzungssemantik. Eine blosse Beobachtung oder Allowlist-Zugehoerigkeit
genuegt nicht.

### `Unresolved`

Provenienz, Eigentum, Identitaet oder Semantik reichen nicht aus. Der strikte
Export bleibt fail-closed.

## Native Bring-up-Dispatchgrenze

Ein Bring-up-Dispatch darf nur dann ausfuehren, wenn alle folgenden
Bedingungen vor der ersten Zustandsaenderung gelten:

1. Ziel und Site sind fuer genau diesen AOT-Pack in der Bring-up-Allowlist.
2. Das Ziel ist ein exakter Blockanfang in der versiegelten Blocktabelle.
3. In v1 gehoeren Source und Target zum residenten `primary`-Image; seine
   Bootidentitaet, Packgeneration und Codeidentitaeten stimmen.
4. Das Handle ist aktiv, generationgesichert und weiterhin dispatchbar.
5. Blockvariante, ABI, Aliasnormalisierung und Fortsetzung sind gueltig.

Die executable Bring-up-Allowlist darf ausdruecklich reviewte `Candidate`-
und `Proven`-Records enthalten. Bei einem `Candidate` beweist der Export nur
die sichere konkrete Ausfuehrung dieses Source-/Target-Paars im gebundenen
AOT-Pack; der weiterhin eingetragene `missing_proof` bleibt offen. Das ist
weder ein Vollstaendigkeitsbeweis fuer die Zielmenge noch eine stille
Promotion. `Observed`, `Unresolved` und ein blosses `RuntimeContract` werden
nicht in diese Allowlist uebernommen. Der strikte Export konsultiert sie nie.

Der v1-Schluessel aus Transferart, Callsite und Target ist nur innerhalb
dieser residenten, exportseitig versiegelten Sicht gueltig. Bevor dynamische
Module zugelassen werden, muss der Schluessel zusaetzlich die aktive Source-
und Target-ModuleInstance samt Lifecycle-Generation binden.

Die Allowlist enthaelt keine rohen Hostfunktionszeiger und darf die
Blocktabelle weder erweitern noch mutieren. Ein Miss endet sofort als
`UnknownCompiledTarget` beziehungsweise typisierter Identity-/Generation-
Miss. Es gibt keinen Materializer-, Interpreter-, JIT-, No-op- oder
Default-Fallback.

Ein Bring-up-Hit bedeutet nur: sicher vorkompilierter Code wurde ausgefuehrt,
obwohl sein Vollstaendigkeitsbeweis noch fehlt. Er erzeugt einen Witness und
keine automatische Promotion.

## Zielvertrag fuer den Runtime Witness Store

Der vollstaendige Store dieses Abschnitts ist noch nicht implementiert. Der
aktuelle Runtime-Unterbau besitzt nur einen kleinen, deduplizierenden Puffer
fuer 16 verschiedene Dispatch-Hit-/Miss-Signaturen und einen Offline-JSON-
Serializer. Er ist eine sichere Dispatchdiagnose, aber weder der geplante
persistierbare Witness-Ring noch ein Repro-Bundle.

Ein Witness ist klein, deterministisch serialisierbar und mindestens an
folgende Felder gebunden:

- Schema, Witness-ID, Replay-ID und monotone Eventnummer;
- Pack-, Runtime-, Adapter-, Manifest- und Buildidentitaet;
- Dispatchklasse, Site/Callsite, Ziel und Fortsetzung;
- aktives Image/Modul, Modulidentitaet, Lifecycle und Generation;
- exakter Block-/Function-Entry und validiertes generationsgesichertes Handle;
- Registerquelle, Speicherquelle und letzte bekannte Writer-Provenienz;
- relevante Guard-, Alias-, ABI- und Codeidentitaeten;
- Ergebnis: Hit, `UnknownCompiledTarget`, stale generation, identity mismatch
  oder anderer typisierter Stop.

Witnesses liegen ausserhalb des Hot Paths in einem begrenzten Store. Der
Lauf schreibt kompakte feste Events oder Digests; umfangreiche
Zusammenfuehrung, Symbolisierung und JSON-Ausgabe erfolgen offline.

## Evidence Promotion

Implementiert ist ein deterministischer Exportreport, der authorierte
Records und ihre erneute Execution-Safety-Zulassung ausweist. Die folgende
vollstaendige Witness-Korrelation und automatische Taskableitung ist noch
Zielzustand, nicht bereits eine Promotion-Engine.

Der Promotion-Lauf korreliert einen Witness mit der statischen Welt und gibt
mindestens aus:

- Observation und gebundene Artefaktidentitaeten;
- vorhandener ProgramIndex-/Block-/Owner-/CFG-Befund;
- Datenfluss zur Zielquelle sowie gefundene Stores, Tabellen oder
  Callbackregistrierungen;
- exakt fehlende Proofbedingung;
- vorgeschlagener Ausgang: generische Analyzerregel, privater
  identity-bound Vertrag, RuntimeContract, Produktbug oder `Unresolved`;
- betroffene Analyse-/Manifestpfade und erforderliche Reanalyse;
- maschinenlesbare Acceptance-Bedingungen.

Promotion bleibt eine explizite, reviewte Aenderung. Kein Runtime-Witness
wird beim Import still zu `Proven` oder `RuntimeContract`.

## Zielvertrag fuer Replay und First-Divergence

Der vorhandene deterministische System-Replaypfad kann einen Ereignisstrom
und einen finalen Guest-State-Hash gegen eine Aufzeichnung verifizieren. Er
speist Host-/Providerantworten, Async-Completion-Reihenfolgen und
Modul-Lifecycle noch nicht allgemein wieder ein und besitzt noch keine
automatische Checkpoint-Bisektion. Die folgenden Anforderungen beschreiben
den noch zu implementierenden Provider-Replay- und First-Divergence-Pfad.

Das Replay bindet alle titelbeobachtbaren nichtdeterministischen Hostresultate,
insbesondere Eingabe mit Framegrenzen, Titelzeit, Providerantworten,
asynchrone Completion-Reihenfolge, Datei-/Contentresultate, Save-/Load,
Movie/Audio, externe Entropie sowie Modul- und Overlay-Lifecycle.

Der Produktcode bleibt derselbe native AOT-Code; nur Hostprovider liefern die
aufgezeichneten Antworten. Periodische Rolling Hashes fuer CPU-Zustand,
relevante Speicherpages, aktives Modul, Providerzustand und Guest-Callstack
lokalisieren die erste Abweichung. Bei Bedarf wird nur um dieses Intervall
feiner aufgezeichnet.

## Diagnosebudget

Diagnostik darf die Produktruntime nicht unbrauchbar machen. Ein Profil, das
sekundenlange Frames erzeugt, ist kein Standard-Bring-up-Profil.

- Hot-Path-Ereignisse sind vorallokiert, kompakt und begrenzt.
- Grosse Traces, Symbolisierung, Clustering und Last-Writer-Auswertung laufen
  offline oder gezielt fuer ein kurzes Divergenzintervall.
- Crashkapsel und stabiler Crash-Digest bleiben klein und allokationsfrei.
- Tests und Replays laufen standardmaessig stumm, unsichtbar und ohne
  Screenshot-/Audio-Capture; sichtbare Laeufe erfolgen nur auf Anforderung.

## Tasksteuerung

Die Prioritaet waehrend des Bring-ups ist:

1. erster reproduzierbarer Produktfehler oder erste Divergenz;
2. AOT-/Identity-Luecke, wenn sie genau diesen Lauf blockiert;
3. kleinster Runtime-, Adapter-, Manifest- oder generischer Fix;
4. Wiederholung desselben Replays mit demselben AOT-Pack;
5. erst bei einer nachgewiesenen AOT-Ursache Rueckkehr zur grossen Schleife.

Offene globale Frontiers bleiben wichtige Releasearbeit, werden aber nicht
allein durch ihre Anzahl zu Bring-up-Auftraegen. Private Titeladressen und
title-bound Evidenz bleiben im privaten Portprojekt; der oeffentliche Kern
implementiert nur address-agnostische Vertraege, Validierung und Formate.

## Strict Admission Rule

Der strikte Produktbuild akzeptiert ausschliesslich:

| Evidence | Strict | Bedeutung |
| --- | --- | --- |
| `Observed` | nein | Laufzeitbeobachtung, kein Vollstaendigkeitsbeweis |
| `Candidate` | nein | gerichteter, noch offener Beweisauftrag |
| `Proven` | ja | statisch oder vollstaendig identity-bound bewiesen |
| `RuntimeContract` | ja | validierter dynamischer Vertrag im aktiven AOT-Universum |
| `Unresolved` | nein | fehlende Provenienz, Identitaet oder Semantik |

Die Regel ist fail-closed. Widerspruechliche, stale, unvollstaendige oder
nicht reproduzierbare Evidence faellt auf `Unresolved` zurueck.
