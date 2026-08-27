# Verbindliche Arbeitsregeln fuer das gesamte Repository

Diese Regeln gelten fuer jeden automatisierten Bearbeiter, jeden Task, jede
Phase und jeden Teilbereich dieses Repositories. Sie sind keine Empfehlung.
Widersprechende aeltere Prozessbeschreibungen in Roadmap-, Task-, Status-,
Handoff- oder Performance-Dokumenten werden durch diesen Vertrag ersetzt.

## Verbindliche Entwicklungsloops und Evidence-Klassen

Die vollstaendige Definition steht in
`docs/NATIVE_BRINGUP_WORKFLOW.md`. Fuer jeden Bearbeiter gelten zwei getrennte
Schleifen. Welche Schleife gilt, entscheidet der erste fehlende
Spielwissensvertrag des aktiven Replaypfads:

```text
strict-product:
  statische Analyse -> Closure -> AOT/Overlays -> stabiler AOT-Pack
  -> Produktbuild und Releasegate

native-bringup:
  gleicher AOT-Pack -> Replay -> erste Divergenz/typisierter Stop
  -> kleinster Fix -> inkrementeller Runtime-/Adapter-/Manifestbuild
  -> dasselbe Replay
```

### Makro- und Mikrozyklen

- Ein `Mikrozyklus` verwendet die vorhandene World, den vorhandenen AOT-Pack
  und dasselbe Replayset. Er ist nur fuer bereits statisch bekannte
  Ausfuehrung mit reinem Host-, Adapter-, Renderer-, Audio-, Eingabe- oder
  Praesentationsfehler zulaessig. Er baut genau ein
  inkrementelles NativeBringup-Produktbinary und wiederholt dieselben sechs
  Replays. Er erzeugt weder eine neue Analyse noch einen neuen Egg-Fleet-Pool.
- Ein `Makrozyklus` beginnt bei einem AOT-/Spielwissensereignis:
  `UnknownCompiledTarget`, neue oder geaenderte Funktionen beziehungsweise
  CFG, neue Overlays, geaenderte Roots/Switch-/Jumptabellen, AOT-/Codegen-
  Semantik, AOT-ABI, Callbacks, Provider-/Owner-Semantik, Replacement-
  Reachability, statische Rueckkehrpfade oder eine Evidence-Promotion, die
  die ausfuehrbare Welt veraendert. Erst dann folgen Analyse, neuer Pool, Egg
  Fleet, AOT/Export und die sechs Replays. Viele notwendige Makrozyklen sind
  kein Prozessfehler, solange jeder einen aktiven Knowledge Gap voranbringt.
- Texture-Binding-, Grafik-, Renderer-, Audio-, Provider-, Adapter-, Movie-,
  Input-, Save-, Datei- und reine Diagnosefixes bleiben im Mikrozyklus und
  verwenden den vorhandenen Pack nur, wenn Aufruf, Zielmenge und
  Providervertrag bereits vollstaendig bekannt sind und keine der genannten
  AOT-/Closure-Grenzen beruehrt wird.

### Verbindlicher Cycle-Freeze

- Vor der Implementierung wird jeder Zyklus durch ein maschinenlesbares
  Manifest eingefroren. Es bindet mindestens Zyklus-ID, Workflowklasse,
  World- und Pack-SHA-256, Runtime-Commit/Source-Identity, Replayset-SHA-256,
  exakt sechs Replay-IDs samt K1-K5-Stopklasse, aktuellen und angestrebten
  Storycheckpoint, Knowledge-Gap-Cluster, den an ein bearbeitetes Cluster
  gekoppelten kleinen Runtime-Performance-Fix, Produktbuildbudget `1` und
  `analysis_required=true|false`. Ein Grafikcluster ist nur bei
  Bring-up-Relevanz, Crashursache oder nachgewiesenem Multi-Close Pflicht.
- Nur Findings, deren World/Pack/Source und Replay-Reachability zum Freeze
  passen, gehoeren in den Batch. Spaeter eintreffende Findings gehen in den
  naechsten Zyklus. Das Ein-Build-Budget darf nicht durch ein nachtraeglich
  wachsendes Batch umgangen werden.

### Verbindliches Produktbuild-Budget im Native-Bring-up

- Im laufenden Sonic-NativeBring-up ist standardmaessig ausschliesslich ein
  `NativeBringup`-Produktbuild zulaessig. Ein Strict-/Release-/Gate-Build wird
  nur nach einer aktuellen ausdruecklichen Nutzeranweisung fuer genau diesen
  Build gestartet; statischer Fortschritt allein ist keine Freigabe.
- Pro zusammenhaengendem Fortschrittsbatch wird hoechstens ein
  NativeBringup-Produktbinary gebaut. Mehrere kompatible Bug-, Evidence-,
  Analyzer-, Runtime-, Adapter- und Manifestarbeiten werden davor gebuendelt;
  ein Produktbuild pro Einzelbug ist unzulaessig.
- Dasselbe NativeBringup-Binary wird fuer alle zugehoerigen Pflichtreplays
  wiederverwendet. Ein zweiter Produktbuild im selben Batch braucht eine neue
  ausdrueckliche Nutzeranweisung.
- Gezielte CLI-, Unit- oder Komponenten-Targets zur lokalen
  Quellverifikation sind keine Produktbuilds und bleiben zulaessig. Sie duerfen
  aber weder vorsorglich den Strict-Port bauen noch einen zusaetzlichen
  Produktbuild als Messlauf erzwingen.

- Ein vollstaendiger Analyzer-/Exportlauf wird nur bei einer nachgewiesenen
  AOT-wirksamen Aenderung gestartet: neue oder geaenderte Imagebytes,
  Funktionsgrenzen, CFG, Roots, Tabellen, Overlays, Patchstellen,
  Instruktions-/Codegensemantik, AOT-ABI oder ein echtes
  `UnknownCompiledTarget`.
- Grafik-, Audio-, Movie-, Input-, Save-, Datei-, Provider-, Adapter-,
  Renderer- oder Diagnoseaenderungen verwenden den vorhandenen AOT-Pack und
  die kleine Schleife, solange ihre AOT-Identitaet unveraendert bleibt.
- Bring-up lockert Proof-Completeness, niemals Execution-Safety. Ausgefuehrt
  werden nur exakte aktive vorkompilierte Blockanfaenge einer identity- und
  generationgebundenen Allowlist, validiert durch die versiegelte
  Blocktabelle. Die Allowlist enthaelt keine rohen Hostfunktionszeiger und
  mutiert keine Runtime-Tabelle.
- Ein explizit authorierter `Candidate` darf in dieser nicht releasefaehigen
  Allowlist nur nach unabhaengiger exakter Source-/Callsite-/Target-/Owner-/
  Byte-/Pack-/Generationsvalidierung ausgefuehrt werden. Sein `missing_proof`
  bleibt offen; der Hit darf weder Strict noch eine Frontier schliessen.
  `Observed`, `Unresolved` und blosse RuntimeContracts sind hier nicht
  executable.
- Ein Allowlist-/Blocktabellen-Miss endet sofort als
  `UnknownCompiledTarget`. Interpreter, JIT, Runtime-Dekodierung,
  Materializer-, No-op- und Guessing-Fallbacks bleiben verboten.
- Evidence folgt ausschliesslich
  `Observed -> Candidate -> Proven | RuntimeContract -> Strict Product`.
  Runtime-Witnesses sind Existenz- oder Gegenbeweise und erzeugen
  Beweisauftraege; sie werden nie automatisch zu Closure hochgestuft.
  `Observed`, `Candidate` und `Unresolved` blockieren den strikten Export.
- `RuntimeContract` ist nur zulaessig, wenn der bestehende strikte
  RuntimeOnly-Vertrag Site, aktives Modul, Generation, exakten Blockanfang,
  ABI und Fortsetzung vor jeder Zustandsaenderung validiert.
- Im Bring-up ist die erste reproduzierbare Divergenz oder der erste
  typisierte Stop autoritativ. Die gesamte offene Analyzer-Frontier erzeugt
  nicht automatisch einen Implementierungsauftrag. Nach jeder erfolgreichen
  Analyse muss der Analyzerstand trotzdem als eigener read-only
  Katana-Taskpool durch die feste Egg-Fleet inventarisiert werden.

### Pflichtgate nach jeder Analyse: Egg Fleet

- Die allererste Aktion unmittelbar nach jeder erfolgreich publizierten
  `analyze-port`-Generation ist die Ausgabe und Delegation des neuen
  Egg-Fleet-Pools. Lokale Closure-, RuntimeOnly-, Replay- oder sonstige
  Folgearbeit beginnt erst, nachdem diese Tasks gestartet wurden, damit ihre
  Ergebnisse und relevanten Fixes noch in die naechste Analyse eingehen.
- Aus genau der publizierten `materialization-world.katana-world` wird mit
  `next-analysis-task --format agent-json --task-count 28` ein neuer,
  unveraenderter Pool erzeugt. Pool-, World-, World-JSON- und
  Native-Analysis-SHA-256 werden vor der Delegation festgehalten. Liefert die
  Authority nach echter Closure weniger als 28 priorisierte Tasks, ist diese
  kleinere Taskzahl autoritativ; es werden keine leeren Positionen erfunden.
- Die vorhandenen Poolpositionen werden zuerst nach Owner, Provider,
  Frontierart und betroffenen Dateien geclustert. Exklusive Arbeitspakete von
  hoechstens sechs Positionen gehen an die bestehenden read-only Egg-Fleet-
  Tasks; Positionen duerfen dafuer nicht zusammenhaengend sein. Dieselbe
  Ownerfamilie bleibt ueber Zyklen moeglichst beim selben Chat. Nur bei echter
  Lastungsdifferenz wird umverteilt. Poolposition, Frontier-ID und Authority
  bleiben in jedem Paket explizit und verlustfrei.
  Dafuer werden die bestehenden, dauerhaften Egg-Fleet-Chats wiederverwendet.
  Ein neuer Analysezyklus erzeugt neue Arbeitspakete, aber keine neuen Chats;
  neue Chats duerfen nur auf eine ausdrueckliche Nutzeranweisung angelegt
  werden. Nicht benoetigte bestehende Slice-Chats bleiben fuer den Zyklus
  einfach ohne Auftrag.
- Jeder Egg-Fleet-Slice und jeder andere delegierte Analyse-Task laeuft
  verbindlich mit `gpt-5.6-luna` und Reasoning `max`. `gpt-5.6-sol` mit
  Reasoning `max` ist ausschliesslich dem Haupttask vorbehalten und darf fuer
  die Flotte weder implizit geerbt noch explizit gewaehlt werden.
  Jeder Task prueft Pool-zu-World, den Delta zur zuletzt von ihm geprueften
  Generation, die aktuelle Source-/Disassembly-/Provider-Evidence,
  A/B/C-Klassifikation, kleinsten fail-closed Scope, Acceptance, Kollisionen
  und Multi-Close. Sein maschinenlesbarer Handoff bindet zusaetzlich World-
  und Pack-SHA, Task-IDs, Root-Cause-Key, betroffene Dateien, Collision-Key,
  Replay-Reachability, Multi-Close-Set, naechsten erreichbaren
  Story-/Gameplaycheckpoint und beantwortet fuer jeden Frontier:
  aktiver Sechserpfad, gemeinsamer Replay-Close, generischer oder
  Sonic-spezifischer Ursprung, `sad_disasm`-Beweischance, strenger
  identity-bound Titelvertrag und erwarteter neuer Produktpfad.
- `A` bedeutet: aktuelle World und Source, genaue Ursache und Implementierung,
  keine offene Evidencefrage; der Fall wird in diesem Zyklus umgesetzt.
  Aktuelle Replay-/Produkt-Reachability oder klarer Multi-Close erhoehen die
  Reihenfolge, sind aber keine Voraussetzung: Jeder streng beweisbare
  Hardware-Owner wird im selben Zyklus geschlossen, auch wenn ihn erst ein
  spaeterer Story- oder Gameplaypfad erreicht.
  `B` ist ein bestaetigter, aber fuer den eingefrorenen Batch nicht
  erreichter oder anderweitig blockierter Befund. `C` ist stale, duplicate,
  bereits geschlossen, falsch interpretiert, unzureichend bewiesen oder
  ausserhalb des aktuellen Produkts und wird nicht implementiert.
- Die Egg Fleet darf weder Dateien noch Pools aendern oder erzeugen und keine
  Builds, Tests, Replays, Commits oder Pushes starten. Fehlende Evidence ist
  niemals `unreachable`, und Runtime-Witnesses werden nie zu statischem Proof
  hochgestuft.
- Replay-Sammlung und read-only Inspektion duerfen parallel zur Egg Fleet
  laufen. Der eine Produktbuild darf erst starten, wenn alle fuer den Pool
  benoetigten Handoffs eingegangen, gegen den Freeze abgeglichen und alle
  relevanten A-Faelle umgesetzt sind.

### Replays als Knowledge-Gap-Probes

- Jeder der sechs ersten Stops traegt genau eine Primaerklasse:
  `K1` unbekanntes ausfuehrbares Ziel/FunctionEntry/Block/Overlay/Loaded-AOT;
  `K2` bekannte Funktion mit unvollstaendiger Callback-, Ziel-, Return- oder
  Function-Pointer-Menge; `K3` bekannter Code mit fehlendem nativen
  Provider-/Result-/State-Vertrag; `K4` falsche CPU-/AOT-Semantik; `K5` reine
  native Grafik-/Audio-/Movie-/Input-/Pacing-Abweichung.
- K1 fuehrt zu Analyzer-/AOT-Arbeit, K2 zu CFA/FVA oder identity-bound
  Titelbeweis, K3 zu Owner-Semantik und Providerbindung, K4 zu Decoder/IR/
  Codegen/Runtime-Semantik und K5 zur kleinen Hostschleife.
- Nach der Sechsermatrix werden nicht sechs Einzelbugs, sondern gemeinsame
  Callback-/Overlay-/AOT-/Provider-/Semantikcluster implementiert. Prioritaet
  ist `Replay-Reachability * Multi-Close * Story-/Gameplay-Naehe /
  Implementierungsrisiko`, nicht rohe P0- oder Frontier-Reihenfolge.
- Exakte Sonic-Tabellen, Funktionsgrenzen, Callbackregistrierungen,
  Objektmethoden, Story-/Event-Dispatch, Overlayeintritte und Handler duerfen
  als private `sad_disasm`-Evidence gebunden werden, wenn Disc-/Image-SHA,
  Tabellenbereich und -bytes, Eintragszahl, Zielbytes, Function-Boundary,
  Modulidentitaet und Generation exakt validiert werden. Wiederkehrende
  adressunabhaengige SH-4-Muster gehoeren dagegen in den generischen Analyzer;
  wirklich dynamische Callbacks bleiben RuntimeContract/RuntimeOnly.

## Projektweiter Taskablauf

Fuer jeden Task gilt ab sofort genau diese Reihenfolge:

```text
Task implementieren
  -> alle durch den Task betroffenen Pfade reviewen
     und bestaetigte Fehler innerhalb desselben Reviewdurchlaufs schliessen
  -> den reviewten Stand im dirty Worktree konfigurieren und gezielt bauen
  -> erst nach erfolgreichem Build auf main committen und pushen
  -> naechster Task
```

- Die Reviewstufe ist die Fehlerfindungs- und Fixstufe. Sie umfasst den
  implementierten Pfad, seine Aufrufer und Verbraucher, Verdrahtung,
  Datenfluss, Fehlerpfade, ABI-/Cache-/Versionsvertraege sowie alle weiteren
  unmittelbar betroffenen Schichten.
- Bestaetigte Korrektheits-, Boot-, Vollstaendigkeits- und relevante
  Performancefehler im Taskscope werden vor dem Build und Push geschlossen.
  Eine separate nachgelagerte Fix- oder Testphase wird daraus nicht erzeugt.
- Ein Commit ist niemals Voraussetzung fuer einen lokalen Build. Nach
  Sourceaenderungen wird der bestehende Buildbaum zuerst neu konfiguriert,
  damit ein dirty Entwicklungsbinary die untrusted Source-Identity einbettet.
  Erst der reviewte und erfolgreich gebaute Stand wird committed.
- Der kanonische Windows-Build des dirty CLI-Targets laeuft ueber
  `tools/build-katana-cli.ps1`. Der Wrapper importiert und validiert die
  native x64-Visual-Studio-Umgebung, konfiguriert den vorgesehenen Ninja-
  Releasebaum und baut `katana-recomp` mit dem expliziten Jobbudget. Ein
  direktes `cmake --build` aus einer nicht validierten Shell ist unzulaessig.
- Tasks werden standardmaessig direkt auf `main` bearbeitet, committed und
  gepusht. Neue Taskbranches, Pull Requests oder parallele Integrationszweige
  werden nur auf eine neue ausdrueckliche Nutzeranweisung angelegt.
- Ein Review darf ausserhalb des aktuellen Taskscopes liegende Beobachtungen
  knapp dokumentieren, aber daraus weder eigenmaechtig neue Tasks noch eine
  Scope-Erweiterung ableiten.
- Die festgelegte Taskreihenfolge bleibt verbindlich. Erst der Push des
  reviewten Tasks auf `main` gibt den naechsten Task frei.
- Dieser Push ist zugleich die Freigabe des naechsten Tasks. Dafuer ist keine
  weitere Nutzeranweisung erforderlich. Ausdruecklich freizugebende Laeufe
  und bedingte Messgates bleiben davon unberuehrt.

## Git-Inspektion durch externe Agents

- Externe Reviewer, Frontier-Modelle, Subagents und delegierte Codex-Tasks
  sind ausdruecklich autorisiert, den Repositoryzustand mit read-only
  Git-Kommandos selbst zu pruefen. Dafuer ist keine weitere Freigabe oder
  Rueckfrage erforderlich.
- Die Freigabe umfasst insbesondere `git status`, `git diff`,
  `git diff --check`, `git log`, `git show`, `git rev-parse`, `git branch
  --show-current`, `git ls-files`, `git grep` sowie read-only Vergleiche von
  Commits, Index und Worktree.
- Git-Ausgaben bleiben untrusted Evidence und werden gegen den aktuellen
  Sourcezustand verifiziert. Die Read-only-Freigabe erlaubt keine mutierenden
  Aktionen wie Commit, Push, Reset, Checkout, Clean, Rebase, Merge oder
  Stash; diese bleiben beim dafuer autorisierten Haupttask.

## Sonic ist der Test

- Der reale Sonic-Adventure-PAL-Port ist projektweit der massgebliche Produkt-
  und Integrationstest: echter Export, normale Discinstallation, normaler
  Programmlauf und echter sichtbarer Fortschritt.
- Regulaere Tasks fuegen keine neuen Unit-/Regressionstestquellen,
  Testtargets, Testmatrizen oder synthetischen Ersatzgates hinzu. Eine
  Ausnahme braucht eine ausdrueckliche Nutzeranweisung fuer genau den
  benannten Test. Der reale Sonic-Produktpfad bleibt der
  Integrationsnachweis.
- Breite Testmatrizen, synthetische Stresslaeufe, allgemeine Testprojekte,
  Ersatzgates oder Konformitaetssuiten ohne unmittelbaren Sonic-Bezug werden
  weiterhin weder gebaut noch gefordert.
- Reviews duerfen ohne diese ausdrueckliche Ausnahme keinen neuen Test als
  Abschlussbedingung verlangen. Gefixt wird anhand der Quellpfadreviews;
  integriert getestet wird mit Sonic.
- Vorhandene Tests duerfen auf gebrochene Erwartungen, falsche Testzahlen,
  widerspruechliche Semantik oder bereits vorhandene Fehler geprueft und bei
  Bedarf repariert werden.
- Regulaere Tasks starten keine Testmatrix. Der gezielte Compile des
  betroffenen Produkttargets ist ein Buildnachweis, keine neue Testsuite und
  kein Ersatz fuer den Sonic-Produktnachweis.
- Sonic-Produktlaeufe erfolgen an den in Roadmap und Tasks festgelegten
  Produktgates oder innerhalb der kleinen, reproduzierbaren Bring-up-Schleife.
  Mehrere zusammenhaengende reviewte Tasks duerfen vor dem naechsten
  vollstaendigen Produktgurt auf `main` landen.
- Vor jeder Implementierungsrunde der sechs Pflichtreplays werden zuerst mit
  demselben bereits vorhandenen BringUp-Binary alle sechs ersten
  reproduzierbaren Crash-/Typed-Stop-Befunde aufgenommen. Bis die vollstaendige
  6er-Crashmatrix vorliegt, werden aus einzelnen Replaybefunden keine Fixes
  implementiert und keine Zwischenbuilds erzeugt.
- Erst danach werden die sechs Ursachen gemeinsam mit den relevanten
  Egg-Fleet-/RuntimeOnly-Befunden als ein zusammenhaengendes Fixpaket
  umgesetzt. Fuer dieses Gesamtpaket folgt genau ein neuer NativeBringup-
  Produktbuild; ein Build pro Replay oder pro Bug ist unzulaessig.
- Der gemeinsame Zyklusbatch ist erst vollstaendig, wenn neben der
  6er-Crashmatrix auch alle aktuellen Egg-Fleet-Handoffs abgeglichen und alle
  relevanten aktuellen sowie historischen, noch nicht umgesetzten A-Befunde
  aufgenommen sind. Vor diesem Abgleich darf kein Produktbuild starten;
  irrelevante oder nachweislich ueberholte A-Befunde werden mit Evidence
  begruendet ausgelassen.
- Automatisierte Sonic-Laeufe und gezielte Tests sind standardmaessig stumm,
  unsichtbar und ohne Screenshot-/Audio-Capture. Ein sichtbarer Lauf erfolgt
  nur auf ausdrueckliche Anforderung. Redundante Replays, die denselben
  frueheren Checkpoint nur langsamer erreichen, gehoeren nicht in den
  Standardgurt.
- Performance wird am realen End-to-End-Produktpfad gemessen. Synthetische
  Zeiten, gruene Testmatrizen oder technische Hilfsframes sind kein Ersatz
  fuer Kaltbuildzeit, vollstaendigen Export und sichtbaren Sonic-Lauf.
- Jeder Analysezyklus darf genau eine kleine, isoliert reviewbare und im
  ausgefuehrten Produktpfad wirksame Runtime-Performanceoptimierung im ohnehin
  bearbeiteten Knowledge-Gap-Pfad mitnehmen. Sie ist kein eigenstaendiger
  Arbeitsblock, wird mit dem normalen Fixpaket gebaut und mit denselben sechs
  Replays nur akzeptiert, wenn kein Replay regressiert. Sie erzeugt weder
  Zusatzbuild noch reine Messrunde und darf den Bring-up-Fix nicht verdraengen.
- Reine Instrumentierung sowie Analyzer-, Exporter-, Graph-, Cache-, Ninja-
  oder Buildsystemarbeit erfuellt diese Pflicht nicht. Solche Optimierungen
  duerfen separat sinnvoll sein, werden aber niemals als Produktruntime-Fix
  gezaehlt.
- Grafikarbeit wird nur in den Batch aufgenommen, wenn sie mehrere Szenen
  schliesst, einen generischen Rendererfehler oder Crash behebt, Diagnose oder
  aktuellen Fortschritt verdeckt oder zugleich die gekoppelte Performance-
  Verbesserung liefert. Ein einzelner kosmetischer Fehler ist vor M4-M8
  nachrangig. Aufgenommene Grafikcluster werden weiterhin als `Submission
  fehlt`, `Geometrie verschwindet`, `falsches Asset`, `falscher Renderstate`,
  `falscher Shadervertrag` oder `falsche Reihenfolge` klassifiziert.
- Performance-Replays laufen einzeln und mit Grafikdiagnostik `Off`, damit
  weder konkurrierende D3D11-Instanzen noch Breadcrumb-/Capture-I/O die
  Messung verfaelschen. Fuer reine Crashsammlung sind hoechstens zwei
  Produktprozesse parallel erlaubt; Breadcrumbs werden nur fuer den ersten
  relevanten Fehlerpfad aktiviert.
- Der gekoppelte Runtime-Fix, die relevanten aktuellen und historischen
  A-Befunde, die geclusterten sechs Replayursachen und ein gegebenenfalls
  bring-up-relevanter Grafikcluster werden vor genau einem
  NativeBringup-Produktbuild gebuendelt. A-Befunde werden implementiert und
  auf Closure geprueft, nicht nur erneut ausgewertet. Dazu gehoert jeder
  aktuelle Hardware-Owner, dessen Boundary, Bytes, Semantik, Provider und
  Residualfreiheit bereits streng beweisbar sind; fehlende unmittelbare
  Replay-Reachability ist kein Auslassgrund. Nur ueberholte, doppelte oder
  weiterhin beweisoffene Befunde werden mit Evidence begruendet ausgelassen.
- Grafikdiagnostik ist standardmaessig `Off`. `Digest` darf nur feste
  Integer-Mixes ausfuehren; `Breadcrumbs` schreibt vorallokierte numerische
  Records ohne Hotpath-I/O; `ArmedCapture` ist auf ein kurzes Frameintervall
  begrenzt. Strings, JSON, Screenshots, Payload-Hashes, zweite
  Geometriepassagen und synchroner GPU-Readback gehoeren nicht in den
  permanenten Draw-Hotpath. Binding-Provenienz (Texlist, Resolver, Epoch,
  Last Writer) und Resource-Provenienz (Content-SHA, Generation, Archivslot)
  bleiben getrennt und werden erst offline menschenlesbar dekodiert.
- Zyklusfortschritt wird primaer als weitester Storycheckpoint, weitester
  steuerbarer Gameplaycheckpoint, Zahl der Replays jenseits ihres alten Stops,
  geschlossene gemeinsame Root Causes und verbleibende K1/K2/K3-Blocker
  berichtet. Task-, Commit-, Closure- und Grafikbugzahlen sind nur
  Sekundaermetriken. Der aktuelle Produktstand ist M3 (teilweise laufende
  Idle-Demo); das naechste Gate ist M4 (Sonics Story-Intro startet).

## Unveraenderte Produktgrenzen

- KatanaRecomp bleibt ein statischer SH-4-Recompiler.
- KatanaRecomp erzeugt native PC-Ports und keinen Emulator. Der Produktpfad
  besteht aus statisch rekompiliertem SH-4-Code sowie nativer PC-Grafik,
  -Audio, -Datei-, -Eingabe- und Save-Anbindung.
- Kein allgemeiner Interpreter, kein JIT und kein Emulationsfallback im
  normalen Produktpfad. Das gilt auch fuer Geraeteprozessoren: Ein AICA-ARM7-
  Interpreter und zyklusweise Gastfirmwareausfuehrung sind im Produkt
  verboten.
- Kein CPU-PVR-Softwarerasterizer und kein vollstaendiger emulierter
  Dreamcast-Geraeteverbund im Produkt. Grafik laeuft ueber eine native GPU-
  API; Audio und Movie ueber native Hostdienste.
- Plattformgrenzen werden an der hoechsten sicher identifizierten Spiel-/SDK-
  Schnittstelle durch native Hooks ersetzt. Kleine ABI-/Datenadapter sind
  erlaubt, Chip- oder Konsolenemulation nicht.
- Historische Geraetemodelle duerfen nur in einem expliziten, nicht
  verteilbaren Diagnoseprofil erhalten bleiben und duerfen nicht in ein
  Produktbinary gelinkt werden. Es gibt keinen Laufzeitfallback darauf.
- Keine Sonic-spezifischen Adresshacks, Retailbytes oder aus kommerziellen
  Dateien kopierten beziehungsweise ungebunden erzeugten Inhalte im
  generischen Katana-Kern.
- Das reale Produkt und sein Bootfortschritt bleiben autoritativ; ein Review
  darf keine fehlende Produktabdeckung durch erfundene Erfolge oder stilles
  Weglassen von Arbeit kaschieren.
- Geeignete frei verfuegbare Bibliotheken und Komponenten werden bevorzugt
  vollstaendig integriert, um bewiesene Funktion wiederzuverwenden und
  Eigenaufwand zu vermeiden. Lizenz, Redistributierbarkeit, Produktgrenze,
  Packaging und Fail-Closed-Verhalten muessen sauber geprueft sein;
  Eigenimplementierung ist nur zulaessig, wenn keine passende freie Loesung
  existiert oder deren Lizenz beziehungsweise Architektur unbrauchbar ist.

Der vollstaendige verbindliche Vertrag steht in
`docs/NATIVE_PORT_PRODUCT_CONTRACT.md`. Er hat Vorrang vor aelteren
RuntimeOnly-, AICA-, PVR-, Performance- und Handoff-Beschreibungen.

## Laufzeit und Ressourcen

### Generierte Produkt- und Analyseinputs

- Ein teurer Produkt-, Export- oder Analyselauf darf niemals direkt mit
  versionierten `runXX-*`-Kopien, einem lediglich passend benannten Artefakt
  oder einem aus einem frueheren Sourcezustand stammenden Generated Input
  starten.
- Vor jedem solchen Lauf werden die zustaendigen Authoring-Tools aus dem
  aktuellen Source gebaut. GameProject, NativePort-Definition, Runtime-Image
  und weitere generierte Inputs werden kanonisch neu erzeugt oder nur dann
  wiederverwendet, wenn ein maschinengepruefter Provenance-Vertrag aktuelle
  Source-, Generator-, Payload- und Artefakt-SHA-256-Identitaeten bindet.
- Der Consumer liest ausschliesslich die dabei publizierten `current`-
  Artefakte. Direkt vor Prozessstart werden Source und alle Artefakte erneut
  gegen die Provenance gehasht; jede Abweichung beendet den Lauf vor der
  teuren Analyse. Der verwendete Eingabesatz wird als eigenes Manifest neben
  dem Laufprotokoll festgehalten.
- Der private Sonic-Workflow verwendet fuer Analysen ausschliesslich
  `private/run-sonic-native-analysis.ps1`; fuer Exporte bleibt
  `private/run-sonic-native-export.ps1` autoritativ. Ad-hoc-Skripte duerfen
  diese Freshness- und Provenance-Gates nicht umgehen.

- Kein gestarteter Prozess und keine einzelne Phase laeuft laenger als
  20 Minuten. Nur eine ausdrueckliche Nutzerfreigabe fuer genau einen benannten
  Lauf hebt diese Grenze voruebergehend auf.
- Ein abgelaufener oder abgebrochener Prozess wird mitsamt seinem Prozessbaum
  quiesziert, bevor ein Nachfolger startet.
- Produktive Arbeit nutzt die verfuegbaren Hostressourcen parallel;
  Ein-Kern-Ausfuehrung ist kein akzeptabler Default.
- Potenziell lange Produktphasen melden spaetestens alle zehn Sekunden
  belastbaren Fortschritt beziehungsweise einen Heartbeat in ein Log. Die
  Produktoberflaeche selbst bleibt bei automatisierten Tests unsichtbar und
  stumm.
- Lange Prozesse stellen maschinenlesbaren Live-Fortschritt bereit; ein nur
  am Ende ausgegebener gepufferter Log ist unzulaessig. Das erzwingt weder ein
  sichtbares Fenster noch Audioausgabe.
- Ein wiederholter Heartbeat ohne Aenderung von Phase, geplant, queued, aktiv,
  fertig oder kanonisch publiziert ist nur Liveness und kein Fortschritt.
  Bleibt ein Prozess 60 Sekunden ohne nachweisliche Arbeitsbewegung, wird er
  vor der 20-Minuten-Obergrenze als Stall beendet und sein Prozessbaum
  quiesziert.
- Steigende CPU-Last, Evaluation-, Cache-, Miss-, Requeue- oder
  Kontextzaehler gelten fuer sich niemals als Produktfortschritt. Bleiben
  abgeschlossene beziehungsweise kanonisch publizierte Arbeit und der
  Head-of-Line-Fortschritt stehen, waehrend interne Arbeit wiederholt neu
  erzeugt, invalidiert oder verdraengt wird, ist der Lauf nach kurzer
  Gegenprobe als Konvergenz- beziehungsweise Requeue-Fehler abzubrechen. Das
  gilt auch bei einer ausdruecklich aufgehobenen Zeitgrenze.
- Fuer Phasen mit `planned > 0` und `canonical == 0` ist die First-Publish-Zeit
  des letzten gesunden Produktlaufs die verbindliche Vergleichsbasis. Ist sie
  erreicht und bleiben danach drei aufeinanderfolgende 10-Sekunden-Samples
  trotz fertiger/ready Arbeit, steigendem internem Churn und unbewegtem
  Head-of-Line weiterhin bei null, ist der Lauf als Nichtkonvergenzfehler zu
  beenden. Fehlt eine gesunde Vergleichsbasis, ist spaetestens nach drei
  Minuten ohne erste kanonische Publikation eine explizite Fehlerentscheidung
  anhand dieser Signale Pflicht.

Die ausfuehrlichen Projektvertraege in `ROADMAP.md`, `docs/CODEX_HANDOFF.md`,
`docs/TASKS.md` und den aktiven Performanceplaenen gelten zusaetzlich, soweit
sie diesem repositoryweiten Arbeitsvertrag nicht widersprechen. Eine
aktuelle ausdrueckliche Nutzeranweisung hat Vorrang.

## Aktueller nativer Portpfad

- Der strikte Produktpfad bleibt statisch, fail-closed und releasefaehig.
  Historische ARM7-/AICA-, CPU-PVR- und sonstige Geraetemodelle sind nur
  Offline-Referenz und werden weder weiter als Produktarchitektur entwickelt
  noch in das Produkt gelinkt.
- Der aktive Bring-up-Pfad arbeitet mit einem stabilen AOT-Pack und einer
  nativen, identity-/generationgebundenen Allowlist. Er sammelt Witnesses und
  reproduziert die erste Divergenz, ohne sie als Produktbeweis auszugeben.
- Titelgebundene Funktionsgrenzen, Roots, Tabellen, dynamische Zielmengen,
  Overlays, Hooks, Patches und Stubs gehoeren mit exakten Bytes und
  Identitaeten in das private Spielprojekt. Der oeffentliche Core validiert
  nur ihre generischen Formate und Vertraege.
- Agenten bearbeiten zuerst den kleinsten konkreten Auftrag aus Replay,
  Witness oder typisiertem Stop. Eine generische Analyzerregel ist nur dann
  der richtige Fix, wenn Disassembly und weitere Evidence ein
  address-agnostisches Muster tragen; andernfalls bleibt der Fix eng privat
  identity-bound.
- Fuer jeden Task gilt weiterhin:
  **implementieren -> betroffene Pfade reviewen und Findings schliessen ->
  dirty konfigurieren und gezielt bauen -> erst danach auf main pushen**.
