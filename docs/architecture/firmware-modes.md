# Alpha-Firmware- und Retail-Bootprofile

Seit KR-4601 ist die Firmwareentscheidung ein versionierter, maschinenlesbarer
Vertrag (`katana-alpha-firmware`, Version 1). Die Pruefung erfolgt vor
Eingabesnapshot, Loader, Speicherabbildung und CPU-Reset. Ein nicht verfuegbares
Profil kann daher keine Teilwirkung hinterlassen.

| Profil | Zustand | Retail | Firmwarevertrag |
| --- | --- | --- | --- |
| `direct` | `available` | ja | geprueftes Bootimage am deklarierten Einstieg; kein BIOS und kein Flash |
| `hle` | `available` | ja | dynamische BIOS-ABI-Vektoren; Hardwaredienste bleiben explizit begrenzt |
| `lle` | `unsupported` | nein | optional, nicht fuer Alpha erforderlich und nicht implementiert |

## Direkteinstieg

Der aktuell ausfuehrbare Pfad braucht weder BIOS noch Flash. Raw-, ELF- und
GDI-Loader liefern ein gemeinsames `ExecutableImage`; der Plattformboot kopiert
dessen Segmente und BSS in den Dreamcast-Hauptspeicher, prueft den Einstieg und
setzt den definierten CPU-Zustand. BIOS-Syscalls, Firmwarevektoren und
Firmware-MMIO-Initialisierung werden nicht vorgetaeuscht.

- Reset: `PC` zeigt auf den geprueften Image-Einstieg, `R15` standardmaessig auf
  `0x8D000000`; VBR, SR und FPSCR stammen aus der Plattformkonfiguration.
- Speicher: Jedes Segment muss vollstaendig in einem Hauptspeicher-Alias liegen.
  Dateibytes werden kopiert, BSS wird genullt.
- Fehler: fehlende Segmente, nicht ausfuehrbare Einstiege und Firmwareeingaben
  werden vor Speicher- oder CPU-Wirkung abgelehnt.

## HLE-BIOS-ABI

HLE ist fuer den Retail-Alpha-Pfad verfuegbar. KR-4602 installiert die sechs
allgemeinen BIOS-Sprungvektoren dynamisch im RAM und bindet sie an normale
Runtimeblocks. Bekannte, noch nicht an Plattformdienste angeschlossene Aufrufe
enden `service-unavailable`; unbekannte Aufrufe sind harte Fehler. Es werden
keine BIOS-ROM-Bytes benoetigt oder statisch in erzeugten Code kopiert. Details
stehen in [`../BIOS_ABI.md`](../BIOS_ABI.md).
Die Moduswahl wird beim Portexport aus dem Manifest in den produktiven
GDI-Runtimeaufruf uebernommen; eine HLE-Logzeile ohne installierte Ressourcen
ist kein zulaessiger Erfolg.

## Optionales LLE

LLE ist benannt, aber `unsupported` und keine Alpha-Voraussetzung. Eine spaetere
Aktivierung muesste lokale Nutzerabbilder vor dem Mapping auf Groesse und
Integritaet pruefen, Resetvektoren und Aliase festlegen und denselben
beobachtbaren Bootvertrag wie HLE erfuellen. Bis dahin endet jeder LLE-Versuch
kontrolliert vor Teilwirkungen.

## Lokale Eingaben und Arbeitskopien

BIOS- und Flashquellen sind stets read-only, werden nie paketiert und muessen
ausserhalb des Port-Ausgabeordners liegen. Benutzt ein kuenftiges HLE-/LLE-Profil
eine veraenderliche Flashabbildung, ist eine getrennte kontrollierte
Arbeitskopie ausserhalb des Ports Pflicht. Der Vertragsvalidator prueft diese
Pfadgrenzen lexikalisch, bevor eine Firmwarequelle geoeffnet wird.

Tests, Beispiele und Gates enthalten ausschliesslich synthetische Homebrew-,
Flash- und Speicherfixtures. BIOS-, Flash-, Font-, PVR- oder sonstige
proprietaere Firmwaredaten werden nicht eingecheckt oder verteilt.
