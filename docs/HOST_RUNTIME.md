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

Der native Produktport verwendet diesen historischen AICA-/ARM7-Pfad nicht.
`NativePortAudioEngine` reicht bounded PCM16-Hostfeeds und native
Codecstimmen an die Plattformausgabe. `NativePortSoundBankEngine` verarbeitet
Manatee-MLT/SMPB/SMSB/SFPB/SFOB/SFPW semantisch und rendert PCM16, PCM8 sowie
AICA-ADPCM direkt zu Host-PCM. Programme, Layer/Splits, Sequenzen, Loops,
Controller, Pitch/Pan/Sends, Huelle, Filter/LFO und die belegte
QSound/Reverb-Konfiguration bleiben im nativen Dienst; Sound-RAM,
AICA-Register, ARM7, Kommandoringe, Interrupts und G2-DMA werden nicht
nachgebildet. Generation-gepruefte Handles, bounded Queues/Budgets und
threadgebundene Mutationen enden bei unbekannten Formaten fail-closed.

Soundbankvertrag `6` ergaenzt eine bounded Predecode-Grenze und den
identitaetsgebundenen `SPSR`-PCM-Stream-Ring-Vertrag im 2-MiB-Manatee-
Layoutfenster. Ringbereiche sind ausgerichtet, ueberlappungsfrei und ueber
Generationen gebunden; es wird kein Gast-Sound-RAM und keine AICA-Oberflaeche
erzeugt. Payloadlose `SFPW`-Einheiten werden als Layoutreservierung validiert,
waehrend befuellte `SOSB`-Einheiten bis zur separat bewiesenen One-shot-ABI
typisiert abgebrochen werden. Die Predecode-Grenze nutzt weiterhin dieselben
PCM16-/PCM8-/ADPCM-Decoder und denselben Samplecache wie Live-Noten.
Der Sonic-Korpus belegt `122` Collections, `52.253.920` Bytes, `7.139`
Splits, `5.596` Sequenzen, `40.294` Events und `4.698` eindeutige Samples
(`115` PCM16, `246` PCM8, `4.337` ADPCM; `94.889.624` Frames). Damit werden
Formatfehler vor der ersten Note sichtbar und First-Note-Stalls vermieden.

Der alte 64-Slot-AICA-/ARM7-Mixer bleibt ausschliesslich im nicht
installierbaren historischen Diagnosepfad. Seine fruehere
`Arm7ExecutionFailure`-Semantik ist kein Produktvertrag.

## Speicherstaende

Der native Produktpfad verwendet `NativePortSaveProvider` als semantische
Titel-/SDK-Grenze. Bis zu 24 explizit konfigurierte Endpunkte liefern Query,
List, Read, Write und Remove ueber logische IDs; die Standardgeometrie eines
VMU-artigen Nutzermediums betraegt 200 Bloecke zu 512 Bytes. Profil und Medium
sind Bestandteil des inneren Volumes, waehrend der Plattformdienst das
gesamte Volume projekt-, slot-, schema-, generations- und digestgebunden mit
Backup-Recovery atomar speichert. Reads kopieren erst nach vollstaendiger
Validierung in den Zielpuffer; Writes und Removes publizieren nur ein
vollstaendiges neues Volume. DirectoryFull, InsufficientBlocks, ReadOnly,
GenerationConflict, Corrupt und Incompatible bleiben getrennte Fehler.

Diese Schicht ist kein Maple-/VMU-Geraet: Ein privater Titeladapter erhaelt
die originalen ABI-, RAM- und Callback-Seiteneffekte und ruft ausschliesslich
die abgeschlossenen semantischen Operationen auf.

## Eingabe und Lebenszyklus

Unter Windows pollt `Win32GamepadSource` XInput dynamisch ueber
`xinput1_4.dll`, `xinput1_3.dll` oder `xinput9_1_0.dll`. Aktuelle
Xbox-Controller verwenden damit den nativen XInput-Vertrag. Parallel stellt
WinMM/Joystick einen ueber DirectInput-VID/PID und Geraeteinstanz
identitaetsgebundenen Pfad fuer die bekannten DualSense- und DualShock-
Layouts bereit. Unbekannte HID-Button- und Achsordnungen werden
nicht als Standardprofil geraten. Ein mehrdeutiger XInput-Endpunkt bleibt bei
gleichzeitigem Sony-HID unsichtbar, bis drei aufeinanderfolgende aktive
Eingabesamples ihn eindeutig als Duplikat oder unabhaengigen Controller
klassifizieren. Ein gebundenes Duplikat teilt den XInput-
Vibrationsendpunkt, der auch beim Shutdown gestoppt wird. Hotplug verwirft
veraltete Unabhaengigkeitsevidenz. Auf Hosts ohne geladenes XInput
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
