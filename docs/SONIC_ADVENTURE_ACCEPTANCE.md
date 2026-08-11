# Sonic Adventure als private Produkt- und Integrationstestbench

Dieses Dokument begrenzt die lokale Sonic-Adventure-Nutzung innerhalb der
KatanaRecomp-Entwicklung. KatanaRecomp ist ein allgemeines statisches SH-4-
Recompiler-Framework und Native-Port-SDK. Es ist kein Dreamcast-Emulator und
enthaelt weder einen Sonic-Adventure-Port noch dessen Installer oder
Enhancement-Projekt.

Die repositoryweiten Arbeitsregeln in `../AGENTS.md` sind verbindlich. Sonic
ist der massgebliche Produkt- und Integrationstest; neue synthetische Tests,
Fixtures, Regressionen oder Testmatrizen werden aus Retailbefunden nicht mehr
abgeleitet.

## Verbindliches v0.50.0-Alpha-Gate

`v0.49.1` bleibt Pre-Alpha, bis der lokal erzeugte Sonic-PAL-Port ueber einen
rein nativen PC-Pfad das Hauptmenue erreicht. Genau dieser Nachweis gibt
`v0.50.0 Alpha` frei.

Das Gate verlangt gleichzeitig:

- statisch rekompilierten SH-4-Spielcode ohne Interpreter, JIT oder
  Runtime-Dekodierung;
- Bild ueber eine native PC-GPU-API und Ton/Movie ueber native Hostdienste;
- native Datei-, Eingabe- und Savepfade;
- Opening ohne Skip, Ersatzframe oder erzwungenen Playerstatus;
- 60-Hz-PAL-Pfad, Memory-Card-Screen und Hauptmenue;
- kein gelinkter ARM7-Interpreter, CPU-PVR-Softwarerasterizer oder
  vollstaendiger Dreamcast-Geraeteverbund;
- keinen Laufzeitfallback auf historische Geraetemodelle.

Ein durch die historische RuntimeOnly-Geraeteausfuehrung erreichtes Bild oder
Menue erfuellt dieses Gate nicht. Dreamcast-MHz sind kein Alpha-Gate; bewertet
werden reale Ladezeit, Framezeit, Audio-Stabilitaet, Hostauslastung und
Eingabelatenz. Der vollstaendige Vertrag steht in
`NATIVE_PORT_PRODUCT_CONTRACT.md`.

## Grundregel

- Sonic Adventure dient privat und read-only als autoritativer End-to-End-
  Produkt- und Integrationstest fuer Analyse, Codegen, Hostbuild, native
  Runtime und native Plattformdienste.
- Der projektweite Taskablauf bleibt:

  ```text
  Task implementieren
    -> alle betroffenen Pfade reviewen und bestaetigte Fehler schliessen
    -> den reviewten Task direkt auf main committen und pushen
  ```

- Gefixt wird anhand der Quellpfadreviews. Getestet wird an den geplanten
  Produktgates mit dem real erzeugten Sonic-Port und seinem normalen Lauf.
- Ein einzelner Task benoetigt keinen eigenen Sonic-Lauf. Zusammenhaengende,
  reviewte Tasks duerfen vor dem naechsten ausdruecklich vorgesehenen
  Produktgate auf `main` landen.
- Private Sonic-Ergebnisse koennen ein internes Alpha-Akzeptanzziel stuetzen,
  sind aber kein oeffentlicher Katana-Produktvertrag und keine verteilbare
  Gateevidenz.
- Ein spaeterer Sonic-Port, Installer und alle titelbezogenen Erweiterungen
  gehoeren in ein eigenstaendiges Repository.

## Keine neue Testinfrastruktur aus Retailbefunden

- Ein Sonic-Befund wird in eine allgemeine Fehlerklasse uebersetzt und in den
  betroffenen generischen Pfaden behoben.
- Daraus werden keine neuen Unit-Tests, Regressionstests, synthetischen
  Reproduktionen, Fixtures, Stresslaeufe, Testprojekte oder Testmatrizen
  erzeugt.
- Reviews duerfen fehlende neue Tests nicht beanstanden und keine neue
  Testabdeckung als Abschlussbedingung verlangen.
- Vorhandene Tests duerfen auf gebrochene Erwartungen, falsche Zahlen oder
  widerspruechliche Semantik geprueft und bei Bedarf repariert werden. Ihr
  Bestand wird fuer einen neuen Retailbefund jedoch nicht erweitert.
- Der anschliessende reale Sonic-Lauf an einem geplanten Produktgate bleibt
  der einzige neue Integrations- und Produktnachweis.

## Datenschutz und Implementierungsgrenze

- Verwendet wird nur eine rechtmaessig lokal bereitgestellte GDI.
- GDI, Tracks, extrahierte Dateien, generierte Retail-Quellen, Programme,
  Rohlogs, Screenshots, Audio, Hashes und lokale Pfade bleiben ausserhalb des
  Repositorys und aller Pakete.
- Oeffentliche CI darf proprietaere Eingaben nie voraussetzen. Bestehende
  generische Checks koennen weiterlaufen, sind aber kein Ersatz fuer Review
  oder Sonic-Produktnachweis und werden nicht um neue Retailregressionen
  erweitert.
- Fest codierte Sonic-Adressen, Symbole, Dateinamen, Bytes, Profile, Remaps,
  Shader, Assets, Patches und titelbezogene Runtimeausnahmen sind unzulaessig.
- Sonic-spezifische Auswahl-, Installer- und Enhancementlogik ist kein Teil von
  KatanaRecomp.

## Build-only-Nachweis

Erlaubt:

```text
GDI
-> Analyse
-> Codegen
-> externes generisches Portprojekt
-> Hostprogramm gebaut
```

Im `build-only`-Modus ist ein Prozessstart technisch verboten. Der private
Bericht muss `game_executable_started == false` ausweisen und darf hoechstens
den generischen Checkpoint `KR_RETAIL_ANALYSIS_CONTINUES` melden.

## Generische Runtime-Ereignisse

Katana darf nur titelunabhaengige, versionierte Ereignisse ausgeben:

| Ereignis | Bedeutung |
|---|---|
| `KR_RETAIL_ANALYSIS_CONTINUES` | Analyse entdeckt nach einem indirekten Bootpfad weiteren Code. |
| `KR_GUEST_PROGRAM_ENTERED` | Das Hostprogramm fuehrt validierten Gastcode jenseits des initialen Einstiegs aus. |
| `KR_FIRST_GUEST_FRAME` | Ein aus echtem Gast-PVR-Zustand erzeugter Frame wird praesentiert. |
| `KR_GUEST_INPUT_INTERACTIVE` | Gastzustand reagiert deterministisch auf Hosteingabe. |
| `KR_CONTROLLED_RETAIL_SCENE` | Video, Eingabe, Disc-I/O und Gastzeit machen gemeinsam kontrollierbaren Fortschritt. |

Diese Ereignisse beschreiben Frameworkverhalten. Sie enthalten keine
Sonic-Adressen, Funktionsnamen, Symbole oder titelbezogenen Kontrollflussziele.

## Retail-getriebener Debugzyklus

Jeder private Lauf besitzt Hostzeit- und Gastzyklusbudgets. Fuer jeden Blocker
gilt:

1. letzten stabilen generischen Checkpoint und Fehlerklasse redigiert erfassen
2. allgemeine Ursache ohne Titelsonderfall bestimmen
3. betroffene Analyse-, IR-, Codegen-, Runtime- und Produktpfade reviewen
4. bestaetigte Ursache im generischen Pfad korrigieren
5. den reviewten Task direkt auf `main` pushen
6. den privaten Sonic-Lauf erst am naechsten vorgesehenen Produktgate
   wiederholen

Der private Lauf ist die Produkt- und Integrationstestbench fuer
Frameworkfehler, kein oeffentlicher Testbestand. Ein erfolgreicher Sonic-Lauf
darf nicht durch Sonic-spezifische Implementierungslogik erkauft werden.

## Deterministischer A/B-Nachweis

Der historische generische KR-4842-Nachweis verwendete zwei frische
Runtimewurzeln, dieselbe native AOT-EXE, denselben lokal aus der Originaldisc
installierten Pack und dasselbe positive Gastzyklusbudget. Zwischen den
Laeufen unterschied sich nur `KATANA_PORT_DIAGNOSTICS=0` beziehungsweise `1`;
konkurrierende Rohtrace- oder Diagnosevariablen waren verboten.

Am 23.07.2026 endeten beide Laeufe bei exakt 100.000 Gastzyklen
`complete`/`budget-reached`. Systemreplay v3 war vollstaendig und versiegelt,
alle normativen Felder waren gleich, EXE und Pack unveraendert und beide
Wait-Loop-Tracezaehler null. Dokumentiert wird nur diese aggregierte Aussage,
nicht die privaten Pfade, Rohlogs, Hashes oder Gastwerte. Dieser historische
Nachweis begruendet keine neue Matrixpflicht.

## Framework-Alpha

Das Katana-Alpha-Gate ist das erste rein nativ erreichte Sonic-Hauptmenue.
Zusaetzlich bleiben versionierte Frameworkvertraege, reproduzierbare Builds,
redigierte Diagnosen und die Abwesenheit proprietaerer Daten verbindlich.
Neue synthetische oder frei lizenzierte Regressionen sind keine Voraussetzung
und werden nicht als Ersatzabnahme aufgebaut. Ein emulationsnaher
RuntimeOnly-Erfolg kann `v0.50.0 Alpha` nicht freigeben.

## Vertagter Direct-Scanout-Befund vom 27.07.2026

Ein historischer privater Produktport praesentierte den SEGA-Lizenzbildschirm
reproduzierbar. Frisch erzeugte Ports erreichen weiterhin denselben spaeten
Bootzustand, dieselbe Anzahl abgeschlossener Disc-Kommandos und dieselben
aggregierten Direct-Scanout-Zaehler, praesentieren im Clientbereich jedoch
schwarz. Es liegt daher kein belegter Verlust der Gast-Bootdistanz vor; offen
ist eine Bildinhalt- oder Scanout-Zeitregression.

Isolierte historische Produkt-A/Bs schlossen Instruktionsaccounting,
indirekten Inline-Cache, lokales AOT-Chaining und den globalen MSVC-Inliner
jeweils als alleinige damalige Ursache aus. Diese historischen Laeufe erzeugen
keine allgemeine Verpflichtung zu neuen A/B-Matrizen.

Der Wiedereinstieg muss den exakten damaligen Dirty-Tree-Quellstand
rekonstruieren und am ersten VBlank nur begrenzte, titelunabhaengige
Frame-/VRAM-/Scheduler-Hashes vergleichen. Private Pfade, Hashidentitaeten,
Gastadressen, Screenshots und Retailwerte bleiben im lokalen Handoff und
ausserhalb dieses Repositorys. Bis dahin darf der Befund weder als
Bootkorrektheitsnachweis noch als funktionaler Rueckschritt bewertet werden.

## Executable-First-Produktgate vom 27.07.2026

Der aus dem privaten Boot-Executable-Artefakt frisch erzeugte v0.49-Port wurde
mit MSVC und clang-cl real ausgefuehrt. Beide Laeufe erreichten exakt
600.000.000 Gastzyklen, meldeten weder AOT-, Runtime- noch Geraeteprobleme und
erreichten die generischen Checkpoints `KR_GUEST_PROGRAM_DISPATCHED`,
`KR_GUEST_PROGRAM_PROGRESSED` und `KR_GUEST_PROGRAM_ENTERED`.

MSVC benoetigte 14,8563 Sekunden Produktzeit (40,3869 MHz), clang-cl 14,1289
Sekunden (42,4662 MHz). Beide Varianten zaehlten 52.329.316 zentrale
Dispatches. Es wurde kein klassifizierbarer Frame praesentiert; der hoechste
sichtbare Meilenstein bleibt daher `none`. Das Ergebnis belegt einen stabilen
DirectBoot-Ausfuehrungspfad und einen historischen Compilervergleich, aber
weder Bootkorrektheit bis zum SEGA-Bild noch das 200-MHz-Ziel. Auch dieser
historische Vergleich begruendet keine neue projektweite Testmatrix.
