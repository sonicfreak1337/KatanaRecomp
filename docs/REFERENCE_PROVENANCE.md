# Referenz- und Implementierungsprovenienz

Stand: 2026-07-16, KR-3709. Dieser Bericht trennt Spezifikation,
beobachtbares Verhalten, Architekturvergleich und uebernommenen Drittcode.

## Spezifikationen

Die Instruktions-, Ausnahme-, MMU-, Cache-, Timer-, Interrupt- und
DMA-Semantik wurde unabhaengig anhand folgender Herstellerdokumente
implementiert:

- Renesas, *SH-4 Software Manual*, Dokument REJ09B0318-0400, Rev. 4.00,
  15.06.2006:
  <https://www.renesas.com/en/document/mas/sh-4-software-manual>
- Renesas, *SH7750, SH7750S, SH7750R Group User's Manual: Hardware*,
  Dokument R01UH0456EJ0702, Rev. 7.02, 24.09.2013:
  <https://www.renesas.com/en/document/mah/sh7750-sh7750s-sh7750r-group-users-manual-hardware>

Die Handbuecher werden als Verhaltensspezifikation verwendet. Text, Tabellen,
Abbildungen und Beispielcode daraus werden nicht in KatanaRecomp
weiterverbreitet.

## Beobachtbares Verhalten

Die Tests verwenden selbst erstellte, minimale SH-4-Opcodes, ELF-/Raw-Images,
GDI-/ISO-Strukturen, Speicher- und MMIO-Ereignisse. Erwartete Werte wurden aus
den oben genannten Semantikregeln abgeleitet und anschliessend als unabhaengige
Assertions formuliert. Firmware-, Disc- oder Spielbytes sind keine Testorakel.

Die lokale Sonic-Adventure-GDI und lokal vorhandene BIOS-/Flashdateien sind
ausdruecklich nicht Teil dieses Gates. Die vom Nutzer genannte RetroBIOS-Seite
<https://github.com/Abdess/retrobios/tree/main/bios/Sega/Dreamcast> wurde in
KR-3709 weder heruntergeladen noch als Implementierungs- oder Testquelle
verwendet.

## Referenzvergleich: Flycast

- Upstream: <https://github.com/flyinghead/flycast>
- verifizierter Snapshot: `f09d1f22ef8d199b8b7a2395d0b46774e08a58c2`
  vom 10.07.2026
- ausgewiesene Lizenz: GPL-2.0
- betrachteter Umfang: Trennung virtueller/physischer Blockadressen,
  Blockendklassen, zustandsabhaengige Blockvarianten, seitenweise
  Codeinvalidierung und gastzyklusbasierte Ablaufplanung

Flycast diente nur als Architektur-Plausibilitaetsvergleich. Keine Datei,
Funktion, Tabelle oder Konstante wurde kopiert, uebersetzt oder gelinkt. Eine
direkte Flycast-Einbindung ist deaktiviert und darf erst nach einer expliziten
Projektlizenzentscheidung erfolgen, welche die GPL-2.0-Pflichten fuer das
Gesamtwerk bewertet.

## Referenzvergleich: dcrecomp

Der Name `dcrecomp` wurde im internen Planungscommit `7fdcdef` fuer die
allgemeinen Muster aufgeteilte AOT-Ausgabe und zentrale Adresstabelle genannt.
Eine autoritative Upstream-URL, ein reproduzierbarer externer Commit und eine
kompatible Lizenz wurden dabei nicht festgehalten und konnten in KR-3709 nicht
verifiziert werden. Deshalb ist dcrecomp **keine zulaessige Codequelle**.

Hart kodierte Forced Entries, titelbezogene Remaps, stille BIOS-No-ops und
Wall-Clock-Timing wurden ausdruecklich nicht uebernommen. Ohne nachgewiesene
Upstream-Provenienz und kompatible Freigabe darf weder Code noch Datenmaterial
aus einem als dcrecomp bezeichneten Projekt in KatanaRecomp gelangen.

## Synthetische Testvektoren

Alle versionierten Fixtures stammen aus KatanaRecomp selbst:

- `tests/fixtures/*.bin`: kleine, aus kommentierten SH-4-Opcodes erzeugte
  Ausfuehrungsprogramme
- Loader-/GDI-/ISO-Tests: zur Laufzeit erzeugte Header, Sektoren und
  Markerdaten ohne kommerzielle Inhalte
- FPU- und Decodervektoren: manuell aus den Renesas-Semantikregeln abgeleitete
  Eingabe-/Erwartungspaare
- Replay-, Diagnose-, Fuzz- und Differentialdaten: feste synthetische Seeds
  und frei gewaehlte Marker

`tools/generate_v014_fixtures.py` dokumentiert die Erzeugung der historischen
Opcodefixtures. Neue Fuzzer schreiben keine Eingaben ungefragt in das
Repository.

## Uebernommener Drittcode und Abhaengigkeiten

In `include/`, `src/`, `tests/` und `tools/` ist kein Flycast-, dcrecomp-,
RetroBIOS- oder anderer Emulatorquellcode eingebettet. KatanaRecomp linkt nur
gegen die C++-Standardbibliothek und Betriebssystembibliotheken der gewaehlten
Toolchain. CMake, Ninja, MSVC/GCC/Clang, clang-format, clang-tidy,
Microsoft.CodeCoverage.Console und gcovr sind reine Build-/Pruefwerkzeuge und
werden nicht in Katana-Artefakten weiterverteilt.

Das Repository besitzt derzeit keine Projektlizenzdatei. Damit wird keine
pauschale Weiterverbreitungserlaubnis behauptet. Die bewusste
Projektlizenzentscheidung bleibt KR-4003 vorbehalten und ist vor einem
externen Release zwingend; das lokale `0.37.0-dev`-Artefakt aus KR-3709 ist nur
ein reproduzierbarer Gate-Nachweis.
