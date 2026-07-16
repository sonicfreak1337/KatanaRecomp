# Reproduzierbare Pre-Alpha-Artefakte

`artifact-debug` erbt alle bisherigen Phase-8-Debugprofile und aktiviert
reproduzierbare Compiler- und Linkoptionen. Lokale Quellwurzelpfade werden in
Debuginformationen auf `.` abgebildet. Das Profil bleibt ein Debug-Build; es
ist weder ein regulaerer Release-Build noch das v0.37.0-Gate.

Nach einem sauberen Build erzeugt der folgende Befehl das Paket:

```text
powershell.exe -NoProfile -ExecutionPolicy Bypass -File tools/quality/write-reproducible-artifact.ps1
```

Die feste Positivliste enthaelt ausschliesslich `katana-recomp`, `katana-fuzz`,
`VERSION`, `README.md`, `CHANGELOG.md` und `ROADMAP.md`. Disc-, GDI-, Firmware-,
BIOS-, Flash-, Portexport- und sonstige generierte Nutzdaten werden weder
gesucht noch akzeptiert. Ein versioniertes Manifest nennt Git-Commit,
Entwicklungsversion, Groesse und SHA-256 jedes Paketeintrags, aber keine
absoluten Hostpfade.

Der Paketierer sortiert alle Eintraege ordinal, speichert sie unkomprimiert und
setzt jeden ZIP-Zeitstempel auf 1980-01-01 UTC. Er erzeugt zwei unabhaengige
Kandidaten und vergleicht ihre SHA-256-Werte. Nur bei Bytegleichheit bleiben
unter `build-current/artifacts/` zurueck:

- `KatanaRecomp-0.37.0-dev.zip`
- `artifact-manifest.json`
- `KatanaRecomp-0.37.0-dev.zip.sha256`

Ein schmutziger Git-Stand, fehlende oder doppelte Werkzeuge, ein abweichender
Buildpfad und jede Doppelgenerierungsabweichung brechen sichtbar ab. Der Lauf
erfolgt erstmals gesammelt in KR-3709.
