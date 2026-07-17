# Sonic-Adventure-Debug- und Alpha-Akzeptanz

Dieses Dokument trennt Pre-Alpha-Buildnachweise von der eigentlichen
Sonic-Adventure-Ausfuehrung.

## Grundregel

- Vor `v0.50.0`-Alpha-Arbeit darf Sonic Adventure privat und read-only
  analysiert sowie bis zu einer externen `game.exe` gebaut werden.
- In `v0.47.0` wird diese `game.exe` nicht gestartet.
- Der erste echte Prozessstart gehoert zur Alpha-Entwicklung.
- Der oeffentliche Alpha-Release erfolgt erst bei `SA_ALPHA_PLAYABLE`.

Eine nur erzeugte EXE, ein betretenes Hauptprogramm oder ein einzelner Frame
reichen nicht fuer den oeffentlichen Alpha-Release.

## Datenschutz

- Verwendet wird nur eine rechtmaessig lokal bereitgestellte GDI.
- GDI, Tracks, extrahierte Dateien, generierte Retail-Quellen, `game.exe`,
  Rohlogs, Screenshots, Audio, Hashes und lokale Pfade bleiben ausserhalb des
  Repositorys und aller Pakete.
- Oeffentliche CI verwendet nur synthetische und frei lizenzierte Eingaben.
- Fest codierte Sonic-Adressen, Remaps und titelbezogene Runtimeausnahmen sind
  unzulaessig.

## Pre-Alpha-Nachweis v0.47

Erlaubt:

```text
GDI
-> Analyse
-> Codegen
-> externes Portprojekt
-> game.exe gebaut
```

Nicht erlaubt:

```text
game.exe starten
-> SA_MAIN_ENTERED
```

Der private Bericht muss `game_executable_started == false` ausweisen. Ein
Checkpoint oberhalb von `SA_ANALYSIS_CONTINUES` ist in v0.47 ein Fehler.

## Alpha-Checkpoints

| Checkpoint | Bedeutung |
|---|---|
| `SA_ANALYSIS_CONTINUES` | Analyse entdeckt nach dem ersten indirekten Bootpfad weiteren Code. |
| `SA_MAIN_ENTERED` | Die private `game.exe` fuehrt Gastcode jenseits des initialen Einstiegs aus. |
| `SA_FIRST_FRAME` | Ein aus echtem Gast-PVR-Zustand erzeugter Frame wird praesentiert. |
| `SA_MENU_INTERACTIVE` | Eine Auswahl reagiert deterministisch auf Hosteingabe. |
| `SA_ALPHA_PLAYABLE` | Mindestens eine sichtbare Spielszene ist kontrollierbar und die Runtime macht weiter Fortschritt. |

`SA_MAIN_ENTERED` bis `SA_ALPHA_PLAYABLE` sind Alpha-Bring-up-Checkpoints.
Nur `SA_ALPHA_PLAYABLE` ist das oeffentliche Alpha-Gate.

## Retail-getriebener Debugzyklus

Jeder private Lauf besitzt Hostzeit- und Gastzyklusbudgets.

Fuer jeden Blocker:

1. letzten stabilen Checkpoint und Fehlerklasse redigiert erfassen
2. Ursache ohne Titelsonderfall bestimmen
3. synthetische oder frei lizenzierte Reproduktion erstellen
4. allgemeine Implementierung korrigieren
5. Regression ins Repository aufnehmen
6. privaten Lauf wiederholen

Der private Lauf ist ein Kompatibilitaets-Orakel, kein oeffentlicher Testbestand.

## Alpha-Gate v0.50.0

KR-4999 muss nachweisen:

1. Die GDI wird read-only ueber den offiziellen Workflow verarbeitet.
2. Analyse und Codegen melden keine unvollstaendige Abdeckung als Erfolg.
3. Das externe Portprojekt baut eine eigenstaendige `game.exe`.
4. Dynamische Codebereiche und Module koennen nicht still unkompiliert laufen.
5. Zwei identische Laeufe erreichen `SA_ALPHA_PLAYABLE`.
6. Video und Eingabe funktionieren gemeinsam.
7. Disc-I/O, Scheduler, DMA und Interrupts machen messbaren Fortschritt.
8. `silent_failures == 0`.
9. Fehler- und Budgetpfade liefern redigierte Diagnoseberichte.
10. Debug-/Release-Builds, Regression und CI bestehen ohne proprietaere Daten.
11. Pakete und Repository enthalten keine privaten Retail-Artefakte.

Audio darf klar dokumentierte Alpha-Abweichungen besitzen. Ein kompletter
Spieldurchlauf, perfekte Grafik-/Audiokorrektheit und mehrere Retail-Titel sind
Beta-Ziele.

## Review und Release

KR-4999 stoppt fuer das Nutzerreview. Erst nach ausdruecklicher Freigabe darf
KR-5000 Version, Release-Commit, Tag und Downloads erzeugen.
