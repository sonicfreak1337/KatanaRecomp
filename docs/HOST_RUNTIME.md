# Native Audio-, Eingabe- und Hostruntime

KR-4703 erweitert `katana-native-host-runtime` auf Version 2. Der Vertrag verbindet
Hostaudio, explizite Fenster-/Tastaturereignisse und Maple-Eingabe mit dem
gastzeitdeterministischen Scheduler, ohne Entscheidungen aus der Host-Wall-
Clock abzuleiten. KR-4814 ergaenzt den nativen Controllervertrag und bindet
Liveeingaben des generierten Produktports direkt an die gastzeitliche
Maple-Sicht.

## Audio

`HostAudioOutput` uebernimmt PCM16-Stereo aus dem AICA-Mixer. Jeder Puffer wird
zusammen mit seiner Samplerate stabil gehasht. Identische Samples erzeugen
damit unabhaengig vom Ausgabegeraet denselben Testnachweis.

Unter Windows reicht `Win32AudioOutput` Puffer asynchron an WinMM/WaveOut
weiter. Pause, Resume und Shutdown verwenden native Geraeteoperationen;
Shutdown setzt Puffer zurueck, loest Header und schliesst das Geraet. Ohne
verfuegbares Audiogeraet bleibt der Recording-Pfad nutzbar. Nicht implementierte
Plattformen behaupten keine native Audioausgabe.

Im generierten Port ist dieser Mixer kein Platzhalter: Jeder Audio-Tick liest
die 64 AICA-Slots aus demselben Registerfile und Sound-RAM, die SH-4 und der
echte AICA-ARM7 gemeinsam beschreiben. Seit `e1d8ade` startet der ARM7TDMI bei
der gastseitigen Resetfreigabe, erhaelt `512` Zyklen je Sample und bedient
Sound-/Main-Interrupts, Timer, REG_L/REG_M und Common-Monitorregister. Der
Mixer dekodiert die dadurch programmierten PCM16-, PCM8- und AICA-ADPCM-
Stimmen und wendet Keying, Loopgrenzen, Pitch, Lautstaerke, Direct Send, Pan
und Master Volume an. ARM-Fehler werden als `Arm7ExecutionFailure` sichtbar
und niemals als erfolgreiche Ausfuehrung gemeldet.

## Eingabe und Lebenszyklus

Unter Windows pollt `Win32GamepadSource` XInput dynamisch ueber
`xinput1_4.dll`, `xinput1_3.dll` oder `xinput9_1_0.dll`. Aktuelle
Xbox-Controller verwenden damit den nativen XInput-Vertrag. Parallel stellt
WinMM/Joystick einen identitaetsgebundenen Pfad fuer die bekannten DualSense-
und DualShock-Layouts bereit. Unbekannte HID-Button- und Achsordnungen werden
nicht als Standardprofil geraten. Ein mehrdeutiger XInput-Endpunkt bleibt bei
gleichzeitigem Sony-HID unsichtbar, bis aktive Eingabeevidenz ihn eindeutig
als Duplikat oder unabhaengigen Controller klassifiziert. Ein gebundenes
Duplikat teilt den XInput-Vibrationsendpunkt. Auf Hosts ohne geladenes XInput
und ohne WinMM-Joysticktreiber wird keine native Verfuegbarkeit vorgetaeuscht.

`ControllerInputTimeline` normalisiert die Profile mit versionierten Deadzones
auf Dreamcast-Buttons, 8-Bit-Trigger und zentrierte 8-Bit-Achsen. Das
Win32-Fenster fuehrt seinen Keyboardfallback sowie Fokusereignisse durch
dieselbe Timeline. Hotplug, Fokusverlust und explizite beziehungsweise stabile
automatische Controller-1-Auswahl leeren entfernte Zustaende, sodass keine
Taste haengen bleibt. Nur tatsaechliche Aenderungen erhalten eine streng
monotone Sequenz und den Gastzyklus des Safepoints; Host-Wall-Clock und
unveraenderte Polls erzeugen keinen Gastinput.

Der generierte Produktport bindet `MapleControllerDevice` direkt an diese
Timeline. Seine Hosteventpump pollt den nativen Gamepadpfad an
Gast-Safepoints, fuehrt Keyboard und Fokus durch dieselbe Ereignisschnittstelle
und spiegelt nur Timeline-Aenderungen fuer Runtime-Metriken und Lebenszyklus in
`HostRuntimeSession`. Maple `GetCondition` fragt den letzten am tatsaechlichen
Transaktionszyklus sichtbaren Zustand ab und kodiert Buttons aktiv-niedrig;
Trigger und beide Analogsticks stammen aus demselben atomaren Sample. Damit
kann ein Hostpoll keinen halb alten, halb neuen Maple-Zustand erzeugen.
Portprojektvertrag 36 versioniert diese direkte Produktintegration.

Deterministische Probes verzweigen vor der Live-Initialisierung und verwenden
weiterhin ausschliesslich den Replay- beziehungsweise neutralen Inputpfad.
`ControllerInputReplay` konsumiert dieselbe Sequenz-/Gastzyklusspur, sodass
Live- und Replay-Mapleantworten fuer dieselben Transaktionszyklen bytegleich
bleiben.

`HostRuntimeSession` akzeptiert streng steigende Sequenzen und monotone
Gastzyklen. Fokusverlust/Pause stoppen Media-Clock und Audio. Resume setzt sie
fort. Close, Shutdown und jeder Fehlerpfad stoppen Audio und Media-Clock und
leeren alle Schedulerereignisse.

Version 2 fuegt den optionalen `HostPacer` und einen genau einmal ausgefuehrten
Persistenz-Callback hinzu. Der Pacer wird zusammen mit Media-Clock und Audio
pausiert beziehungsweise neu verankert. Shutdown leert zuerst den Scheduler,
stoppt die Hostausgabe und speichert danach Flash/VMU. Ein Savefehler ist ueber
`require_clean_shutdown()` sichtbar. Der vollstaendige Vertrag steht in
[`MUTABLE_STORAGE_AND_PACING.md`](MUTABLE_STORAGE_AND_PACING.md).

## Nachweis

Die erzeugte `game.exe` besitzt Input, Media-Clock, Hostaudio und Lebenszyklus
selbst. Sie praesentiert VRAM-Frames und reicht gastzeitgebundene AICA-Puffer ein; sie
meldet `audio_buffers`, `audio_hash` und `input_events`. Der Hostruntime-Test
prueft Eingabeinjektion, Audiohash, Fokus/Pause, WinMM und sauberen Shutdown;
Port-CLI und relocatiertes SDK pruefen den Produktpfad ohne CLI-Laufzeithuelle.

Die fokussierten KR-4814-Regressionen pruefen zusaetzlich identische
Normalisierung fuer XInput-, DualSense- und DualShock-Profile,
Deadzones, Trigger, Achsen, Hotplug, Fokusreset, Controller-1-Auswahl,
Aenderungsdeduplizierung, monotonen Replayinput und gastzyklusgenaue
Maple-`GetCondition`-Payloads. Die Projekt-Homebrew-Strecke vergleicht zwei
gastzeitlich getrennte Button-, Trigger- und Achszustaende bitgenau; der
Maple-MMIO-Test prueft zusaetzlich die Zustandsgrenze waehrend einer laufenden
DMA-Transaktion. Die Homebrew-Ausgabe leitet Bildfarbe und Geometrie
tatsaechlich aus Buttons, Triggern und beiden Analogsticks ab. Sechs
fokussierte Kernregressionen bestanden 6/6; der getrennte Produktpfad
`katana-port-cli-tests` pruefte Connect, unveraenderten Poll, Fokusreset,
Disconnect, Reconnect und die jeweiligen Maple-Payloads End-to-End 1/1.
Hostvideo,
Scheduler-Safepoint und PlatformServices bestanden ergaenzend 3/3, der
installierte SDK-/Packagevertrag 1/1, insgesamt damit 11/11 fokussierte Tests.
Dieser automatisierte Vertrag schliesst KR-4814. Die getrennte praktische
interaktive Sitzung bleibt `KR-4914`; sie ist noch offen, weil derzeit kein
Spiel bis zu einem bedienbaren Pfad bootet. Ein Voll-CTest lief nicht.
