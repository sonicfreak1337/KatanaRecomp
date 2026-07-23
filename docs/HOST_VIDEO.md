# Native Hostvideo-Runtime

KR-4701 definiert `katana-native-video` als Runtimevertrag Version 2. Externe
Portprojekte erhalten die Schnittstelle ueber `katana_runtime`; die erzeugte
`game.exe` benoetigt die KatanaRecomp-CLI nicht als Laufzeithuelle.
Der kumulative Integrationsstand verwendet Runtime-ABI 39, Block-ABI 3,
Backend-Interface-ABI 3 und Portprojektvertrag 24.

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
Dreamcast-VRAM. Read- und Write-Framebuffer benutzen dabei dieselbe logische
32-Bit-VRAM-Sicht: Jeder logische Zugriff wird ueber die Dreamcast-`map32`-
Abbildung in das gemeinsame 8-MiB-Backing uebertragen. Das gilt sowohl fuer
TA-Renderziele als auch fuer Direct-Framebuffer-Writes des recompilierten
Gasts; Parameter- und Texturspeicher bleiben davon getrennt linear.

Der Scanout dekodiert opakes `RGB0555`, ignoriert dessen nicht als Alpha
definiertes Bit 15 und fuegt die drei `FB_R_CTRL.CONCAT`-Bits an die
Farbkanaele an. `RGB565`, gepacktes `RGB888` und `RGB0888` folgen demselben
Pfad. `FB_R_SIZE` verwendet den Hardwarestride
`(x_size + modulus) * 4`: Modulus 0 ueberlappt die vorherige Zeile um ein
32-Bit-Wort, Modulus 1 ist dicht, groessere Werte fuegen Padding ein. PAL-
Interlace kombiniert benachbarte SOF1-/SOF2-Felder nur bei passender
Weave-Geometrie; getrennte Felder werden feldweise ueber den aktuellen
`SPG_CONTROL`-Zustand abgetastet.

Ein TA-Renderabschluss speichert fuer jeden final geaenderten Pixel den
gepackten Zielwert und die geaenderte Bytemaske als monotone
Generationsevidenz. Direkte Gastwrites setzen backing-byte-adressierte
Dirty-Evidenz im gemeinsamen VRAM-Backing. Beim VBlank wird sie mit dem
vorherigen Scanout-Abbild verglichen; erst ein sichtbar geaenderter,
tatsaechlich abgetasteter Pixel kann einen Direct-Proof erzeugen.
`PvrRegisterFile` ruft den Renderer am tatsaechlichen Scheduler-VBlank-In auf.
Erst dort werden die aktuellen VRAM-Bytes erneut validiert und der exakte
`PvrFrame` eingefroren; ein Render oder Direct-Write nach einem bereits
verarbeiteten VBlank kann deshalb erst am folgenden VBlank zaehlen.

Ein Proof verlangt aktiviertes `FB_R`, ungeblankten Videoausgang und einen
tatsaechlich abgetasteten Evidenzpixel. Seine Quelle ist explizit entweder
`TaRender` oder `DirectFramebuffer`. Ein Direct-Proof entsteht nur, wenn die
backing-byte-adressierte Dirty-Evidenz gegen das vorherige Scanout-Abbild einen
sichtbaren Pixel des aktuellen Scanouts als geaendert belegt; ein Offscreen-
Write, ein unveraenderter Write oder blosses Entblanken reicht nicht.
PAL/Interlace prueft das aktive `SPG_CONTROL`-Feld
gegen `FB_R_SOF1` beziehungsweise `FB_R_SOF2` und traegt das Feld in den Proof
ein. Auf der Schreibseite waehlen
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

Der erste private Produktnachweis dieser kombinierten Kette stammt aus einem
Sonic-Adventure-PAL-AOT-Lauf in der v0.48-Entwicklung. Der recompilierte
Discbootstrap `IP.BIN` schreibt den Direct-Framebuffer und erreicht innerhalb
eines 50-Millionen-Gastzyklusbudgets in 5,3 Sekunden beide Marker. TA bleibt
dabei null. Der folgende Budget-Exit ist erwartet; BootExecutable und
Spielboot sind zu diesem Zeitpunkt noch nicht erreicht.

Nach dem vollstaendigen 178/178-x64-Gate reproduziert ein frisch exportierter
Vertrag-24-/Runtime-ABI-39-Port den Nachweis mit `frames=2`,
`pvr_guest_frames=2`, `pvr_direct_frames=2` und 302.287 geaenderten
Direct-FB-Pixeln. TA, Rendergeneration und Materializer bleiben null; der
Budget-Exit nach 50 Millionen Gastzyklen ist erwartet.

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
Write-Framebuffer, einen spaeteren RTT-Flip, Scaling/Interlace-Abtastung,
logische 32-Bit-VRAM-Abbildung, `RGB0555`/Concat, gepacktes `RGB888`, Modulus
0/1/>1, PAL-Weave und feldweisen PAL-Scanout. Ein Direct-FB-Test belegt
zusaetzlich, dass ein sichtbarer Gastwrite ohne TA am VBlank genau einmal
zaehlt, waehrend Offscreen-Writes und Blanking keinen Proof erzeugen. Eine
ausfuehrbare Regression
fuehrt Scheduler-VBlank, Proof, `pump_guest_frame_proof` und ein `FakeVideo`
pixelgenau zusammen; die Codegen-Regression bindet die beiden Marker an dessen
getrennte Resultate. Der aktuelle CLI-Smoke erwartet weiterhin bewusst keinen
Gastframe und behauptet daher weder eine nicht bewiesene Generation noch eine
nicht ausgefuehrte Presentation. Dieser synthetische Nullframe-Vertrag steht
nicht im Widerspruch zum getrennten privaten Direct-FB-Produktnachweis.
