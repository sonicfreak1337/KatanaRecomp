# Sichere Firmware- und Flash-Diagnostik

`firmware-diagnose` prueft lokale Dreamcast-BIOS- und Flashabbilder read-only.
Die Eingabe wird weder gemappt noch als Runtime-Arbeitskopie geoeffnet und das
Werkzeug besitzt in diesem Pfad keine Schreiboperation.

```text
katana-recomp firmware-diagnose bios  dc_boot.bin
katana-recomp firmware-diagnose flash dc_flash.bin
katana-recomp firmware-diagnose flash dc_flash.bin --sha256 <erwarteter-hash>
```

Der versionierte JSON-Bericht enthaelt erwartete und tatsaechliche Groesse,
SHA-256, Pruefergebnis, Bereichsklassifikation und bei Flash die festen
Partitionsgrenzen. Blockallozierte Partitionen werden ueber Magic, Bitmap,
64-Byte-Datensaetze und CRC-16 diagnostiziert. Pro logischem Block erscheinen
Generationszahl, juengster physischer Datensatz sowie die Zahl gueltiger CRCs.
Es werden keine Datensaetze, Firmwarestrings, BIOS-Schriften, Texturen oder
sonstigen Eingabebytes extrahiert.

## Konservative Klassifikation

Ohne vertrauenswuerdige Symbole oder explizite Analysebeweise wird das gesamte
2-MiB-BIOS als `unknown` gemeldet. KatanaRecomp versucht nicht, das ROM linear
zu disassemblieren und zufaellige Daten als Code auszugeben. Die bekannten
Flashpartitionen sind `data`. Das Format kann `code`, `data` und `unknown`
abbilden, ohne durch die Diagnose eine Ausfuehrbarkeit zu behaupten.

## Redaktion

Standardberichte setzen `portable` auf `true`, redigieren logische Block-IDs
und enthalten weder Region, Factory-/Serien- noch Netzwerkwerte. Die lokale
Option `--include-sensitive` gibt ausschliesslich die fuenfstellige Regionkennung
und logische Block-IDs frei; der Bericht wird dann als `portable: false`
markiert. Auch mit Opt-in werden keine kompletten Datensaetze, Passwoerter,
Adressen, Firmwarebytes oder Assets ausgegeben.

Die Partitionsgrenzen und der beobachtbare CRC-/Bitmapvertrag wurden
unabhaengig gegen die frei verfuegbare KallistiOS-Flashrom-Dokumentation und
-Implementierung abgeglichen. Es wurde kein Referenzcode uebernommen.
