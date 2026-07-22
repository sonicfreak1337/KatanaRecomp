# Referenz- und Implementierungsprovenienz

Stand: 2026-07-22, v0.48-Bootaudit. Dieser Bericht trennt Spezifikation,
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

Fuer den sichtbaren GD-ROM-Taskfile-/SPI-Vertrag wird ausserdem das
Herstellerdokument *GD-ROM Protocol SPI (Sega Packet Interface) Specifications
Ver.1.30* vom 12. Januar 1999, verwendet (oeffentlicher
Dokumentmirror:
<https://segaretro.org/images/7/72/Cdif131e.pdf>). Daraus werden
REQ_MODE/SET_MODE-Grenzen, `Features.Bit0` als DMA-Wahl sowie der getrennte
DMARQ/DMACK- und Abschluss-INTRQ-Ablauf abgeleitet. Dokumenttext, Tabellen und
Identitaetsstrings werden nicht uebernommen; nicht benoetigte Hardware-
Identitaetsfelder bleiben explizit unavailable.

Der echte freie Dreamcast-Hardwarepfad im Linux-Kernel dient als unabhaengiger
Plausibilitaetscheck fuer CD_READ-DMA und die belegte ATA-SET-FEATURES-
Kombination:
<https://github.com/torvalds/linux/blob/master/drivers/cdrom/gdrom.c>.
KatanaRecomp uebernimmt weder Treibercode noch dessen Hoststruktur.

Fuer den PVR-Y-Scaler und die feldweise Framebufferausgabe ist Sega,
*Dreamcast/Dev.Box System Architecture*, Stand 3. September 1999, Abschnitt
3.4.12.2, die Primaerquelle:
<https://segaretro.org/images/7/78/DreamcastDevBoxSystemArchitecture.pdf>.
Der sichtbare Katana-Vertrag leitet daraus `SCALER_CTL`-Interlace und
`Field Select` sowie die zugehoerige feldweise SOF-Ausgabe ab. KallistiOS-
Header dienen hier nur als Plausibilitaetscheck und decken die betreffenden
Bits nicht vollstaendig ab; Dokumenttext, Abbildungen, Tabellen oder
Implementierungscode werden nicht uebernommen.

Fuer die oeffentlich sichtbare Dreamcast-BIOS- und GD-ROM-ABI ist KallistiOS
die Primaerquelle:

- BIOS-Syscallimplementierung:
  <https://github.com/KallistiOS/KallistiOS/blob/master/kernel/arch/dreamcast/hardware/syscalls.c>
- dokumentierter GD-ROM-Syscallvertrag:
  <https://kos-docs.dreamcast.wiki/group__gdrom__syscalls.html>
- dokumentierte GD-ROM-Streaming-API:
  <https://kos-docs.dreamcast.wiki/group__gdrom.html>

KatanaRecomp leitet daraus Rueckgabeklassen, Vierwortstatus, Request- und
Streamingselektoren sowie nicht zurueckkehrende Lifecycle-Grenzen ab. Es wird
weder KallistiOS-Code eingebettet noch dessen Implementierungsstruktur
uebernommen.

## Beobachtbares Verhalten

Die Tests verwenden selbst erstellte, minimale SH-4-Opcodes, ELF-/Raw-Images,
GDI-/ISO-Strukturen, Speicher- und MMIO-Ereignisse. Erwartete Werte wurden aus
den oben genannten Semantikregeln abgeleitet und anschliessend als unabhaengige
Assertions formuliert. Firmware-, Disc- oder Spielbytes sind keine Testorakel.

Die lokalen Retail-GDIs bleiben ausschliesslich private, read-only
Produktpfadtests. Die lokal zerlegte BIOS-Kopie wurde im v0.48-Audit nur
statisch als semantischer Crosscheck fuer GD-ROM-Selektoren und
Zustandsgrenzen betrachtet. Keine Firmwarebytes, Disassemblyausschnitte,
Hashes oder titelbezogenen Werte werden uebernommen oder versioniert; alle
Repositorytests formulieren den unabhaengig mit KallistiOS abgeglichenen
Vertrag aus synthetischen Daten neu. Die vom Nutzer genannte RetroBIOS-Seite
<https://github.com/Abdess/retrobios/tree/main/bios/Sega/Dreamcast> wurde in
KR-3709 weder heruntergeladen noch als Implementierungs- oder Testquelle
verwendet.

## Architekturvorbild: XenonRecomp

- Upstream: <https://github.com/hedge-dev/XenonRecomp>
- bei der Architekturpruefung betrachteter Main-Snapshot:
  `ddd128bcca99fe8bfbb99bea583c972351fa6ace`
- ausgewiesene Lizenz: MIT
- betrachteter Umfang: vorausanalysierte Executable-zu-C++-Uebersetzung,
  anschliessender Hostcompiler, getrennte Plattformruntime und native
  Adress-/Funktionsbindung

XenonRecomp dient ausschliesslich als Architekturvorbild fuer die
Werkzeugklasse. KatanaRecomp uebernimmt weder Code noch Xbox-spezifische
Annahmen, Tabellen, Adressen oder Titelwissen. Der Dreamcast-Vertrag wird
unabhaengig aus SH-4- und Plattformdokumentation sowie synthetischen Tests
implementiert.

## Referenzvergleich: Flycast

- Upstream: <https://github.com/flyinghead/flycast>
- lokal gepruefter Snapshot: `4126f1464fbc77c6bcec9cad00c32017ecabb799`
- ausgewiesene Lizenz: GPL-2.0
- betrachteter Umfang: Bootreihenfolge, sichtbare BIOS-/GD-ROM-Zustaende,
  G1-Registerrollen, Interrupt- und Completiongrenzen, Trennung
  virtueller/physischer Blockadressen, zustandsabhaengige Blockvarianten und
  gastzyklusbasierte Ablaufplanung

Flycast diente nur als Architektur-Plausibilitaetsvergleich. Keine Datei,
Funktion, Tabelle oder Konstante wurde kopiert, uebersetzt oder gelinkt. Eine
direkte Flycast-Einbindung ist deaktiviert und darf erst nach einer expliziten
Projektlizenzentscheidung erfolgen, welche die GPL-2.0-Pflichten fuer das
Gesamtwerk bewertet.

Der v0.48-Bootaudit behandelt Reios-Verhalten ausdruecklich nicht als
normative BIOS-Semantik. Insbesondere ist ein Reios-Neustart bei `SYSTEM 1`
eine Emulatorpolicy fuer ein fehlendes BIOS-Menue und kein Beleg fuer einen
Disc-Rebootvertrag. KatanaRecomp folgt hier der oeffentlichen ABI und erzeugt
den nicht zurueckkehrenden Lifecycle-Ausgang `BiosMenu`. Der lokale Vergleich
lieferte ausserdem einen Hinweis auf den direkten GD2-ABI-Einstieg bei
`0x8C0010F0`; dessen Katana-Vertrag wurde eigenstaendig und synthetisch
getestet. Weder Reios-Code noch Flycasts Emulationsarchitektur werden
uebernommen.

Der Vergleich des Dreamcast-BSC-Pfads zeigte ausserdem, dass der BIOS-Handoff
bei Composite die oberen Kabelbits als Eingang behandelt und alternative
Pinmodi nicht als GPIO-Ausgangslatch liest. Implementiert wurde daraus
unabhaengig die allgemeine SH-4-Regel, dass ausschliesslich 2-Bit-Modus 1 ein
normaler Ausgang ist; Flycasts geraetespezifische Lesefunktion wurde nicht
uebernommen.

## Referenzvergleich: dcrecomp

Der Name `dcrecomp` wurde im internen Planungscommit `7fdcdef` fuer die
allgemeinen Muster aufgeteilte AOT-Ausgabe und zentrale Adresstabelle genannt.
Fuer die vorhandene lokale Kopie lassen sich weder ein reproduzierbarer
externer Commit noch eine geschlossene, kompatible Lizenzprovenienz
verifizieren. Die Kopie enthaelt zudem als Flycast ausgewiesene, GPL-2.0-
lizensierte Bestandteile. Deshalb ist dcrecomp **keine zulaessige Codequelle**
und darf ausschliesslich als Warn- und Architekturvergleich gelesen werden.

Hart kodierte Forced Entries, titelbezogene Remaps, stille BIOS-No-ops und
Wall-Clock-Timing wurden ausdruecklich nicht uebernommen. Ohne nachgewiesene
Upstream-Provenienz und kompatible Freigabe darf weder Code noch Datenmaterial
aus einem als dcrecomp bezeichneten Projekt in KatanaRecomp gelangen; das gilt
insbesondere fuer die enthaltenen Flycast-Teile.

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
Projektlizenzentscheidung bleibt KR-4902 vorbehalten und ist vor dem ersten
oeffentlichen Alpha-Release zwingend; das lokale `0.37.0-dev`-Artefakt aus
KR-3709 ist nur ein reproduzierbarer Gate-Nachweis.
