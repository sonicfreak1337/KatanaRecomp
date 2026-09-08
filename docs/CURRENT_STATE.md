# Aktueller Projektstand

Stand: 8. September 2026, r322 gebaut; autonome Performancephase und Levelmatrix laufen. Historische Runs und
Zwischenstaende stehen in Git, `STATUS.md`, `TASKS.md` und `ROADMAP.md`.
Private Produkt- und Laufmanifeste binden die genauen Artefaktidentitaeten.

## Produktziel und Grenzen

Katana erzeugt statisch rekompilierte native PC-Ports. Der Produktpfad
enthaelt keinen Emulator, Interpreter, JIT, Runtime-Decoder oder geratenen
Kontrollfluss. NativeBringup bleibt nicht releasefaehig; ein funktionierender
Teil des Spiels bedeutet keine vollstaendige Closure oder Releaseabnahme.

Gleichrangige P0-Ziele sind erhaltener Story-/Gameplay-/Savefortschritt,
stabile originale 30-Hz-Simulation, unabhaengige 144-Hz-Praesentation, ein
vollstaendiger Kaltexport unter zehn Minuten und kosteneffiziente Umsetzung.
Bildwiederholungen zaehlen zur Praesentation, nicht als neue Simulationsbilder.

## Belegter Fortschritt

- Intro, Hauptmenue, Character Select und regulaerer Story-Einstieg sind
  erreichbar. Der Nutzer hat im bisherigen Storypfad Chaos 4 besiegt; danach
  wurde der naechste Film angefordert. Der fruehere Stand vor dem ersten
  Story-Intro ist damit ueberholt. Dieser Nutzerlauf bleibt an r315 gebunden.
- Das Spiel erkennt die VMU-Anbindung. Laden hat Storyfortschritt nach
  vorherigen Stops wiederhergestellt. Vollstaendige Save-/Quicksave- und
  Eventfortsetzung in allen Situationen bleibt ein eigenes Abnahmeziel.
- r316 erhaelt alle 249 gebundenen Loaded-AOT-Module und alle vorherigen
  Primary-Blockanfaenge. Drei neue private Callbackroots erschliessen weitere
  1.822 Loaded-AOT-Blockanfaenge in der Flugsequenz-Familie. Das ist
  Candidate-Fortschritt, keine automatische Proof-Promotion.
- r317 erhaelt diese Abdeckung und erschliesst durch die gesamte belegte
  Kameraindex-Familie weitere 208 Blockanfaenge in SHOOTING und 373 in
  B_E101_R. Die drei neuen Entries sind im Produkt vorhanden; ihre spaeteren
  Spielpfade sind noch nicht durch einen Lauf bestaetigt.
- r316 korrigiert die gemeinsame Objekttexturliste: Das Laden einer
  Charaktertexturliste darf die vom Main-Owner veroeffentlichte Liste nicht
  ueberschreiben. Ein begrenzter Produkttrace bestaetigt die korrigierte
  gemeinsame Bindung. Der Nutzer bestaetigt anschliessend: Die Objekte sind
  gefixt. Diese Objektabnahme ist ein r316-Nutzerwitness, kein automatischer
  Beweis fuer die gesamte Grafikpipeline.
- Alle zehn gebundenen Filmdateien sind im privaten Inhalt vorhanden und
  gegen die Originalquelle geprueft. Die acht Storyfilme wurden mit dem
  nativen Decoder vollstaendig dekodiert. Das ersetzt nicht ihren Nachweis
  innerhalb der jeweiligen Spielsequenz.
- r317 ersetzt die feste Drawanzahlgrenze durch ein Budget der tatsaechlich
  reservierten Queue-/Geometriekapazitaet. Der sichtbare Knuckles-Replay
  passiert den Film und den bisherigen Draw-Limit-Abbruch; die Szene am
  Master Emerald laeuft. Nach 145,5 Sekunden folgt ein neuer Stop beim
  naechsten Film, dessen Hook einen noch offenen Grafikframe ablehnt.
- Beide belegten SDK-Textur-Tailcalls in Primary und Casinopolis sind jetzt
  mit ihren exakten Callerbytes und aktiven Ownern gebunden. Der Trial-Replay
  uebersteht mit derselben EXE 301,6 Sekunden, erreicht aber den Abschluss
  nicht: Sonic verbleibt nach mehreren Toden unter einem Steg in Emerald
  Coast. Der eigentliche Rueckkehrfix bleibt zur Laufabnahme offen. Beide
  Runs verwenden isolierte Spielstandkopien; der exakte urspruengliche
  Vorzustand der Aufzeichnungen ist nicht belegt.
- r318 erhaelt alle 249 Loaded-AOT-Modulbindungen, alle 250.699 Primary-
  und alle bisherigen 699.496 Loaded-Blockanfaenge. Eine quellgebundene
  Parent-/Child-Aufraeumfamilie ergaenzt elf Blockanfaenge in SHOOTING;
  der im Nutzercrash fehlende Cleanup-Entry ist jetzt vorkompiliert.
  Die Familieninventur umfasst 250 Programmabbilder. Dies bleibt
  Candidate-Fortschritt ohne automatische Proof-Promotion.
- r318 schliesst beim Movieaufruf den vorhandenen nativen Grafikframe ab,
  ohne einen zusaetzlichen Simulationstick oder Storyskip. Der sichtbare
  Knuckles-Replay passiert den bisherigen Uebergabestop, spielt den
  Folgefilm vollstaendig und erreicht Station Square. Der angeforderte
  Fuenf-Minuten-Lauf endet ohne Crash; inklusive Abschlussarbeiten sind es
  308,5 Sekunden. Die gezielte Moviefortsetzung ist damit bestaetigt,
  nicht die gesamte Knuckles-Story.
- r319/r320 erhalten alle 249 Loaded-AOT-Modulbindungen. r320 enthaelt
  250.806 Primary- und 699.851 Loaded-Blockanfaenge: gegen r319 kommen 107
  beziehungsweise 261 hinzu, ohne einen bisherigen Entry oder eine
  Sourcebindung zu entfernen. Alle 99 privaten Primary-Candidates und
  6.253 Loaded-Seedrecords sind im erzeugten Produkt vorhanden. Neue
  Taskzustands-, Auxwork-, Timer- und Indexfamilien bleiben Candidates;
  die quellseitige Inventur umfasst 250 Programmabbilder.
- Der r320-Batch enthaelt Korrekturen fuer die gemeinsam ausgewerteten
  r319-Nutzercrashes von Sonic, Tails, Knuckles, Amy und Gamma. Ein
  zusaetzlicher Texturfix erkennt eine inzwischen fremd belegte, noch nicht
  geladene SDK-Registryzeile als verdraengte alte Hostbindung; Freigabe und
  Wiederladen ueberschreiben diese fremde Zeile nicht. Die betroffenen
  Laufpfade sind noch nicht abgenommen. Auf aktuelle Nutzeranweisung wurde
  r320 nur gebaut und nicht vom Agenten gestartet.
- r321 ist als Performance-NativeBringup-Produkt gebaut. Alle 249 geladenen
  Modulbindungen und alle bisherigen Primary-Dispatchentries bleiben erhalten.
  Das Produkt enthaelt 250.862 Primary- und 699.948 Loaded-Blockanfaenge sowie
  alle 6.256 geladenen Seedrecords. Die vier vorherigen fehlenden Crashentries
  und Gammas direkter Primary-Helfer sind statisch vorhanden. Bei SBOARD wird
  ein frueherer Innenroot in die vollstaendige Funktion aufgenommen; seine
  Instruktion bleibt im AOT enthalten, die Epilogaufteilung wurde an den
  Originalbytes geprueft. Das ist keine pauschale Laufabnahme.
- r322 erhaelt alle 249 Loaded-AOT-Modulbindungen, jeden bisherigen Loaded-
  Blockanfang samt Bytebereich und alle bisherigen Primary-Dispatchtuples.
  Der Pack enthaelt 1.086 Partitionen, 18.965 Funktionen und 304.710 Bloecke;
  die Dispatchtabelle waechst von 951.746 auf 953.473 Eintraege. Alle 106
  privaten Primary-Candidates und die sourcegebundenen neuen Loaded-Roots
  sind vorhanden. 71 Ownerwechsel liegen ausschliesslich in ADV00: Der
  vollstaendige Callback 29EA nimmt den frueheren Innenroot und seine
  gemeinsame Rueckkehrroutine auf. Alte Resume-PCs behalten ihren jeweiligen
  Eintrittszustand; kein alter Block und keine Gastoperation entfallen.

## Offene Produktfragen

- Der in r321 enthaltene r320-Nutzerbatch betrifft Gamma, den Sky-Chase-2-Boss, Tails,
  Knuckles und Amy. Vier Faelle betreffen AOT-Entries; Amy endet im
  Grafikadapter. Die Gamma-Ursache betrifft eine gemeinsame Primary-
  Funktion mit vier belegten Aufrufern: Die strukturelle Erkennung hat einen
  gueltigen Delayslot ausserhalb ihres 32-Instruktions-Prueffensters verworfen.
  Der Sourcefix prueft diesen gegen die tatsaechliche Image-/Segmentgrenze;
  alle bisherigen Opcode-, Owner- und Entrypruefungen bleiben erhalten.
  Die neuen Entries und ihr direkter Helfer sind jetzt kompiliert; die
  jeweilige Laufabnahme bleibt getrennt. Die sourcegebundenen
  Sky-Chase-Child- und Hub-Zustandsfamilien wurden ueber 250 Images geprueft;
  ihre vorhandenen Geschwister bleiben erhalten. Der vorher fehlende
  SBOARD-Effektentry ist ebenfalls im Produkt enthalten.
- Die SDK-Clippingsemantik erlaubt sechs Ausgabepunkte aus vier
  Eingangspunkten. Der native Puffer fuer diesen Fall waechst von fuenf auf
  sechs Punkte, mit zwoelf statt neun Listvertices. Reihenfolge und Winding
  bleiben erhalten. Der konkrete Amy-Abbruchzweig ist in der alten Capsule
  nicht belegt; neue Fehlertranskripte behalten Zweig, Counts, Flags und
  Kameratiefen. Diese Aenderung ist in r321, der urspruengliche Amy-Abbruch
  ist weiterhin nicht eindeutig diesem Zweig zugeordnet.
- Der abgeschlossene r321-Nutzerbatch enthaelt Gamma, Sonic, einen Stop vor
  Chaos 6, Tails, Knuckles und Amy. Die ersten vier Faelle betreffen weitere Funktionsentries:
  zwei ADV03-Callbacks mit drei gemeinsamen Helfern, ein Primary-Finalizer,
  beide fehlenden Arme einer Chaos-6-Childfamilie und der Folgecallback des
  SBOARD-Effekts. Amy benoetigt einen echten kurzen Task-Finalizer in STG12.
  Diese Familien sind in r322 exportiert. Bei Knuckles lag keine bewiesene
  falsche Generation vor: Ein SDK-verwalteter TEXLIST-Traeger im Heap wurde
  faelschlich wie Code innerhalb des Loaded-AOT-Images geprueft. Der Fix
  trennt beide bereits gebundenen Traegerarten und behaelt RAM-, Owner-,
  Referenzzaehler-, Epoch- und Ueberschreibpruefungen bei. Die jeweiligen
  spaeten Nutzerpfade bleiben bis zum tatsaechlichen Erreichen offen.
- Der globale Callbackscan klassifiziert alle 36 gefundenen fehlenden
  Storekandidaten in 250 Images: 29 Stores gehoeren zu 22 Callbackfamilien,
  sieben zu Daten. Einschliesslich belegter Folgecallbacks ergaenzt der
  globale Vertrag sechs Primary- und 37 Loaded-Roots. Seine 100 bestehenden
  Primary-Candidates bleiben erhalten; insgesamt sind es jetzt 106.
  Nicht bewiesene dynamische Zielmengen bleiben fail-closed. Es gibt keine
  automatische Proof-Promotion. Der Canonical-Coverage-Builder
  revalidiert vorhandene Disassembly-Daten bei unveraenderter CLI und
  unveraenderten Images. Der gemessene Erweiterungslauf dauert 7,6 Sekunden
  und startet keinen neuen Analyzer- oder Compilerprozess.
- Der Nutzer hat den Batch geschlossen und anschliessend autonome Builds,
  FPS-Messungen und gesteuerte 60-Sekunden-Leveltests freigegeben. Die aktuelle
  32er-Matrix verwendet r322, Profil 1, 144-Hz-Anforderung und isolierte Saves.
  Danach werden nur betroffene Performance-/Regressionspfade wiederholt.
  Ein spaeterer langsamer Abschnitt in Amys Hot Shelter ist durch den
  anfangs stabilen Debugabschnitt nicht abgenommen.
- Sky Chase, Chao Garden und weitere noch nicht erreichte Storyfortsetzungen
  sind nicht pauschal freigegeben. Ein neuer unbekannter Block endet weiterhin
  fail-closed; ein fehlender Crashrecord ist kein Beweis fuer Fehlerfreiheit.
- Der neue Sky-Chase-Cleanup ist im r318-Produkt gebunden, aber noch nicht
  im betroffenen Spielpfad abgenommen. Der Nutzerreplay weicht mit der
  vorhandenen Savekopie nach Mystic Ruins ab und wird gezielt beendet.
  Dieser Lauf gilt weder als Sky-Chase-Crashabnahme noch als HUD-Diagnose.
  Der mechanische Selektorkatalog enthaelt Sky Chase; der aktuelle
  direkte Launcher bietet jedoch nur Action Stages und einen nicht
  startfaehigen Eventvorschau-Eintrag. Ein gepruefter Sky-Chase-Einstieg
  fehlt. HUD und Fadenkreuz bleiben zur Diagnose offen; die begrenzte,
  standardmaessig ausgeschaltete Spritebeobachtung unterscheidet jetzt
  korrekt zwischen gebundenen und ungebundenen Texturen.
- Untertitel und Rueckblicktexte bleiben zur Nutzerabnahme offen. Die
  Diagnose zeigt wechselnde Pixelpuffer bei wiederverwendeter nativer Ansicht
  und wachsendem SDK-Referenzzaehler. r320 bindet die drei belegten dynamischen
  Textfreigabe-Caller an die vorhandene kompilierte SDK-Freigabe. Erst nach
  deren tatsaechlicher Registryfreigabe werden alte Hostansichten verworfen;
  der Folgetext kann neu hochgeladen werden. Die Diagnose war kein visueller
  Nachweis einer fortschreitenden Cutscene, und r320 hat noch keinen Laufpass.
- Der gemeinsame Objektcluster ist in r316 durch den Nutzer bestaetigt;
  Menuegrafik und andere nicht erneut beurteilte Grafikpfade bleiben offen.
  Bestehende Shader-, Tiefen-,
  Alpha- und Reihenfolgevertraege werden nicht durch pauschale Overrides
  ersetzt. Die lokale Flycast-Referenz und der originale Datenfluss bleiben
  Vergleichsbasis.
- Stabile 30 Simulationsbilder pro Sekunde sind noch nicht durchgehend
  erreicht. Der r317-Trial-Replay liefert ueber seine gesamte Laufzeit rund
  18,4 Simulations- und 142,4 Praesentationsbilder pro Sekunde, einschliesslich
  Start, Menues, Laden und Gameplay. Das ist keine reine Levelmessung und
  kein kompatibler Vergleich zur vorigen Casinopolis-Diagnose.
- r317 benoetigt 594,9 Sekunden im CLI-Export, zusaetzlich zur Wrapper-
  Vorpruefung. Der Gesamtlauf bleibt ueber zehn Minuten. Das ist ein
  inkrementeller Lauf mit Caches und zehn AOT-Compiles, kein Kaltnachweis.
  Countabhaengige Sharddeklarationen stehen jetzt nur im Aggregator; kuenftige
  Ergaenzungen dieser Liste invalidieren damit nicht mehr den gemeinsamen
  Header. Die Umstellung selbst baut dessen bisherige Verbraucher einmal
  neu. Eine gemessene Zeitersparnis durch diese Entkopplung steht noch aus.
- r318 benoetigt 485,6 Sekunden im CLI und 564,3 Sekunden fuer den gesamten
  Export-Wrapper einschliesslich Vorpruefung und Abschlussarbeiten. Die
  vorherige gezielte Komponentenverifikation ist darin nicht enthalten.
  Das ist ein inkrementeller Cachelauf unter zehn Minuten, kein Kaltexport.
  1.083 von 1.084 generierten AOT-Einheiten kommen aus dem Codegen-Cache;
  tatsaechlich werden eine AOT-Einheit, 58 Dispatch-Shards, ein Loaded-Shard
  und drei weitere C++-Dateien kompiliert. Der Hostbuild dauert 107,2 Sekunden.
  Der gemeinsame Dispatchheader bleibt unveraendert. Ein weiterer
  Bauzeitverlust ist konkret belegt: Elf neue Eintraege verschieben die
  festen 8.192-Zeilen-Grenzen von 58 Dispatchdateien. r319 implementiert eine
  Aufteilung nach stabilen Adresspraefixen mit hoechstens 8.192 Eintraegen
  pro Blatt; sein einmaliger Wechsel baut 188 Dispatchdateien. Der gesamte
  r319-Wrapper dauert 644,2 Sekunden und verfehlt das Zehn-Minuten-Ziel.
- Der erfolgreiche r320-Export dauert 513,7 Sekunden einschliesslich
  Vorpruefung und Packaging; davon entfallen 457,1 Sekunden auf den CLI-Lauf
  und 64,0 Sekunden auf den Hostbuild. Der Ninja-Plan umfasst neun
  C++-Compiles: drei AOT-Einheiten, zwei Dispatch-Shards, einen Loaded-Shard
  und drei weitere Dateien. 1.081 von 1.084 AOT-Einheiten kommen aus dem
  Codegen-Cache. Dieser inkrementelle Lauf verwendet auch Artefakte eines
  zuvor gestoppten Versuchs; dessen Zeit sowie gescheiterte Vorpruefungen
  sind in den 513,7 Sekunden nicht enthalten. Das ist kein Kaltnachweis.
- Der erfolgreiche r321-Wrapper dauert 613,5 Sekunden, davon 535,7 Sekunden
  im CLI. Der Hostbuild umfasst 59,4 Sekunden; Kompilieren und Linken davon
  56,7 Sekunden. Zehn von 1.084 AOT-Einheiten und insgesamt 21 C++-Dateien
  werden kompiliert. Die fruehere fehlgeschlagene Vorpruefung und der
  CLI-Neubau sind nicht enthalten. Der inkrementelle Export verfehlt das
  Zehn-Minuten-Ziel; ein Kaltnachweis fehlt. Der groesste Zeitanteil bleibt
  Analyse und Validierung. Kleine belegte Callbackfamilien sollen die
  vorhandene CLI und bytegebundene Erkenntnisse weiterverwenden; allgemeine
  Analyzeraenderungen werden nur gebuendelt mit ihrem realen Bedarf gebaut.
- Acht unveraenderte alte Candidate-Familien mit 534 Roots haben jetzt
  einen gebundenen Quellcheckpoint. Er bewahrt die Originalaudits und
  verifiziert die lebenden Quellen, ohne geloeschte alte Portverzeichnisse
  vorauszusetzen. Historische Missingness bleibt historisch; die aktuelle
  CLI prueft ihre eigenen Image-, Source-, Block- und Ownerbindungen weiter.

## Entwicklungsablauf

Verbindlich sind `AGENTS.md`, `NATIVE_BRINGUP_WORKFLOW.md` und
`NATIVE_PORT_PRODUCT_CONTRACT.md`, unter Vorrang aktueller Nutzeranweisungen.

1. Aktuelle Witnesses und typisierte Stops nach gemeinsamer Ursache
   zusammenfassen. Das gesamte gebundene Spiel nach derselben Familie pruefen;
   aus einer Crashadresse allein entsteht kein Ausfuehrungsrecht.
2. Generische Regeln bei einem belegten adressunabhaengigen Muster verwenden.
   Titelbezogene Familien bleiben privat, source-/byte-/imagegebunden und
   werden in ihrer aktiven Generation validiert. Dynamische Restmengen bleiben
   offen. Vorhandene Evidence wird wiederverwendet.
3. Nur konkrete disjunkte Arbeit delegieren. Keine automatische Fleet-
   Auslastung, doppelten Vollreviews oder Pflicht zum Warten auf unbenutzte
   Tasks. Der Haupttask finalisiert Approach, Implementierung und Abnahme.
4. Kompatible Fixes vor einem Performance-Produktbuild buendeln. Reine native
   Host-/Adapterfehler verwenden den gueltigen AOT-Pack. Neue Roots oder
   geaenderte AOT-Semantik erfordern die grosse Schleife. Der echte Ninja-Plan
   begrenzt die neu kompilierten AOT-Dateien vor Compilerstart.
5. Mit derselben EXE den betroffenen Sonic-Pfad gezielt pruefen: Debuglaeufe
   hoechstens 60 Sekunden, Storylauf hoechstens fuenf Minuten. Vollmatrizen
   sind kein Standardgurt; Grafik-/Engineaenderungen koennen begruendete
   breitere Abdeckung erfordern. Historische Passes behalten ihre Build-ID.
6. Nach funktionierendem Produkt ohne bekannten Fortschrittsrueckschritt den
   geprueften Source-Batch lokal committen. Kein Push. Retaildaten,
   Spielstaende, private Titelvertraege und Buildartefakte bleiben ausserhalb
   des oeffentlichen Repositorys.

## Implementierter Unterbau und Beweisgrenzen

Der Bring-up-Dispatch validiert aktive vorkompilierte Blockanfaenge gegen
versiegelte Identitaeten, aktuelle Owner und Generationen, einschliesslich
Loaded AOT. Native Audio-, Grafik-, Datei-, Eingabe-, Movie- und Saveprovider
ersetzen belegte Plattformvertraege. Crash Capsules korrelieren unter anderem
Providerfehler, Kontrollfluss und Loaded-AOT-Identitaet; Rohadressen oder
Laufzeitbeobachtungen schliessen keine statische Frontier.

`Observed -> Candidate -> Proven | RuntimeContract -> Strict Product` bleibt
die Evidence-Grenze. Eingabereplays sind nur mit gebundenem Vorzustand und
tatsaechlich beobachtetem Meilenstein ein Reproduktionsnachweis. Vollstaendiges
deterministisches Provider-Replay, automatische First-Divergence-Lokalisierung
und universelle Spiel-/Grafikabdeckung sind nicht als fertig abgenommen.
