# Sonic Adventure als private Retail-Testbench

Dieses Dokument begrenzt die lokale Sonic-Adventure-Nutzung innerhalb der
KatanaRecomp-Entwicklung. KatanaRecomp ist ein allgemeines Dreamcast-
Recompiler-Framework, eine Dreamcast-Runtime und ein Runtime-SDK. Es ist weder
ein Sonic-Adventure-Port noch dessen Installer oder Enhancement-Projekt.

## Grundregel

- Sonic Adventure darf privat und read-only als End-to-End-Testbench fuer
  Analyse, Codegen, Hostbuild, Runtime und Dreamcast-Plattformdienste dienen.
- Der erste Prozessstart erfolgt erst in einem ausdruecklich freigegebenen,
  budgetierten `runtime-probe`-Lauf.
- Private Sonic-Ergebnisse koennen ein internes Alpha-Akzeptanzziel stuetzen,
  sind aber kein oeffentlicher Katana-Produktvertrag und keine verteilbare
  Gateevidenz.
- Ein spaeterer Sonic-Port, Installer und alle titelbezogenen Erweiterungen
  gehoeren in ein eigenstaendiges Repository.

## Datenschutz und Implementierungsgrenze

- Verwendet wird nur eine rechtmaessig lokal bereitgestellte GDI.
- GDI, Tracks, extrahierte Dateien, generierte Retail-Quellen, Programme,
  Rohlogs, Screenshots, Audio, Hashes und lokale Pfade bleiben ausserhalb des
  Repositorys und aller Pakete.
- Oeffentliche CI verwendet nur synthetische und frei lizenzierte Eingaben.
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

Jeder private Lauf besitzt Hostzeit- und Gastzyklusbudgets. Fuer jeden Blocker:

1. letzten stabilen generischen Checkpoint und Fehlerklasse redigiert erfassen
2. allgemeine Ursache ohne Titelsonderfall bestimmen
3. synthetische oder frei lizenzierte Reproduktion erstellen
4. allgemeine Implementierung korrigieren
5. Regression ins Repository aufnehmen
6. privaten Lauf wiederholen

Der private Lauf ist eine Testbench fuer Frameworkfehler, kein oeffentlicher
Testbestand. Ein erfolgreicher Sonic-Lauf darf nicht durch Sonic-spezifische
Implementierungslogik erkauft werden.

## Framework-Alpha

Das Katana-Alpha-Gate bewertet versionierte Frameworkvertraege, reproduzierbare
Builds, synthetische oder frei lizenzierte Regressionen, redigierte Diagnosen
und die Abwesenheit proprietaerer Daten. Private Sonic-Probes duerfen
zusaetzliche interne Evidenz liefern, bestimmen aber weder den oeffentlichen
Releasevertrag noch das Layout eines spaeteren Sonic-Portprodukts.
