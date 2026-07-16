# Adress-zu-Quelle-Mapping

Der C++-Emitter markiert jede erzeugte Instruktionssequenz mit einem stabilen
Kommentar `// katana-guest 0xXXXXXXXX`. Daraus entsteht die versionierte
`katana-address-source-map` ohne Compiler- oder Debugformatabhaengigkeit.

Jede Position enthaelt:

- die numerische SH-4-Gastadresse
- den portablen Image-Segmentnamen und den Byteoffset in der Eingabe
- den relativen Pfad der generierten Translation Unit
- die 1-basierte Zeile des zugehoerigen Markers

Mehrere generierte Positionen fuer dieselbe Gastadresse bleiben als
deterministisch sortierte Mehrfachzuordnung erhalten. Der Lookup liefert den
vollstaendigen zusammenhaengenden Bereich dieser Positionen. Unsortierte Maps,
absolute oder ausbrechende Generated-Pfade, nichtportable Segmentnamen und
Nullzeilen werden abgelehnt.

Das Mapping landet beim Portexport unter
`generated/metadata/source-map.json`. Es enthaelt weder den lokalen Eingabepfad
noch Hostzeiger oder Compiler-Buildpfade und kann deshalb zusammen mit Crash-,
Dispatch- und Traceberichten portabel ausgewertet werden.
