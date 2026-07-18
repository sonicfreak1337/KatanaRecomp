# GDI-Quellenvertrag

KatanaRecomp behandelt `.gdi` als read-only Mehrdateiquelle. Der Descriptor
und alle Trackdateien werden ausschliesslich lesend geoeffnet. Parser,
Identitaetsbildung, GD-ROM-Zugriff und ISO9660 legen keine Sidecar-Dateien an
und schreiben weder Descriptor noch Tracks zurueck.

Relative Trackpfade werden gegen den Ordner des Descriptors aufgeloest.
Absolute Hostpfade sind Laufzeitdetails und weder semantische Identitaet noch
Teil oeffentlicher Diagnosen. Die Quellidentitaet `gdi-sha256:<64 hex>` entsteht
deterministisch aus Tracknummern, LBAs, Typen, Sektorformaten, Offsets,
Sektorzahlen und den wiederverwendbaren Track-SHA-256. Ein unveraendert verschobener
Mehrdateisatz behaelt deshalb dieselbe Identitaet.

Trackdateien bleiben ueber dauerhafte read-only Handles geoeffnet. Tracknummern
und LBA-Intervalle sind indiziert, zusammenhaengende Sektoren werden gebuendelt
gelesen und ein abschaltbarer Cache behaelt hoechstens 256 dekodierte Sektoren.
ISO9660 besitzt zusaetzlich begrenzte Verzeichnis- und Extentcaches. Der genaue
P1-Vertrag und seine Messpunkte stehen in [`P1_DISC_IO.md`](P1_DISC_IO.md).

Audiotracks bleiben als Raw-Sektoren in ihrem Descriptorformat erreichbar.
Datentracks werden fuer den gemeinsamen `DiscSource`-, GD-ROM- und
ISO9660-Pfad auf 2048 Nutzbytes pro Sektor abgebildet. Unterstuetzt sind
direkte 2048-Byte-Sektoren, 2336-Byte-Mode-2-Sektoren sowie Mode 1 und Mode 2
in 2352- oder 2448-Byte-Raw-Sektoren. Luecken, Audiobereiche und unbekannte
Raw-Modi koennen nicht still als Datensektoren gelesen werden.

Bei mehreren Datentracks gilt der letzte, hoechstliegende Datentrack als
primaere Sitzung. Der ISO9660-Mount trennt die Sitzungs-LBA des Primary Volume
Descriptors von der LBA-Basis der Directory-Extents. Damit bleiben sowohl
relative synthetische Fixtures als auch absolute Extents realer Dreamcast-GDs
bereichsgeprueft lesbar, ohne Tracks umzubauen oder umzuschreiben.

Der lokale Phase-6-Runner liest die Bootmetadaten aus dem primaeren
Dreamcast-Bootsektor, validiert die Hardwarekennung und einen einfachen
ISO9660-Bootdateinamen und liest die Bootdatei zweimal ueber denselben
`DiscSource`-Pfad. Die allgemein dokumentierte Dreamcast-Disc-Ladeadresse ist
`0x8C010000`; titelbezogene Adressen oder Remaps werden nicht verwendet.

Seit Gastzeitvertrag 1 plant `GdRomAsyncReader` jede Completion direkt auf dem
zentralen `EventScheduler`. Ein Request wird allein durch das Fortschalten der
gemeinsamen Gastuhr fertig; eine getrennte GD-ROM-Uhr und ein manueller zweiter
`advance_to`-Aufruf existieren nicht mehr.

Repository und Release enthalten ausschliesslich synthetische GDI-Fixtures.
Private Disc-Dumps duerfen optional lokal fuer das jeweilige kumulative
Phasengate verwendet werden; ihre Pfade, Namen, Hashes, temporaer generierten
Bootblockquellen und Inhalte sind keine Releaseartefakte. Der Runner prueft die
Quelldateien vor und nach dem Lauf und behaelt nur den redigierten Bericht.
