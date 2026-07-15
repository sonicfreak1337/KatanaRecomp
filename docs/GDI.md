# GDI-Quellenvertrag

KatanaRecomp behandelt `.gdi` als read-only Mehrdateiquelle. Der Descriptor
und alle Trackdateien werden ausschliesslich lesend geoeffnet. Parser,
Identitaetsbildung, GD-ROM-Zugriff und ISO9660 legen keine Sidecar-Dateien an
und schreiben weder Descriptor noch Tracks zurueck.

Relative Trackpfade werden gegen den Ordner des Descriptors aufgeloest.
Absolute Hostpfade sind Laufzeitdetails und weder semantische Identitaet noch
Teil oeffentlicher Diagnosen. Die Quellidentitaet `gdi-fnv1a64:<wert>` entsteht
deterministisch aus Tracknummern, LBAs, Typen, Sektorformaten, Offsets,
Sektorzahlen und den aktiven Trackbytes. Ein unveraendert verschobener
Mehrdateisatz behaelt deshalb dieselbe Identitaet.

Audiotracks bleiben als Raw-Sektoren in ihrem Descriptorformat erreichbar.
Datentracks werden fuer den gemeinsamen `DiscSource`-, GD-ROM- und
ISO9660-Pfad auf 2048 Nutzbytes pro Sektor abgebildet. Unterstuetzt sind
direkte 2048-Byte-Sektoren, 2336-Byte-Mode-2-Sektoren sowie Mode 1 und Mode 2
in 2352- oder 2448-Byte-Raw-Sektoren. Luecken, Audiobereiche und unbekannte
Raw-Modi koennen nicht still als Datensektoren gelesen werden.

Repository und Release enthalten ausschliesslich synthetische GDI-Fixtures.
Private Disc-Dumps duerfen optional lokal fuer einen read-only Smoke-Test
verwendet werden; ihre Pfade, Namen, Hashes und Inhalte sind keine
Releaseartefakte.
