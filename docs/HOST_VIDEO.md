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
Dreamcast-VRAM. Ein Renderabschluss speichert fuer jeden final geaenderten
Pixel den gepackten Zielwert und die geaenderte Bytemaske als monotone
Generationsevidenz. `PvrRegisterFile` ruft den Renderer am tatsaechlichen
Scheduler-VBlank-In auf. Erst dort werden die aktuellen VRAM-Bytes erneut
validiert und der exakte `PvrFrame` eingefroren; ein Render nach einem bereits
verarbeiteten VBlank kann deshalb erst am folgenden VBlank zaehlen.

Ein Proof verlangt aktiviertes `FB_R`, ungeblankten Videoausgang und einen
tatsaechlich abgetasteten Evidenzpixel. PAL/Interlace prueft ausschliesslich das
aktive `SPG_CONTROL`-Feld gegen `FB_R_SOF1` beziehungsweise `FB_R_SOF2` und
traegt das Feld in den Proof ein. Auf der Schreibseite waehlen
`SCALER_CTL.Interlace` Bit 17 und `Field Select` Bit 18 `FB_W_SOF1` oder
`FB_W_SOF2`. Scaling und Interlace bestimmen, ob der geaenderte Quellpixel im
Ausgabebild wirklich gesampelt wird; eine Offscreen-/RTT-Generation bleibt bis
zu einem passenden Read-Framebuffer-Flip pending.

Die Haltegrenze betraegt 256 Generationen beziehungsweise 64 MiB. Ein
Bereichs-Fast-Reject vermeidet unnoetige Pixelscans; pro VBlank werden maximal
2.097.152 Pixelrecords fair ab dem fortgeschriebenen Cursor geprueft. Die
Metriken `dropped_render_evidence_generations`,
`render_evidence_pixels_examined`, `render_evidence_range_rejections` und
`render_evidence_scan_budget_exhaustions` machen Verwerfen, Arbeit und
Budgetende sichtbar. Scanouts ueber 2048 x 2048 Pixel beziehungsweise
4.194.304 Pixel insgesamt werden vor Frameallokation abgewiesen.

`pump_guest_frame_proof` meldet den eingefrorenen Proof hostunabhaengig und
reicht exakt dessen `PvrFrame` optional an `NativeVideoOutput` weiter.
`KR_FIRST_GUEST_FRAME` entsteht aus `guest_proven`; der getrennte
`KR_FIRST_PRESENTED_FRAME` erst nach nachweislich erfolgreichem Present. Das
relocatierte Runtime-SDK linkt die notwendigen Windows-Systembibliotheken
selbst.

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
Zustand sowie das nachweisliche Ende des nativen Gastdispatchs. Synthetische
PVR-Regressionen pruefen deaktiviertes `FB_R`, Blank, einen nicht gebundenen
Write-Framebuffer, einen spaeteren RTT-Flip, Scaling/Interlace-Abtastung und
das einmalige Konsumieren einer Rendergeneration. Eine ausfuehrbare Regression
fuehrt Scheduler-VBlank, Proof, `pump_guest_frame_proof` und ein `FakeVideo`
pixelgenau zusammen; die Codegen-Regression bindet die beiden Marker an dessen
getrennte Resultate. Der aktuelle CLI-Smoke erwartet weiterhin bewusst keinen
Gastframe und behauptet daher weder eine nicht bewiesene Generation noch eine
nicht ausgefuehrte Presentation. Die fokussierten Targets `pvr-render`,
`pvr-framebuffer`, `host-video` und `port-export` bestehen mit 12 Buildjobs 4/4
in 0,66 Sekunden.
