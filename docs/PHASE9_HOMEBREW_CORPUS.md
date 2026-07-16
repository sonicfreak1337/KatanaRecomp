# Internes Phase-9-Homebrew-Korpus

Das Phase-9-Pflichtkorpus wird vollstaendig im Repository aus kurzen,
projekterzeugten Bytefolgen aufgebaut. Es enthaelt CPU-Konformitaet,
Konsolenausgabe, Controllerzustand, 2D-Farbwerte, PCM-Audio, ein integriertes
Minispiel, den synthetischen Firmware-Handoff sowie die gekoppelte
Scheduler-/DMA-/Interruptsequenz. Das maschinenlesbare Korpusmanifest nennt fuer
jeden Eintrag Ursprung, Groesse und SHA-256, aber keine lokalen Pfade.

Die Dateien sind unabhaengig von BIOS, Flash, Disc-Images, Flycast und dcrecomp.
Da KatanaRecomp vor KR-4902 noch keine Projektlizenz besitzt, lautet ihr
Verteilungsstatus bewusst `internal-until-KR-4902-license-decision`. Das Gate
darf daraus technische Berichte erzeugen, aber noch kein oeffentliches
Homebrew-Paket ableiten.

Der zusammenhaengende Lauf verarbeitet zwei Frameintervalle und koppelt Direct-
Homebrew-Boot, Maple-Eingabe, Tile-Accelerator/PVR-Framebuffer, AICA-Mixer,
Scheduler, DMA, Interruptannahme, Codeinvalidierung und Systemreplay. Sein
deterministischer JSON-Bericht traegt den Marker
`KR_PHASE9_HOMEBREW_HOST_FRAME`; Screenshots oder Audiodateien werden nicht
verteilt.

Das optionale lokale LLE-Smokeprofil ist standardmaessig nicht angefordert.
Ein explizites Opt-in stoppt vor jedem Dateizugriff mit
`KR_PHASE9_FIRMWARE_SMOKE_UNAVAILABLE`, solange der LLE-Pfad erst fuer KR-4601
vorgesehen ist.
