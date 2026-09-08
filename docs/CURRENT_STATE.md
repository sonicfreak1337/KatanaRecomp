# Aktueller Projektstand

Stand: 8. September 2026, Produktcheckpoint r317. Historische Runs und
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

## Offene Produktfragen

- Sky Chase, Chao Garden und weitere noch nicht erreichte Storyfortsetzungen
  sind nicht pauschal freigegeben. Ein neuer unbekannter Block endet weiterhin
  fail-closed; ein fehlender Crashrecord ist kein Beweis fuer Fehlerfreiheit.
- Die Movieuebergabe muss noch einen vom Spiel offen gehaltenen Grafikframe
  korrekt abschliessen, ohne einen weiteren Simulationstick zu erfinden.
  Der neue Knuckles-Stop ist eine fehlende native Uebergabe, kein fehlender
  Film und keine automatische Freigabe fuer das Ueberspringen der Sequenz.
- Untertitel und Rueckblicktexte bleiben zur Abnahme offen. Die neue,
  standardmaessig ausgeschaltete Diagnose beobachtet Timer, Textqueue,
  vollstaendige Pixelpuffer und bestehende native Texturansichten. Geaenderte
  Textpixel bei wiederverwendeter Ansicht werden als Updatefrage untersucht.
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
