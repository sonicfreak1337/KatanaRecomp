# Firmware-Betriebsarten ab v0.26

## Entscheidung

Der verpflichtende und einzige ausfuehrbare v0.26-Bootpfad ist
`DirectHomebrew`. Er benoetigt weder BIOS noch Flash. Raw- und ELF-Loader liefern
ein gemeinsames `ExecutableImage`; der Plattformboot kopiert dessen Segmente und
BSS in den Dreamcast-Hauptspeicher, prueft den Einstiegspunkt und setzt den
definierten CPU-Zustand.

`HleBiosAbi` und `LleFirmware` sind benannte, aber bewusst abgewiesene Modi. Der
Fehler erfolgt vor Speicher- oder CPU-Teilwirkungen. Ein spaeterer HLE-Pfad muss
BIOS-Sprungvektoren beim Boot dynamisch im RAM erzeugen; statische ROM-Funktionen
oder kopierte Firmwarebytes sind kein zulaessiges Modell. Ein spaeterer LLE-Pfad
darf ausschliesslich lokale, vom Nutzer bereitgestellte Abbilder verwenden und
muss Groesse und Integritaet vor dem Mapping pruefen.

## Vertraege des Direkteinstiegs

- Reset: `PC` zeigt auf den geprueften Image-Einstieg, `R15` standardmaessig auf
  `0x8D000000`; VBR, SR und FPSCR stammen aus der Plattformkonfiguration. Alle
  weiteren CPU-Felder werden ueber `reset_cpu` deterministisch geloescht.
- Speicher: ladbare Segmente muessen vollstaendig in einem Hauptspeicher-Alias
  liegen. Dateibytes werden kopiert, der restliche Segmentbereich wird als BSS
  genullt. Fehlende Segmente oder Einstiegspunkte sind harte Fehler.
- Cache: Instruktions- und Datencaches werden noch nicht kohärent modelliert.
  `PREF @Rn` ist ausfuehrbar und protokolliert normale beziehungsweise
  Store-Queue-Adressen deterministisch; QACR-gesteuerter Transfer folgt in der
  spaeteren Store-Queue-Phase.
- MMIO: Der Direkteinstieg setzt keine Firmware-MMIO-Initialisierung voraus.
  Plattformgeraete muessen spaeter explizit abgebildet werden; nicht abgebildete
  Zugriffe bleiben strukturierte Busfehler.
- Syscalls: Es wird keine BIOS-ABI vorgetaeuscht. BIOS-Syscalls und dynamische
  RAM-Vektoren sind in diesem Modus nicht vorhanden und muessen sichtbar
  unaufgeloest bleiben.

Tests, Beispiele und Releases enthalten ausschliesslich synthetische Homebrew-,
Flash- und Speicherfixtures. BIOS-, Flash-, Font-, PVR- oder sonstige
proprietaere Firmwaredaten werden nicht eingecheckt oder verteilt.
