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

## Deterministisches Authoring-Artefakt

Die private Allowlist wird nicht aus einem Runtime-Log gelernt. Ein
versioniertes JSONL-Authoring-Dokument enthaelt genau einen Header und danach
explizit reviewte Evidence-Records. Der Header bindet mindestens Projekt-ID,
Projektversion, committed Analysis-Generation, die SHA-256-Identitaet des
vollstaendigen AOT-Packs und dessen nicht-null Generation. Jeder Record bindet
Source-Owner, terminalen Source-Block, Callsite samt Delay-Slot, Transferart,
Fortsetzung, Target-Block, beide Owner, exakte Codeidentitaeten sowie Image-,
Modul- und Generationsidentitaeten.

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

## Die grosse Schleife: `strict-product`

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

## Die kleine Schleife: `native-bringup`

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

Replays sind stille, unsichtbare, native Produktlaeufe. Redundante Szenarien,
die denselben frueheren Checkpoint langsamer erreichen, gehoeren nicht in den
Standardgurt. Die Suite misst mindestens:

- weitesten stabilen Produktmeilenstein und Frame;
- erste Divergenz gegen eine gebundene Baseline;
- neue oder bekannte Crash-/Stop-Signatur;
- Laufzeit sowie die Identitaeten aller verwendeten Artefakte;
- ob eine AOT-Luecke die Rueckkehr in die grosse Schleife erzwingt.

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
3. Image/Modul, aktive Generation und Codeidentitaet stimmen.
4. Das Handle ist aktiv, generationgesichert und weiterhin dispatchbar.
5. Blockvariante, ABI, Aliasnormalisierung und Fortsetzung sind gueltig.

Die executable Bring-up-Allowlist darf ausdruecklich reviewte `Candidate`-
und `Proven`-Records enthalten. Bei einem `Candidate` beweist der Export nur
die sichere konkrete Ausfuehrung dieses Source-/Target-Paars im gebundenen
AOT-Pack; der weiterhin eingetragene `missing_proof` bleibt offen. Das ist
weder ein Vollstaendigkeitsbeweis fuer die Zielmenge noch eine stille
Promotion. `Observed`, `Unresolved` und ein blosses `RuntimeContract` werden
nicht in diese Allowlist uebernommen. Der strikte Export konsultiert sie nie.

Die Allowlist enthaelt keine rohen Hostfunktionszeiger und darf die
Blocktabelle weder erweitern noch mutieren. Ein Miss endet sofort als
`UnknownCompiledTarget` mit Site, Ziel, Modul, Generation und
Artefaktidentitaeten. Es gibt keinen Materializer-, Interpreter-, JIT-,
No-op- oder Default-Fallback.

Ein Bring-up-Hit bedeutet nur: sicher vorkompilierter Code wurde ausgefuehrt,
obwohl sein Vollstaendigkeitsbeweis noch fehlt. Er erzeugt einen Witness und
keine automatische Promotion.

## Runtime Witness Store

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

## Evidence Promotion Report

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

## Replay und First-Divergence

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
