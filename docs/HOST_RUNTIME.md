# Native Audio-, Eingabe- und Hostruntime

KR-4703 erweitert `katana-native-host-runtime` auf Version 2. Der Vertrag verbindet
Hostaudio, explizite Fenster-/Tastaturereignisse und Maple-Eingabe mit dem
gastzeitdeterministischen Scheduler, ohne Entscheidungen aus der Host-Wall-
Clock abzuleiten.

## Audio

`HostAudioOutput` uebernimmt PCM16-Stereo aus dem AICA-Mixer. Jeder Puffer wird
zusammen mit seiner Samplerate stabil gehasht. Identische Samples erzeugen
damit unabhaengig vom Ausgabegeraet denselben Testnachweis.

Unter Windows reicht `Win32AudioOutput` Puffer asynchron an WinMM/WaveOut
weiter. Pause, Resume und Shutdown verwenden native Geraeteoperationen;
Shutdown setzt Puffer zurueck, loest Header und schliesst das Geraet. Ohne
verfuegbares Audiogeraet bleibt der Recording-Pfad nutzbar. Nicht implementierte
Plattformen behaupten keine native Audioausgabe.

## Eingabe und Lebenszyklus

Das Win32-Fenster liefert Fokus, Tastendruck/-freigabe und Close monoton
sequenziert. Der Port bildet Pfeiltasten sowie Enter, Z, X, A und S auf
Dreamcast-Controllerfelder ab und injiziert sie in `InjectedHostInput`. Maple
liest nur diesen Zustand; Pollingzeit und Wall-Clock erzeugen keinen Gastinput.

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
selbst. Sie praesentiert einen VRAM-Frame, reicht einen AICA-Puffer ein und
meldet `audio_buffers`, `audio_hash` und `input_events`. Der Hostruntime-Test
prueft Eingabeinjektion, Audiohash, Fokus/Pause, WinMM und sauberen Shutdown;
Port-CLI und relocatiertes SDK pruefen den Produktpfad ohne CLI-Laufzeithuelle.
