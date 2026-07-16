# Projektmanifeste

Ein KatanaRecomp-Projektmanifest beschreibt Eingabedatei, Dateiformat und
Adresslayout unabhaengig vom aktuellen Arbeitsverzeichnis. Es ist eine
UTF-8-Textdatei mit genau einem `KEY = VALUE` pro Zeile. Leerzeilen und Zeilen,
die nach optionalem Leerraum mit `#` beginnen, werden ignoriert.

## Pflichtfelder

- `version = 1`
- `format = raw` oder `format = elf32-sh`
- `input = PFAD`
- `base_address = HEX` fuer `raw`

## Optionale Felder

- `entry_point = HEX`
- `map = PFAD`
- `segment_name = NAME` fuer `raw`
- `segment_kind = code`, `data` oder `unknown` fuer `raw`
- `permissions = rwx`, wobei `-` ein fehlendes Recht markiert, fuer `raw`

Relative `input`- und `map`-Pfade werden relativ zum Manifestverzeichnis
aufgeloest. Adressen sind hexadezimal und duerfen mit `0x` beginnen.
Unbekannte oder doppelte Felder, nicht unterstuetzte Versionen und Raw-Felder
in ELF-Projekten sind Fehler mit Datei- und Zeilenangabe.

## Raw-Beispiel

```text
version = 1
format = raw
input = program.bin
base_address = 0x8C010000
entry_point = 0x8C010000
segment_name = .text
segment_kind = code
permissions = r-x
map = program.map
```

## ELF32-SH-Beispiel

```text
version = 1
format = elf32-sh
input = program.elf
map = additional-symbols.map
```

ELF-Segmente, Berechtigungen, Einstiegspunkt, Symbole und Relocations stammen
aus der ELF-Datei. Ein optionaler `entry_point` fuegt einen weiteren bekannten
Einstiegspunkt hinzu; eine Map-Datei ergaenzt die Symbole.

## Version 2 und Ausfuehrungsprofil

Version 2 verwendet das feste Schema `katana-project` und namespaced Felder.
Ohne optionale Profilfelder gilt der sichere Homebrew-Standard: BIOS-freier
Direkteinstieg, deterministischer Scheduler, deaktivierte MMU, konservative
Fastpaths und `abort` statt stillem Fallback.

```text
schema = katana-project
version = 2
project.name = demo
input.format = raw
input.path = program.bin
image.base_address = 0x8C010000
image.entry_point = 0x8C010000
image.expected_entry_points = 0x8C010000,0x8C010100
segment.name = .text
segment.kind = code
segment.permissions = r-x
execution.firmware = direct
execution.fallback = abort
execution.scheduler = deterministic
execution.mmu = disabled
execution.fastpath = conservative
```

Optionale Listen sind kommasepariert. Bereiche verwenden `start:size`,
Aliasgruppen `virtuell:physisch:size`:

```text
memory.canonical_ranges = 0x0C000000:0x01000000
memory.alias_groups = 0x8C000000:0x0C000000:0x01000000
memory.writable_executable = 0x0C010000:0x00100000
execution.required_capabilities = memory,executable-ram,firmware-mode
image.dynamic_bios_vectors = 0x8C0000B0,0x8C0000B4
firmware.flash = flash.bin
```

`execution.firmware` akzeptiert `direct`, `hle` und `lle`; LLE verlangt eine
lokale `firmware.bios`, Aliasgruppen, kanonische physische Bereiche sowie die
Faehigkeiten `memory` und `firmware-mode`. HLE verlangt ebenfalls
`firmware-mode`. Der Parser prueft damit die strukturelle Vollstaendigkeit. Der ausfuehrbare
Alpha-Vertrag ist enger: `direct` und die dynamische HLE-BIOS-ABI sind
verfuegbar, LLE bleibt optional `unsupported`. Noch nicht angebundene HLE-
Hardwaredienste enden sichtbar als `service-unavailable`. Lokale BIOS-/Flashquellen und
veraenderliche Arbeitskopien muessen ausserhalb eines Port-Ausgabeordners
bleiben; keine Firmwarequelle wird paketiert.

Ein aktiver `interpreter`- oder `diagnostic`-Fallback verlangt
`controlled-fallback`, `execution.mmu = sh4` verlangt `mmu`, und WX-Bereiche
verlangen `executable-ram`. Unbekannte Profile und widerspruechliche Aliase
scheitern beim Parsen vor Loader und Analyse.

Dynamische BIOS-Vektoren beschreiben RAM-Laufzeitzustand. Der Loader macht sie
weder zu statischen Einstiegspunkten noch zu ROM-Symbolen. BIOS und Flash
bleiben optionale lokale Pfade; ihre Inhalte werden nicht im Manifest
eingebettet.
