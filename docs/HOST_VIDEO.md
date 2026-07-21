# Native Hostvideo-Runtime

KR-4701 definiert `katana-native-video` als Runtimevertrag Version 1. Externe
Portprojekte erhalten die Schnittstelle ueber `katana_runtime`; die erzeugte
`game.exe` benoetigt die KatanaRecomp-CLI nicht als Laufzeithuelle.

## Vertrag

`NativeVideoOutput` besitzt einen klaren Hostlebenszyklus:

- Erstellung prueft Vertragsversion, Fenstertitel und Clientgeometrie.
- `show()` macht das native Fenster sichtbar.
- `poll_events()` verarbeitet Hostereignisse und liefert Zeichenfehler als
  C++-Ausnahme an den Besitzer zurueck.
- `resize()` aendert die Clientflaeche; Frames werden mit erhaltenem
  Seitenverhaeltnis und schwarzen Raendern skaliert.
- `present()` akzeptiert ausschliesslich vollstaendige RGBA8-Frames und zaehlt
  erfolgreiche Einreichungen monoton.
- `request_close()` und das native Schliessen setzen dieselbe kontrolliert vom
  Port auszuwertende Close-Anforderung. Der Destruktor gibt das Hostfenster frei.

Gastzyklen und Presentation bleiben getrennt. Resize, Fensternachrichten und
Host-Present veraendern weder Schedulerzeit noch Dreamcast-Zustand.

Der PVR-Syncgenerator ist Teil derselben Gastzeitdomäne. `SPG_STATUS` liefert
die aktuelle Scanline sowie Field- und Blank-Status dynamisch. Beim
Firmware-Handoff wird anhand der Disc-Region PAL-Interlace oder NTSC-Interlace
aktiviert; PAL/NTSC non-interlaced und VGA stehen als vollstaendig definierte
Hardwareprofile fuer spaetere Gastumschaltungen bereit. Die Werte stammen aus
der Sega-Systemarchitektur und sind nicht an einzelne Spieleadressen gebunden.

Der vorgelagerte Tile-Accelerator uebernimmt bei `TA_LIST_INIT` die
dokumentierten OPB-/ISP-Initialwerte in seine Arbeitszeiger. `TA_LIST_CONT`
setzt einen abgeschlossenen Parameterstrom am programmierten Objektlistenanfang
fort, ohne bereits erzeugte Primitive des aktuellen Frames zu verwerfen. Eine
offene Liste oder ein unvollstaendiger 64-Byte-Parameter ist kein gueltiger
Fortsetzungspunkt und wird explizit abgewiesen.

## Produktpfad und Plattformen

Der Windows-Backendpfad verwendet ein echtes Win32-Fenster und eine
GDI-DIB-Presentation. Sie ist eine vollstaendige, skalierende RGBA-Ausgabe mit
Aspect-Ratio-Erhalt und kein No-op; die eigentliche allgemeine PVR-
Rasterisierung arbeitet derzeit jedoch auf der CPU und nutzt eine vorhandene
Host-GPU noch nicht. GPU-Rasterisierung bleibt daher eine sichtbare
Performancegrenze, nicht eine behauptete Capability.

Der generierte Port dekodiert die aktiven PVR-Scanoutregister, einschliesslich
`VO_CONTROL`-Blanking und `BORDER_COL`, und erzeugt daraus RGBA-Frames aus
Dreamcast-VRAM. Ein Present wird erst zugelassen, nachdem der TA/PVR-Pfad einen
Frame erfolgreich gerendert hat. `KR_FIRST_GUEST_FRAME` wird zusaetzlich nicht
im Blankzustand gemeldet. Renderframes und Host-Presents bleiben getrennt in
der Diagnostik sichtbar. Das relocatierte Runtime-SDK linkt die notwendigen
Windows-Systembibliotheken selbst.

Auf Hosts ohne implementiertes natives Backend bleibt CLI/Core weiterhin ohne
Fenstersystem-Abhaengigkeit buildbar. `native_video_available()` liefert dort
`false`; ein erzwungener Erstellungsversuch scheitert explizit. Eine native
Linux-Presentation ist damit weiterhin offen und wird nicht als verfuegbar
ausgegeben.

## Nachweis

`katana-host-video-tests` verwendet einen im Test erzeugten, frei von externen
Assets und Rechten gehaltenen 2x2-Farbrahmen. Der Test prueft Vertrag, echtes
Win32-Fenster, Resize, RGBA-Present, abgeschnittene Frames und kontrolliertes
Schliessen. `katana-port-cli-tests` kompiliert und startet zusaetzlich den
produktiven `game.exe`-Pfad. Synthetische Lifecycle-Laeufe pruefen
KeyDown/KeyUp, Fokusverlust/Fokusgewinn, Close im laufenden und pausierten
Zustand sowie das nachweisliche Ende des nativen Gastdispatchs. Ein Frame wird
nur gezaehlt, wenn die Gastfixture gueltige PVR-Scanoutregister programmiert und
zuvor einen erfolgreichen PVR-Renderabschluss erzeugt; der aktuelle CLI-Smoke
behauptet daher keine nicht erzeugte Presentation.
