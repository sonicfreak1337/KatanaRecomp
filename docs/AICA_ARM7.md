# AICA-ARM7-Strategie

## Aktueller Vertrag

Seit `e1d8ade` ist der echte AICA-ARM7TDMI-Pfad Bestandteil des RuntimeOnly-
Bring-ups. Der oeffentliche Runtime-Vertrag steht auf ABI `90`, der portable
AICA-Handoff auf Version `2`. Das aendert nicht die SH-4-Produktgrenze:
RuntimeOnly bleibt statisches SH-4-AOT mit exakter Guest->Host-Tabelle,
Stop-on-miss und typed abort; es gibt keinen SH-4-Interpreter, JIT,
Runtime-Decoder oder geratenen Zielpfad.

`AicaExecutionController` behaelt `HighLevelAudio` fuer den Reset-/Fallback-
Lebenszyklus und wechselt bei der gastseitigen ARM-Resetfreigabe auf
`LowLevelArm7`. Der ARM7 fuehrt dann echten Code aus dem gemeinsamen AICA-
Sound-RAM aus. Ein fehlender Bus, ein ungueltiger Restore oder ein CPU-Fehler
endet sichtbar und fail-closed als `Arm7ExecutionFailure`; der Controller
behauptet keine erfolgreiche Firmwareausfuehrung.

## Takt und Bus

- Der ARM7 erhaelt `512` Zyklen je 44,1-kHz-Audiosample. Das entspricht dem
  22,5792-MHz-AICA-ARM-Takt.
- ARM-Adressen sind 24 Bit breit. Das 2-MiB-Sound-RAM wird entsprechend der
  AICA-Sicht gespiegelt; der Registerbereich fuehrt auf dasselbe Registerfile,
  das SH-4-MMIO, Mixer, Snapshot und Restore verwenden.
- Byte-, Halfword- und Wordzugriffe laufen ueber einen gemeinsamen Busvertrag.
  ARM7 und SH-4 beobachten deshalb denselben Channel-, Timer- und
  Interruptzustand.
- Der HLE-Mixer bleibt fuer die native PCM-Ausgabe verantwortlich, liest aber
  die vom echten ARM7 programmierten 64 AICA-Slots. PCM16, PCM8 und AICA-ADPCM,
  Keying, Loopgrenzen, Pitch, Lautstaerke, Direct Send, Pan und Master Volume
  bleiben darin deterministisch.

## Interrupt- und Monitor-Lifecycle

Der Registervertrag umfasst SCIEB/SCIPD/SCIRE fuer den Soundprozessor,
MCIEB/MCIPD/MCIRE fuer den SH-4, SCILV0/1/2 sowie REG_L/REG_M. Sample-Done und
die drei Timer setzen ihre jeweiligen Pending-Bits; aktivierte Soundinterrupts
werden auf die ARM7-FIQ-Leitung und den Vektor `0x1c` gefuehrt. Main-
Interrupts bleiben ueber den bestehenden AICA-/ASIC-Pfad sichtbar.

Die Common-Monitorregister liefern den fuer Retailtreiber benoetigten MIDI-
Leerstatus, den Envelope-/Sample-Lifecycle des ausgewaehlten Channels und
dessen Current Address. Der Loop-Latch wird beim vorgesehenen Highbyte-Read
quittiert. Damit stehen Polling und Zeitbasis nicht mehr auf statischen
Defaultwerten.

Reset, Schedulerreset und Restore verwenden denselben Lifecycle. Portable
Snapshots enthalten Registerbanken, Pipeline-/Prefetchzustand,
Blocktransfer-Zwischenzustand, Zyklenschuld, FIQ-/Waitzustand und
Ausfuehrungszaehler. Restore validiert Modus, Grenzen und Fehlerkonsistenz vor
der Zustandsuebernahme.

## Herkunft und rechtliche Grenze

Der ARM7TDMI-Kern stammt aus SkyEmu-Commit
`01516d6798e3652b583e6a366085bb51c43b528d` und ist unter MIT eingebunden.
`third_party/skyemu/LICENSE` und `third_party/skyemu/README.md` dokumentieren
Lizenz, Provenienz und die einzige lokale const-correctness-Anpassung. Es
wurden keine Dreamcast-Firmware, Sonic-Retailbytes oder Spieldaten in das
Repository aufgenommen; solche Daten bleiben nutzerbereitgestellte externe
Read-only-Eingaben.

## Nachweis und verbleibende Produktgrenze

Der vorhandene `katana-aica-execution-tests`-Pfad wurde an den realen ARM7-
Lifecycle angepasst und bestand nach einem erfolgreichen 24-Thread-Build.
Ein diagnostischer Sonic-Lauf belegte rund 50,7 Millionen ausgefuehrte ARM-
Instruktionen ohne CPU-Fehler, zwei aktive Stimmen und einen vom frueheren
Stillewert abweichenden Audiohash.

Der anschliessende `45,564 s` lange Sonic-PAL-Produktlauf erreichte SEGA,
PAL-Auswahl und Presented by Sega ohne Fatalfehler. Der zuvor bei null
stehende Sofdec-Audiotakt erreichte `0x2D0` und `0x890` bei der Einheit
`0xAC44` (`44.100`). Der Film blieb trotzdem unsichtbar. AICA-Bereitschaft und
Audiotakt sind damit geschlossen, nicht aber KR-4981. Der naechste P0 ist die
nachgelagerte CRI-/Sofdec-Callback-, YUV-/TA- und gastgesteuerte FB_R-
Bildpublikationsfolge. Movie-Skip, automatischer Framebuffer-Flip oder
titelbezogener Runtime-Hack sind keine Produktloesung.
