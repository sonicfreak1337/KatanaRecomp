# Interner GUI-Asset-Eingang

Fuer Phase 10 liegt ausserhalb des Repositorys ein vom Nutzer bereitgestelltes
App-Logo unter dem privaten Asset-Eingang `private/assets/KatanaLogo.png`.

Gepruefte technische Eigenschaften:

- PNG, 1024 x 1024 Pixel
- 32-Bit-ARGB mit Transparenz
- SHA-256 `56edc4240df8d2dff6c2d3b68cd919a320774e21c5395b605d71960c4da31108`
- quadratische Wort-/Bildmarke mit Katana, orangefarbener Spirale und
  `KATANA RECOMP`-Schriftzug

Das identische PNG wurde als interner GUI-Kandidat ins Repository uebernommen.
KR-4510 erzeugt daraus reproduzierbar `KatanaLogo.ico` mit 16, 24, 32, 48, 64,
128 und 256 Pixeln und bindet es als Fenster-, Taskleisten- und EXE-Ressource
ein. Der Ableitathash lautet
`76d62ba3363939b2008ee213b7dbb8c75a43512026714bec0de20271af24fb46`.
Private Quellpfade erscheinen weder in Ressource noch Paket. Die interne
Anwendungsnutzung ist vom Nutzer beauftragt; eine oeffentliche Distribution
bleibt bis zum vollstaendigen KR-4902-Daten- und Lizenzaudit gesperrt.
