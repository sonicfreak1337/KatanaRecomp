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

## Produktpfad und Plattformen

Der Windows-Backendpfad verwendet ein echtes Win32-Fenster und GDI-DIB-
Presentation. Der generierte Port erzeugt nach erfolgreichem Runtimeeinstieg
einen 640x480-RGBA-Frame aus Dreamcast-VRAM, praesentiert ihn und meldet die
Framezahl in `KATANA_RUNTIME_METRICS`. Das relocatierte Runtime-SDK linkt die
notwendigen Windows-Systembibliotheken selbst.

Auf Hosts ohne implementiertes natives Backend bleibt CLI/Core weiterhin ohne
Fenstersystem-Abhaengigkeit buildbar. `native_video_available()` liefert dort
`false`; ein erzwungener Erstellungsversuch scheitert explizit. Eine native
Linux-Presentation ist damit weiterhin offen und wird nicht als verfuegbar
ausgegeben.

## Nachweis

`katana-host-video-tests` verwendet einen im Test erzeugten, frei von externen
Assets und Rechten gehaltenen 2x2-Farbrahmen. Der Test prueft Vertrag, echtes
Win32-Fenster, Resize, RGBA-Present, abgeschnittene Frames und kontrolliertes
Schliessen. `katana-port-cli-tests` und der relocatierte GUI-Paketlauf pruefen
zusaetzlich den produktiven `game.exe`-Pfad bis `frames=1`.
