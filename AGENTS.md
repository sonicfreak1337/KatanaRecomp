# Verbindliche Arbeitsregeln fuer das gesamte Repository

Diese Regeln gelten fuer jeden automatisierten Bearbeiter, jeden Task, jede
Phase und jeden Teilbereich dieses Repositories. Sie sind keine Empfehlung.
Widersprechende aeltere Prozessbeschreibungen in Roadmap-, Task-, Status-,
Handoff- oder Performance-Dokumenten werden durch diesen Vertrag ersetzt.

## Aktuelle Nutzeranordnung zum Sonic-Export (10. September 2026)

- r347/0.49.9 ist als Mikrobuild exportiert: 94.056 ms, 1 Adapter,
  0 AOT-Compiles, Linkaudit bestanden. Der exakte r346-Crash wurde vorher
  mit isoliertem Save reproduziert; r347 passiert denselben Frame1465.
  Super-Sonic-Folgepfadtest laeuft mit unveraendertem Inputpraefix und
  neutralem Tail weiter. Lokal committen, kein Push.

- Nach dem neuen r346-Crash gibt der Nutzer die autonome Reparatur und
  eigene Super-Sonic-Storytests frei: bis zum Boss laufen nur Cutscenes.
  Gezielte sichtbare Laeufe mit isolierter Kopie des Spielstands und
  Logaufzeichnung statt Vollmatrix; passende inkrementelle Reparaturbuilds
  im bestehenden Produktpfad, danach lokal committen, niemals pushen.

- Auf Nutzer-"build" ist r346/0.49.9 als Mikrobuild exportiert: PVM-
  Publikationsfix und Versionslabel, 73.715 ms Buildhelfer, 1 Adapter,
  0 AOT-Compiles, bytegleicher Pack, Linkaudit bestanden. Jetzt lokaler
  Commit ohne Push. Spieltests bleiben beim Nutzer; kein weiterer Export
  bis zum naechsten Batchsignal.

- Neuer r345-Crash in Super Sonic Story: gemeinsame PVM-Texturverwaltung
  bei EV00F3 -> EV00F4. Nutzer verlangt Pruefung der kompletten Folgepfade
  und setzt die naechste Version auf 0.49.9; 0.5 erst nach Super Sonic und
  Polish. Quellarbeit und begrenzte Familienpruefung, keine Zusatztestserien
  oder Agenten-Spielstarts. Noch kein neuer Exportauftrag.

- Der Nutzer bestaetigt in r345: "sonic story beendet." Alle sechs
  regulaeren Stories sind damit durch Nutzerlaeufe abgeschlossen. Naechster
  Story-Meilenstein ist Super Sonic; Chao bleibt separat offen. Daraus folgt
  kein neuer Exportauftrag und kein Agenten-Spielstart. Exportversion dieses
  bestaetigten Meilensteins: 0.49.8; naechste Quellversion: 0.49.9.

- Auf Nutzer-"go" ist r345/0.49.8 exportiert: elf Egg-Viper-Handler und
  21 Chao-Race-Roots; 14 AOT-Compiles, 1098 Partitionen wiederverwendet,
  Linkaudit bestanden. 492.871 ms Exportkern, ca. 8:31 Minuten ganzer
  warmer Aufruf. Keine Zusatztests oder Agenten-Spielstarts. Jetzt lokal
  committen, nicht pushen; der Nutzer testet selbst.

- Nach der r345-Quellvorbereitung beanstandet der Nutzer zu viele Tests.
  Keine weiteren Zusatztests, Coverage-Verifier oder Wiederholungsreviews
  fuer diesen Batch. Der bereits bestandene Input-Preflight wird nicht nur
  zur Beschaeftigung wiederholt. Naechster Produktschritt ist der einzelne
  inkrementelle Export nach Batchfreigabe, dann Nutzer-Spielpruefung und
  lokaler Commit. Keine Agenten-Spielstarts; kein Push.

- Der anschliessende r344-Nutzerlauf erreicht Egg Viper nach Final Egg.
  Neuer Stop: B_EGM3 fehlender Einsprung 8AB4; Quellbatch r345 ergaenzt
  elf belegte Tabellenhandler, nicht die danebenliegenden Zustandsnamen.
  Integrierter Input-Preflight bestanden, noch kein Produktexport. Nutzer
  uebernimmt weiter Spieltests; neue Crashmeldung ist kein Exportsignal.

- Am 10. September schliesst der Nutzer r343 nach dem einzigen neuen Crash
  nach Final Egg. r344/0.49.8 ist als Mikrobuild exportiert: 88.429 ms im
  Buildhelfer, ca. 96 Sekunden gesamter warmer Aufruf, 1 Adapter, 0 AOT-
  Compiles, Pack bytegleich und Linkaudit bestanden. Enthalten sind der
  quellbelegte SDK-Null-Release-No-op und begrenzte Emblem-Listendiagnostik.
  Light Speed Dash wurde in r343 vom Nutzer bestaetigt. Neue Chao-Roots
  bleiben im spaeteren AOT-Batch. Nutzer uebernimmt Spieltests; keine eigenen
  Starts. Nach diesem Produktbuild lokal committen, nicht pushen.

- Am 10. September priorisiert der Nutzer erneut "Sonic ist der Test" und
  einen schnellen Build. r343/0.49.8 ist daraufhin als Mikrobuild mit Story-
  Freigabe- und Dash-Materialfix exportiert: 84.207 ms, 1 Adapter, 0 AOT-
  Compiles, alter Pack bytegleich, Linkaudit bestanden. Die vorbereiteten
  21 Chao-Race-Roots sind NICHT enthalten; fuer deren spaeteren AOT-Batch
  liegt die alte Inputkette als verifizierter Candidate-Checkpoint vor.
  Nutzer testet selbst; keine Agenten-Spielstarts. Jetzt lokal committen,
  nicht pushen; keinen zweiten Export vor dem naechsten Nutzersignal.

- Am 10. September schliesst der Nutzer den r341-Batch mit "batch vollstaendig".
  r342/0.49.8 ist inkrementell exportiert: 1 Adapter, 0 AOT-Dateien,
  81.635 ms im Produkthelfer, unveraenderter Pack, Linkaudit bestanden.
  Enthalten: SDK-Referenzfreigabe vor authentifiziertem Carrier-Overwrite,
  erfolgreiche SDK-Texturauswahl im nativen Chao-Indexschatten und korrekte
  SDK-Basic-PCW/TSP/ARC1-Zustaende fuer den Dash-Pfad. Beide Crashreplays
  und der sichtbare Dash-Effekt sind noch nicht im Spiel bestaetigt.
  Der Nutzer testet selbst; keine eigenen Spielstarts. Lokal committen,
  nicht pushen. Keine neuen Exporte waehrend seiner naechsten Batchsammlung.

- Am 10. September schliesst der Nutzer den r340-Batch mit neuem Sonic-
  und Chao-Crash sowie weiterhin falschem Light-Speed-Dash-Effekt. Auf
  "ok build jetzt" wurde r341/0.49.8 als Mikrobuild exportiert: 1 Adapter,
  0 AOT-Dateien, alter Pack bytegleich, Linkaudit bestanden. Die drei Fixes
  sind enthalten, Spielbestaetigung steht aus. Der Nutzer testet selbst;
  keine Agenten-Spielstarts. Nach dem Build lokal committen, nicht pushen.

- Am 10. September bestaetigt der Nutzer Gamma und Knuckles Story zu 100 Prozent in
  r339/0.49.6. Der naechste Export erhaelt 0.49.8; der zuvor vorgesehene
  Gamma-Bump 0.49.7 wurde noch nicht exportiert. Der Nutzer hat den Batch
  anschliessend geschlossen und r340/0.49.8 zum Export freigegeben. Er
  uebernimmt weiter die Spieltests; Sonic und danach Super Sonic sind offen.
  Vor Start dieses Exports ist auch der neue Chao-Garden-Crash aufzunehmen.
  Der Nutzer verlangt die Pruefung des gesamten Chao-Moduls einschliesslich
  Garten-/Rennmodulen und fehlender Callback-/Zustandsfamilien; belegte Luecken
  werden im selben Batch ergaenzt, ohne bestehende Einsprungstellen zu verlieren.

- Am 10. September bestaetigt der Nutzer Amy Story clear und korrekt
  laufende Credits in r338/0.49.5. Die naechste Version wird 0.49.6.
  Der Nutzer hat den r338-Batch nach Gamma-Boss, Knuckles und Sonic Final Egg
  geschlossen. Der gebuendelte Reparaturexport 0.49.6 ist freigegeben;
  Spieltests bleiben beim Nutzer. Keine eigenen Spielstarts. Die breite,
  ungemessene Integer-Load-Codegenoptimierung bleibt im Performancebatch.
  Beim r339-Export wurde das Ninja-v7-Log durch einen falschen alten
  Ninja-Diagnoseaufruf verloren. Die echte r318-Sicherung plus natives
  `ninja -t restat` restauriert Dateizeiten aus den vorhandenen Objekten;
  alle 1781 echten Befehls-Hashes bleiben unveraendert. Der gepruefte Plan
  benoetigt 136 AOT-Compiles wegen fehlender/geaenderter Logeintraege und
  einer tatsaechlich geaenderten Partition. Nur fuer diese Wiederherstellung
  gilt Budget 136; keine Logeintraege/Hashes erfinden. Nach erfolgreichem
  Export das neue Log sichern. Normale Crashbatches bleiben bei Budget 32.
- Der Nutzer hat den r330-Batch nach Knuckles und Amy nach Zero geschlossen.
  Export ist nach den Crashfixes und den gemeldeten Grafikbugs einschliesslich
  Credits freigegeben. Anschliessend folgt die ausdruecklich beauftragte
  autonome Performancephase auf Basis von Grounders Empfehlungen mit neuen
  Messungen und Pruefung des originalen Spieltimings. Eigene Spielstarts und
  gezielte Tests sind waehrend seiner Abwesenheit wieder freigegeben; Saves
  bleiben isoliert, Messungen ohne parallele Builds. Diese aktuelle Freigabe
  ersetzt die aelteren Verbote von Agenten-Spieltests weiter unten.
  Anschliessend lokal committen, niemals pushen. 0.49.5 markiert Big Story clear.
- Der Nutzer hat r329 mit "go" und "du darfst bauen. moeglichst inkrementell"
  ausdruecklich freigegeben. Der vorbereitete Boss-/Uebergangsbatch inklusive
  Chaos 7 verwendet den bestehenden Performance-Buildbaum. Der r328-
  Analysecheckpoint lehnt die veraenderte Rootmenge korrekt ab; vorhandene
  Root-/Modul-Caches bleiben aktiv. Der gepruefte Ninja-Plan enthaelt genau
  35 neue/veraenderte AOT-Dateien, 1.063 bleiben bytegleich, alle gemeinsamen
  generierten Header bleiben unveraendert. Nur fuer diesen Batch gilt deshalb
  das belegte Compilebudget 35 statt 32. Spieltests bleiben beim Nutzer.
- Der r329-Quellbatch behandelt die fuenf neuen r328-Storycrashes sowie
  aehnliche Boss-/Uebergangsfamilien. Zusaetzliche Performanceoptimierungen
  sind auf Nutzeranordnung bis zur naechsten Performancephase zurueckgestellt.
  Der Nutzer uebernimmt die Spieltests; kein Agent startet game.exe.
- Am 9. September hat der Nutzer nach Big vor Chaos 6 den r327-Crashbatch
  geschlossen und den naechsten Export nach dessen Fix ausdruecklich freigegeben.
  Der gesamte vorbereitete r328-Batch wird als 0.49.4 exportiert und danach
  lokal committed, ohne Push. Neben einem Nutzerlauf startet kein Agenten-Spieltest.
- Der Nutzer uebernimmt den r328-Spieltest selbst; kein eigener Starttest.
- Schnelle Crashbatches bleiben inkrementell. Zusaetzliche Performanceaenderungen
  an gemeinsamen Runtime-Headern oder am Codegenerator, die breite AOT-Neucompiles
  ausloesen, werden nicht in einen wartenden Crash-Reparaturbuild aufgenommen.
  Sie gehoeren in einen getrennten Performancebatch. Notwendige semantische
  Invalidierung wird niemals umgangen. Vor dessen Export wird die erwartete
  Compile-Reichweite geprueft; r328 mit 872 geaenderten und 215 durch den Header
  ebenfalls invalidierten AOT-Einheiten ist kein akzeptabler schneller Loop.
- Der r321-Crashbatch ist vom Nutzer geschlossen und zum Export freigegeben.
  Anschliessend sind autonome Performancebuilds und eigene Leveltests fuer
  die angeordnete Optimierungsphase freigegeben: einmalige Levelmatrix mit
  Steuerung und hoechstens 60 Sekunden Gameplay je Stage, danach gezielte
  Wiederholungen der betroffenen Pfade. Ziel ist CPU-Leistungspuffer fuer
  stabile aktuell konfigurierte 30-Hz-Simulation bei 144-Hz-Praesentation. Der spaetere
  langsame Abschnitt in Amys Hot Shelter bleibt ohne passende Messung offen.
  Test-Saves bleiben isoliert; waehrend FPS-Messungen laeuft kein Build.
- Waehrend der Nutzer einen Crashbatch sammelt, startet ein Sonic-Produktexport
  erst, wenn der Nutzer ausdruecklich zum Export auffordert.
- Weitere Crashmeldungen und die bisherige Autonomiefreigabe sind kein
  Exportsignal. Ein gestoppter Export wird nicht automatisch fortgesetzt.
- Ursachenanalyse, Familienpruefung und Sourcefixes werden im offenen Batch
  weiter gesammelt. Diese aktuelle Anordnung hat Vorrang vor den folgenden
  aelteren Regeln zum automatischen Export eines fertigen Batches.

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

### Automatische Build-Authority statt Cycle-Freeze

- Es gibt keinen manuellen Cycle-Freeze und keinen Arbeitsstopp vor der
  Implementierung. Streng beweisbare, kompatible Findings werden bis zum
  Beginn des einen Produktexports laufend in den Batch aufgenommen.
- Der Export publiziert automatisch die maschinenlesbare, unveraenderliche
  Build-Authority aus den kanonischen Input-Provenance-, Analyse-, AOT-Pack-,
  Promotion- und Executable-Allowlist-Metadaten. Sie bindet insbesondere
  Source-/Manifest-/Runtime-Identitaet, aktuelle Analyse- und Pack-SHA-256,
  Workflowklasse und Produktbuildbudget `1`; Replayset und Milestones werden
  im zugehoerigen Laufmanifest gebunden. Ein separates manuell gepflegtes
  Freeze-Dokument ist weder erforderlich noch autoritativ.
- Analyse, Produktbuild und Replays duerfen nur gegen diese automatisch
  publizierte Authority laufen. Findings, die nach Beginn des Exports
  eintreffen, gehen ohne nachtraegliches Aufschnueren des Builds in den
  naechsten normalen Batch. Das Ein-Build-Budget bleibt unveraendert.

### Verbindliches Produktbuild-Budget im Native-Bring-up

- Im laufenden Sonic-NativeBring-up ist standardmaessig ausschliesslich ein
  `NativeBringup`-Produktbuild zulaessig. Ein Strict-/Release-/Gate-Build wird
  nur nach einer aktuellen ausdruecklichen Nutzeranweisung fuer genau diesen
  Build gestartet; statischer Fortschritt allein ist keine Freigabe.
- Pro zusammenhaengendem Fortschrittsbatch wird hoechstens ein
  NativeBringup-Produktbinary gebaut. Mehrere kompatible Bug-, Evidence-,
  Analyzer-, Runtime-, Adapter- und Manifestarbeiten werden davor gebuendelt;
  ein Produktbuild pro Einzelbug ist unzulaessig.
- Eggman baut nach jedem sinnvollen, quellseitig geprueften und gezielt
  verifizierten kompatiblen Fortschrittsbatch genau dieses eine
  NativeBringup-Produkt. Damit bleibt echtes Sonic-Feedback regelmaessig;
  offene, noch unbelegte oder grosse Befunde halten einen ansonsten fertigen
  Batch nicht endlos auf, sondern gehen in den naechsten begrenzten Zyklus.
- Dasselbe NativeBringup-Binary wird fuer alle zugehoerigen Pflichtreplays
  sowie Regression-, Progress- und Performanceproben des Batches
  wiederverwendet. Reine Hostaenderungen nutzen den unveraendert gueltigen
  World-/AOT-Pack. Ein zweiter Produktbuild oder zusaetzlicher Strictbuild im
  selben Batch braucht eine neue ausdrueckliche Nutzeranweisung; es gibt
  keinen zeitgesteuerten automatischen Export.
- Gezielte CLI-, Unit- oder Komponenten-Targets zur lokalen
  Quellverifikation sind keine Produktbuilds und bleiben zulaessig. Sie duerfen
  aber weder vorsorglich den Strict-Port bauen noch einen zusaetzlichen
  Produktbuild als Messlauf erzwingen.
- Die aktuelle stehende Nutzerfreigabe deckt alle fuer den voll
  funktionsfaehigen leistungsstarken Sonic-Port notwendigen begrenzten
  Analyse- und Gesamtexportlaeufe ab, auch wenn ihre echte Laufzeit 20 Minuten
  ueberschreitet. Dafuer wird keine erneute Zeitausnahme erfragt. Echte
  Fortschritts- und Stallkontrolle, genau ein Produkt pro kompatiblem Batch,
  produktgegatete lokale Commits, kein Push und keine Emulation bleiben
  verbindlich.

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
  generationgebundenen AOT-Welt, validiert durch die versiegelte
  Blocktabelle und den eindeutigen aktuellen Owner. Mit separat gebundener
  Complete-Disassembly-Coverage ist das Authoring-/Allowlist-Artefakt ein
  optionaler Proof-Beschleuniger, keine zusaetzliche Ausfuehrungsautoritaet.
  Ohne Coverage bleibt die explizite Allowlist Pflicht. Gelieferte Artefakte
  werden weiterhin vollstaendig validiert; sie enthalten keine rohen
  Hostfunktionszeiger und mutieren keine Runtime-Tabelle.
- Ein explizit authorierter `Candidate` darf in dieser nicht releasefaehigen
  Allowlist nur nach unabhaengiger exakter Source-/Callsite-/Target-/Owner-/
  Byte-/Pack-/Generationsvalidierung ausgefuehrt werden. Sein `missing_proof`
  bleibt offen; der Hit darf weder Strict noch eine Frontier schliessen.
  `Observed`, `Unresolved` und blosse RuntimeContracts sind hier nicht
  executable.
- Ein vorheriger Candidate-only-Stand darf den naechsten Ein-Durchlauf-Export
  seed-en. Weichen seine Analyse- oder Packidentitaeten von den aktuellen
  source-bound Inputs ab, darf er nur nach vollstaendiger exakter
  Revalidierung jedes Records auf die im selben Export erzeugte Analyse und
  Blocktabelle rebound werden. Ein `Proven`-Record darf nie auf diese Weise
  rebound werden; jeder Identity-Mismatch bleibt dort fail-closed.
- Ein echter Entry-/Blocktabellen-Miss endet sofort als
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
  nicht automatisch einen Implementierungsauftrag. Eggman entscheidet als
  technischer Leiter nach jeder erfolgreichen Analyse, ob und wie der
  Analyzerstand als read-only Katana-Taskpool inventarisiert und delegiert
  wird.

### Koordinierte Analyse-Inventur und Egg Fleet

- Eggman leitet die gesamte technische Entwicklung von Katana/Sonic und
  finalisiert Architektur, Approach, Prioritaeten und Abnahme. Ein Pool darf
  aus genau der publizierten `materialization-world.katana-world` mit
  `next-analysis-task --format agent-json` erzeugt werden; seine Groesse
  folgt dem konkreten Bedarf und ist kein festes Ziel. Pool-, World-,
  World-JSON- und Native-Analysis-SHA-256 werden vor jeder Delegation
  festgehalten.
- Poolpositionen werden nach Owner, Provider, Frontierart und betroffenen
  Dateien geclustert. Nur konkrete relevante und disjunkte Arbeitspakete
  gehen an die bestehenden Cubot-, Orbot- oder Grounder-Tasks. Diese liefern
  praezise Befunde oder setzen den von Eggman abgegrenzten Scope um. Sage
  uebernimmt unabhaengige Gegenpruefung und Integration und fuehrt keine
  eigene Architektur-, Feature- oder Performance-Roadmap.
  Vorhandene Tasks duerfen inventarisiert werden, erhalten aber keinen
  unnoetigen Auftrag; neue Tasks entstehen nur auf ausdrueckliche
  Nutzeranweisung. Es gibt weder automatisches Fan-out noch eine Wartepflicht
  auf unbenutzte Tasks. Modell und Reasoning werden von Eggman passend zum
  begrenzten Paket festgelegt, nicht repositoryweit fixiert. Vorhandener
  Kontext und vorhandene Evidence werden wiederverwendet; doppelte
  Vollreviews, doppelte Testlaeufe und unbegrenzte Recherche ohne konkrete
  Entscheidungswirkung sind unzulaessig.
- Nur fuer konkrete, begrenzte und disjunkte Arbeit benoetigte Tasks werden
  aktiviert. Zusaetzliche Parallelitaet ist nur zulaessig, wenn sie den
  kritischen Pfad voraussichtlich verkuerzt. Es gibt keine Pflichtauslastung,
  Fuellauftraege, staendigen Statusrunden oder fest auszuschoepfenden Team- und
  Modellkontingente. Nach einem Handoff bleibt ein Task ohne Anschlussauftrag
  idle.
- Jeder delegierte read-only Analyse-Task prueft Pool-zu-World, den Delta zur
  zuletzt von ihm geprueften
  Generation, die aktuelle Source-/Disassembly-/Provider-Evidence,
  A/B/C-Klassifikation, kleinsten fail-closed Scope, Acceptance, Kollisionen
  und Multi-Close. Sein maschinenlesbarer Handoff bindet zusaetzlich World-
  und Pack-SHA, Task-IDs, Root-Cause-Key, betroffene Dateien, Collision-Key,
  Replay-Reachability, Multi-Close-Set, naechsten erreichbaren
  Story-/Gameplaycheckpoint und beantwortet fuer jeden Frontier:
  aktiver Sechserpfad, gemeinsamer Replay-Close, generischer oder
  Sonic-spezifischer Ursprung, `sad_disasm`-Beweischance, strenger
  identity-bound Titelvertrag und erwarteter neuer Produktpfad.
  Handoffs bleiben kompakt und enthalten Beweis, exakten Diff- oder
  Dateiscope, ausgefuehrte Checks und verbleibende Restunsicherheit.
- `A` bedeutet: aktuelle World und Source, genaue Ursache und Implementierung,
  keine offene Evidencefrage; der Fall wird in diesem Zyklus umgesetzt.
  Aktuelle Replay-/Produkt-Reachability oder klarer Multi-Close erhoehen die
  Reihenfolge, sind aber keine Voraussetzung: Jeder streng beweisbare
  Hardware-Owner wird im selben Zyklus geschlossen, auch wenn ihn erst ein
  spaeterer Story- oder Gameplaypfad erreicht.
  `B` ist ein bestaetigter, aber fuer den aktuellen Batch nicht
  erreichter oder anderweitig blockierter Befund. `C` ist stale, duplicate,
  bereits geschlossen, falsch interpretiert, unzureichend bewiesen oder
  ausserhalb des aktuellen Produkts und wird nicht implementiert.
- Read-only Analyse-Tasks duerfen weder Dateien noch Pools aendern oder
  erzeugen und keine
  Builds, Tests, Replays, Commits oder Pushes starten. Fehlende Evidence ist
  niemals `unreachable`, und Runtime-Witnesses werden nie zu statischem Proof
  hochgestuft.
- Replay-Sammlung, read-only Inspektion und Produktvorbereitung duerfen
  parallel zu delegierter Analyse laufen. Eggman nimmt relevante A-Faelle,
  die vor Beginn des Exports eintreffen, in den Batch auf; spaetere gehen in
  den naechsten normalen Batch. Ein ungenutzter Task oder ausstehender
  irrelevanter Handoff blockiert weder Implementierung noch Produktbuild.

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

### Global-first und Family-first sind Pflicht

- Vor jedem Zugriff auf einen erhaltenen Ninja-Buildbaum wird der absolute
  `CMAKE_MAKE_PROGRAM`-Pfad aus dessen Cache gelesen. Kein nackter `ninja`-
  Aufruf aus dem PATH fuer Dry-runs. Die Version wird ohne `-C` geprueft;
  ein unbekanntes/inkompatibles Log darf nicht probeweise geoeffnet werden.
  Die privaten Sonic-Wrapper sichern vor und nach erfolgreichen Builds
  `.ninja_log`, `.ninja_deps`, CMakeCache und Buildgraph mit SHA-Bindung.
  Die Sicherung ist keine automatische Wiederverwendungsautoritaet und
  schreibt keine Logrecords oder Befehls-Hashes um.
- Ein kopiertes Codegen-v2-Manifest beweist keine unveraenderte lokale
  Dateiidentitaet: Windows-Datei-ID und ChangeTime sind ebenfalls gebunden.
  Bei manueller Wiederherstellung muss der vollstaendige Ist-/Soll-Dateisatz
  geprueft werden; belegte Altdateien werden separat erhalten statt als
  aktuelle Generierung ausgegeben. Die Loeschschutzpruefung des Generators
  und die abschliessende exakte Manifestinventur bleiben aktiv.
- Aktuelle Bring-up-Vorgabe: Bereits belegte Sonic-Adressfamilien duerfen
  vorerst vollstaendig als private, image-/byte-/generationgebundene
  Candidate-Vertraege erschlossen werden. Eine neue generische Analyzerregel
  ist dafuer keine Voraussetzung. Die spaetere Verallgemeinerung erfolgt mit
  mehr Spielevidence; bestehende generische Erkennung bleibt erhalten.
  Der Scope bleibt die gesamte belegte Familie, niemals nur der Crash-PC.
  Builds verwenden vorhandene Caches und kompilieren geaenderte AOT-
  Partitionen inkrementell; fehlende Ziele erlauben keinen Runtime-Fallback.
- Vor dem Compilerstart eines kleinen Sonic-Adressfamilienbatches wird der
  tatsaechliche Ninja-Dry-Run mit Dirty-Edge-Begruendungen geprueft.
  `KATANA_MAX_AOT_UNIT_COMPILES` begrenzt die erneut kompilierten `unit-*`-
  Dateien; der Sonic-Wrapper setzt standardmaessig 32. Ein ueberschrittener
  Plan startet keinen Compiler. Eggman behebt zuerst die Wiederverwendung
  vorhandener Objektdateien. Ein nachweislich notwendiger Kalt-/ABI-Rebuild
  bekommt ein explizit begruendetes passendes Budget, keine neue Nutzerfreigabe.
  Compiler-Cachetreffer ersetzen den direkten inkrementellen Build nicht.
- Eine Guestadresse, Crash-PC, Callsite oder ein einzelner Frontier ist immer
  nur Witness und niemals automatisch die Implementierungseinheit. Vor jedem
  Sourceedit wird das vollstaendige verifizierte Primary Image sowie jedes
  gebundene Overlay-, Loaded-AOT- und sonstige Modul derselben Lifecycle-
  Generation nach demselben Producer-, Consumer-, Owner-, Provider-, Tabellen-,
  Callback-, VTable-, CFG- oder Datenflussmuster durchsucht.
- Bei einer beauftragten Verallgemeinerung eines address- und
  titelunabhaengigen Musters wird genau eine
  generische, fail-closed Katana-Regel implementiert. Ist nur die Bedeutung
  titelspezifisch, wird genau ein privater identity- und generationgebundener
  Familienvertrag fuer alle bewiesenen Mitglieder implementiert. Einzelne
  Adressfixes sind ausschliesslich zulaessig, wenn beide Wege nachweislich
  unmoeglich sind; die verworfenen Generalisierungen und ihr Beweis muessen im
  Handoff und in der Acceptance stehen.
- Der globale Scan ist generationslokal: Erkenntnisse aus Primary Image,
  Overlay oder Loaded-AOT duerfen nie roh zwischen Images oder retired
  Generations uebernommen werden. Positiv inventarisierte Ziele bleiben an
  Image-/Modul-SHA, Runtime-/Source-Basis, Bytebereich und aktive Generation
  gebunden; unbekannte Mitglieder und dynamische Zielmengen bleiben offen.
- Eine Load-Phase ist keine Source-Komponentenidentitaet. Bei statischen
  Callback-/VTable-/Codepointer-Vektoren darf der lesbare Vektortraeger (zum
  Beispiel ein System-Bootstrap) getrennt von der aktiven Spielkomponente
  sein; promoviert werden aber nur Ziele, deren `source_kind`, `load_phase`
  und nichtleere `local_source_name` einer bereits aktiven Komponente derselben
  `ExecutableImage`-Generation entsprechen. Fehlt die Sourceidentitaet, gilt
  fail-closed nur das exakte Segment. Gleiche Load-Phase oder ein P1/P2-Alias
  allein ist niemals Owner-/Entry-Proof.
- FunctionMap-, GDI-, ProgramIndex-, Tabellen-, Callback- und CodeIdentity-
  Fakten werden zentral aus der gebundenen Authority materialisiert. Manuelle
  Wiederholung derselben Adresse in mehreren privaten Listen ist kein
  Ersatzbeweis. Eine frische Whole-Game-Analyse muss nach jeder globalen Regel
  Family-Coverage, Null Proof-Downgrades und Null neue ungebundene Roots zeigen.
- Replayprioritaet bestimmt die Reihenfolge, nicht den Scope. Ein Fix gilt erst
  als Family-Close, wenn alle im Whole-Game-Scan gefundenen Mitglieder
  klassifiziert sind: geschlossen, streng dynamisch gebunden oder mit exakt
  benannter fehlender Evidence weiterhin fail-closed.

## Projektweiter Taskablauf

Fuer jeden Task gilt ab sofort genau diese Reihenfolge:

```text
Task implementieren
  -> alle durch den Task betroffenen Pfade reviewen
     und bestaetigte Fehler innerhalb desselben Reviewdurchlaufs schliessen
  -> den reviewten Stand im dirty Worktree konfigurieren und gezielt bauen
  -> verifizierten Dirty-Diff an Eggman uebergeben
  -> nach Integration genau ein Sonic-NativeBringup-Produkt bauen
     und die relevanten Produkt-/Replaypruefungen ohne Progressrueckschritt bestehen
  -> Eggman committet den geprueften zusammengehoerigen Source-Batch
  -> Push nur nach aktueller ausdruecklicher Nutzerfreigabe
```

- Die Reviewstufe ist die Fehlerfindungs- und Fixstufe. Sie umfasst den
  implementierten Pfad, seine Aufrufer und Verbraucher, Verdrahtung,
  Datenfluss, Fehlerpfade, ABI-/Cache-/Versionsvertraege sowie alle weiteren
  unmittelbar betroffenen Schichten.
- Bestaetigte Korrektheits-, Boot-, Vollstaendigkeits- und relevante
  Performancefehler im Taskscope werden vor Build und Handoff geschlossen.
  Eine separate nachgelagerte Fix- oder Testphase wird daraus nicht erzeugt.
- Ein Commit ist niemals Voraussetzung fuer einen lokalen Build. Nach
  Sourceaenderungen wird der bestehende Buildbaum zuerst neu konfiguriert,
  damit ein dirty Entwicklungsbinary die untrusted Source-Identity einbettet.
  Reiner Compile-, Unit- oder Komponententesterfolg loest keinen Commit aus.
  Erst ein funktionierender Sonic-NativeBringup-Produktbuild mit bestandenen
  relevanten Produkt-/Replaypruefungen und ohne Rueckschritt gegen die
  vorhandene Progress-Evidence autorisiert Eggman, den geprueften
  zusammengehoerigen Source-Batch zu committen. Die stehende Nutzerfreigabe
  fuer genau diesen produktgegateten Commit erfordert keine neue Einzelrueckfrage.
- Der kanonische Windows-Build des dirty CLI-Targets laeuft ueber
  `tools/build-katana-cli.ps1`. Der Wrapper importiert und validiert die
  native x64-Visual-Studio-Umgebung, konfiguriert den vorgesehenen Ninja-
  Releasebaum und baut `katana-recomp` mit dem expliziten Jobbudget. Ein
  direktes `cmake --build` aus einer nicht validierten Shell ist unzulaessig.
- Tasks werden standardmaessig als reviewte Dirty-Diffs im aktuellen Baum
  bearbeitet. Nur Eggman integriert und erstellt den produktgegateten Commit;
  private Retailbytes, Saves, Buildartefakte und fremde oder sachfremde
  Aenderungen duerfen darin nicht enthalten sein. Push, neue Taskbranches,
  Pull Requests oder parallele Integrationszweige werden nur auf eine aktuelle
  ausdrueckliche Nutzeranweisung angelegt oder ausgefuehrt.
- Parallele schreibende Agents teilen niemals denselben Worktree, Git-Index
  oder Buildbaum. Der Haupttask erzeugt fuer jeden Writer einen eigenen
  detached Worktree aus demselben aktuellen Dirty-Snapshot, weist einen
  disjunkten Dateiscope und ein eigenes Buildverzeichnis zu und integriert
  danach nur den reviewten Patch in den Hauptbaum. Die Writer committen,
  pushen, staschen, resetten oder bereinigen ihre Worktrees nicht selbst.
- Schreibende Agents wenden Hunks ausschliesslich ueber den lokalen,
  synchronen Codex-Patch-Endpunkt
  'codex.exe --codex-run-as-apply-patch' in ihrem isolierten Worktree an.
  Der zentral vermittelte beziehungsweise asynchrone App-Patchdienst ist fuer
  Repository-Edits unzulaessig, sobald er Dateischreibvorgaenge serialisiert
  oder die 60-Sekunden-Stallgrenze erreicht; derselbe gestallte Vorgang wird
  nicht erneut dort gestartet. Direkte Dateiueberschreibungen, Shell-
  Umleitungen und Skript-Rewrites bleiben ebenfalls unzulaessig.
- Read-only Reviewer duerfen den Hauptbaum weiterhin parallel inspizieren.
  Routinemessungen verwenden dateigenaue Abfragen und `git status
  --untracked-files=no`; eine vollstaendige Untracked-Inventur erfolgt nur
  gezielt vor Integration. So werden grosse Build-/Analysebaeume nicht bei
  jedem Status- oder Patchschritt erneut traversiert.
- Ein Review darf ausserhalb des aktuellen Taskscopes liegende Beobachtungen
  knapp dokumentieren, aber daraus weder eigenmaechtig neue Tasks noch eine
  Scope-Erweiterung ableiten.
- Eggman legt die Taskreihenfolge und die Handoff-Grenzen fest. Ein
  erfolgreicher gezielter Build und reviewter Dirty-Diff koennen den naechsten
  kompatiblen Task freigeben; sie loesen weder Worker-Commit noch Push aus.

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
- Gezielte Tests duerfen ohne weitere Einzelgenehmigung erstellt, erweitert,
  gebaut und ausgefuehrt werden, wenn sie einen konkreten aktuellen Pfad,
  Fehler oder Vertrag des realen Sonic-Ports direkt absichern. Das umfasst
  vorhandene und neue fokussierte Audio-, Renderer-, AOT- und andere
  Regressionstests fuer nachgewiesene Sonic-Probleme. Der reale
  Sonic-Produktpfad bleibt der Integrationsnachweis.
- Breite Testmatrizen, synthetische Stresslaeufe, allgemeine Testprojekte,
  Ersatzgates oder Konformitaetssuiten ohne unmittelbaren Sonic-Bezug werden
  weiterhin weder gebaut noch gefordert.
- Reviews duerfen einen fokussierten Sonic-bezogenen Test als
  Abschlussbedingung verlangen. Vorhandene Tests duerfen auf gebrochene
  Erwartungen, falsche Testzahlen, widerspruechliche Semantik oder bereits
  vorhandene Fehler geprueft, repariert und erweitert werden.
- Regulaere Tasks starten keine unverbundene breite Testmatrix. Gezielte
  Komponenten- und Regressionstests sind Quell- und Vertragsnachweise, aber
  kein Ersatz fuer den Sonic-Produkt-, Fortschritts- oder
  Performancenachweis.
- Sonic-Produktlaeufe erfolgen an den in Roadmap und Tasks festgelegten
  Produktgates oder innerhalb der kleinen, reproduzierbaren Bring-up-Schleife.
  Mehrere zusammenhaengende reviewte Tasks duerfen vor dem naechsten
  vollstaendigen Produktgurt in denselben geprueften Dirty-Batch aufgenommen
  werden.
- Sonic ist der konkrete Produkt- und Fortschrittstest. Vollmatrizen sind
  kein Standardgurt und werden nicht pro Build oder Batch wiederholt.
  Breitere Level-/Charaktermatrizen sind nur bei echten Engineaenderungen
  oder Grafiktests mit begruendetem Abdeckungsbedarf zulaessig. Ansonsten
  werden der aktuelle Nutzerreplay bis ueber den bisherigen Fehler hinaus
  und die direkt betroffenen Regressionen gezielt geprueft. Historische
  Passes behalten ihre Buildidentitaet; unbelegte Pfade gelten nicht als Pass.
  Debug-Gameplayproben dauern hoechstens 60 Sekunden, sequenziell,
  unsichtbar und stumm; Grafikdiagnostik bleibt ohne konkreten Bedarf Off.
- Dieselbe EXE prueft den aktuellen Sonic-Story-Pfad als gezielte Replayzeile
  mit hoechstens 300 Sekunden Laufzeit. Dieser ausdruecklich angeordnete Storytest ist
  die Ausnahme vom 60-Sekunden-Limit der Debug-Leveltests. Sein vorhandener
  Replaypfad, erreichte Story-Meilensteine, Crashes und Performance werden
  separat gebunden; ein Debug-Levelstart ersetzt den normalen Storypfad nicht.
  Bestandene Debug-Level behalten ihre urspruenglichen Build-Identitaeten.
- Wird eine begruendete Engine-/Grafikmatrix ausgefuehrt, subsumiert sie
  gleichwertige Einzelreplays. Eine pauschale zusaetzliche Sechsermatrix
  wird nicht ausgefuehrt; auch andere Sechsergurt-Verweise begruenden keine
  automatische Vollmatrix gegen den aktuellen gezielten Sonic-Testscope.
  Ein nachgewiesener Fehler im gemeinsamen Testprotokoll darf den Lauf
  vorzeitig beenden: Teilbefunde bleiben erhalten, kein betroffener Fall gilt
  als bestanden, und der korrigierte Lauf prueft die weiterhin offenen Faelle.
  Im unsichtbaren Modus ist eine sichtbare DXGI-Presentation kein Pass-Gate.
  Hier sichern aktive Eingaben, exakter aktueller Stage-/Owner-Nachweis und
  nachfolgend abgeschlossene neue Renderer-Draw-Frames den Gameplayfortschritt.
  Leere Frames, Queue-Praefixe und Bildwiederholungen zaehlen nicht dazu.
  Renderer-Command-Abschluss ist weder GPU-Fence-Retirement noch sichtbare
  Bildausgabe; die 144-FPS-Zielerreichung bleibt separat zu belegen.
  Erst werden alle Crashs und Typed Stops gesammelt, dann entsteht daraus der
  naechste begrenzte gemeinsame Fixbatch. Bestandene Kombinationen werden mit
  Build-ID und Meilensteinen im Erfolgsprotokoll vermerkt, aus der aktiven
  Crashmatrix entfernt und ohne relevante Regressionsevidence nicht
  zwanghaft erneut ausgefuehrt.
- Jeder dieser Level-Runs fordert mit dem vorhandenen Produktschalter 144 Hz
  an. Dauerhaftes P0-Ziel ist eine unabhaengig getaktete Bildpraesentation bis
  144 FPS im 144-Hz-Modus (6,94 ms pro Praesentation), bei korrektem Spieltempo,
  Audio und Input. Sonic behaelt vorerst die konfigurierte 30-Hz-Game-Clock;
  deren Bildproduktion muss stabil erreicht werden. Die Timingpruefung vom
  9. September ist wegen falscher Original-RAMdump-Offsets bis zur erneuten
  Bytebindung nicht als Beleg zulaessig. Originaltempo bleibt gesondert zu belegen;
  weder 30 noch 60 Titelupdates werden allein aus dem Anzeigeprofil abgeleitet.
  CPU-Arbeit pro Simulationsframe und CPU-Auslastung im gleichen Gameplayfenster
  sind eigene P0-Messgroessen; hoehere FPS allein beweisen keine Effizienzsteigerung.
  Wiederholungen des letzten
  vollstaendigen Bildes sind fuer das Praesentationsziel ausdruecklich erlaubt.
  144 unterschiedliche Bewegungsbilder beziehungsweise Renderinterpolation
  sind ein separater Ausbau und keine Voraussetzung dieses Ziels. Weder der
  Gast-Spielzeittakt noch generische Host-Servicegrenzen werden dafuer pauschal
  beschleunigt oder umgestellt. Die guenstige Telemetrie weist Simulation-,
  Presentation- und tatsaechlich gezeichnete FPS getrennt aus; wiederholte
  Presentations werden separat berichtet. Framezeiten und Ladezeit werden
  getrennt ausgewiesen. Partielle Messwerte bis zu einem Crash
  oder Typed Stop bleiben erhalten. Verglichen werden nur kompatible Szenen,
  Meilensteine und Messbedingungen. Fehlende Werte bleiben explizit offen,
  und Performancegewinne werden weder geschaetzt noch aus inkompatiblen
  Laeufen abgeleitet.
- In normalen Zyklen ohne angeordnete Vollmatrix werden mit demselben bereits
  vorhandenen BringUp-Binary zuerst die sechs repraesentativen ersten
  Crash-/Typed-Stop-Befunde aufgenommen. Vor Abschluss der jeweils aktiven
  Crashmatrix werden aus Einzelbefunden keine Fixes und keine Zwischenbuilds
  erzeugt.
- Erst danach werden die Ursachen gemeinsam mit den relevanten
  Egg-Fleet-/RuntimeOnly-Befunden als ein zusammenhaengendes Fixpaket
  umgesetzt. Fuer dieses Gesamtpaket folgt genau ein neuer NativeBringup-
  Produktbuild; ein Build pro Replay oder pro Bug ist unzulaessig.
- Der gemeinsame Zyklusbatch umfasst die aktive Crashmatrix sowie alle vor
  Beginn des Exports bereits eingegangenen relevanten aktuellen und
  historischen A-Befunde. Ausstehende read-only Egg-Fleet-Handoffs blockieren
  den Produktbuild nicht; spaete A-Befunde gehen in den naechsten normalen
  Batch. Irrelevante oder nachweislich ueberholte Befunde werden mit Evidence
  begruendet ausgelassen.
- Das Commit-Gate verlangt ein funktionierendes Produkt ohne Rueckschritt
  gegen vorhandene Progress-Evidence, nicht die pauschale Behauptung, bereits
  jede offene Level-/Charakter-Kombination sei spielbar. Bekannte unveraenderte
  Typed Stops werden ehrlich im Matrixprotokoll dokumentiert und blockieren
  den produktgegateten Commit nicht als angebliche neue Regression.
- Eggman bestimmt aus den Matrixmessdaten die naechste begrenzte
  Produktoptimierung; das Team setzt genau dieses konkrete Paket um.
  Progress-/Saveerhalt, vollstaendige Exportzeit, tatsaechliche
  Spielperformance und Kosteneffizienz bleiben gleichrangige P0-Ziele. Die
  Messung laeuft im normalen Gurt derselben EXE und erzeugt weder einen zweiten
  Produktbuild noch eine reine Zusatzmessrunde.
- Automatisierte Sonic-Laeufe und gezielte Tests sind standardmaessig stumm,
  unsichtbar und ohne Screenshot-/Audio-Capture. Ein sichtbarer Lauf erfolgt
  nur auf ausdrueckliche Anforderung. Redundante Replays, die denselben
  frueheren Checkpoint nur langsamer erreichen, gehoeren nicht in den
  Standardgurt.
- Sonic-Produktexporte werden ausschliesslich mit Host-Buildprofil
  `performance` gebaut. `bringup` und `gate` sind dafuer nicht zulaessig;
  der NativeBringup-Ausfuehrungsvertrag ist davon unabhaengig und bleibt
  bestehen. Der kanonische Export-Wrapper weist andere Hostprofile ab.
- Der kanonische Sonic-Export behaelt die bisherige `heuristic`-Modulerkennung.
  Ein Wechsel zu `exact-only` ist kein Performancefix und darf vorhandene
  ungesaete Module nicht aus dem Produkt entfernen. Die vorigen und neuen
  Exportparameter sowie Modulidentitaeten werden vor Abnahme verglichen.
- Jeder vollstaendige Export ueber zehn Minuten ist P0. Der gesamte echte
  Kaltexport muss unter zehn Minuten fallen, einschliesslich Authoring,
  Analyse, AOT-Codegen, Compile, finalem Link und Packaging. Kalt bedeutet
  ohne passende Build-/Compiler-/Artefaktcachetreffer; warme oder rein
  inkrementelle Werte erfuellen dieses Kaltziel nicht. Exportoptimierung
  darf weder Execution-Safety noch vorhandenen Story-/Gameplay-/Savefortschritt
  abschwaechen. Reine Runtimefixes muessen den gueltigen AOT-Pack ohne
  unnoetige Vollanalyse und AOT-Neukompilierung wiederverwenden.
- Performance wird am realen End-to-End-Produktpfad gemessen. Synthetische
  Zeiten, gruene Testmatrizen oder technische Hilfsframes sind kein Ersatz
  fuer Kaltbuildzeit, vollstaendigen Export und sichtbaren Sonic-Lauf.
  Vollstaendiger Export einschliesslich echter Kaltpfade und tatsaechliche
  Spielperformance sind neben fortschreitendem Story-, Gameplay- und
  Savezustand dauerhafte P0-Ziele. Kosteneffiziente Umsetzung ist
  gleichrangiges P0: begrenzte disjunkte Pakete, passende Modell- und
  Reasoningstufe, Wiederverwendung vorhandener Evidence und genau ein Produkt
  pro sinnvollem geprueften Batch. Kosten werden durch weniger vermeidbare
  Arbeit gespart, niemals durch Weglassen notwendiger Semantik-, Progress-
  oder Performancepruefung. Kalt-, Warm- und inkrementelle Werte werden
  getrennt berichtet; ein Umbau darf bekannten Spiel- oder Savefortschritt
  niemals zuruecksetzen. Performancegewinne werden gemessen, nicht aus
  Hilfsmetriken abgeleitet oder erfunden.
- Jeder Analysezyklus darf genau eine kleine, isoliert reviewbare und im
  ausgefuehrten Produktpfad wirksame Runtime-Performanceoptimierung im ohnehin
  bearbeiteten Knowledge-Gap-Pfad mitnehmen. Sie ist kein eigenstaendiger
  Arbeitsblock, wird mit dem normalen Fixpaket gebaut und mit der jeweils
  aktiven Replay-/Debugmatrix nur akzeptiert, wenn kein Fall regressiert. Sie erzeugt weder
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

- Ohne weitergehende Freigabe laeuft kein gestarteter Prozess und keine
  einzelne Phase laenger als 20 Minuten. Die aktuelle stehende Nutzerfreigabe
  hebt diese Grenze fuer alle notwendigen begrenzten Sonic-Analyse- und
  Gesamtexportlaeufe auf; dafuer wird keine erneute laufbezogene Ausnahme
  erfragt. Fuer sachfremde lange Prozesse bleibt eine ausdrueckliche
  Einzelgenehmigung erforderlich.
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
  dirty konfigurieren und gezielt bauen -> verifizierten Dirty-Diff an
  Eggman uebergeben -> nach funktionierendem Sonic-Produkt ohne
  Progressrueckschritt produktgegateter Commit durch Eggman; Push nur nach
  aktueller ausdruecklicher Nutzerfreigabe**.
