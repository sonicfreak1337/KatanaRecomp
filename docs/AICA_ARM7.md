# AICA-ARM7-Strategie

## Unterstuetzter v0.29-Pfad

KatanaRecomp verwendet fuer den minimalen Audiopfad ein explizites
High-Level-Audioprofil. SH-4-seitige AICA-Register, Sampledekodierung, Mixer,
Timer, Interruptzustand und Host-Pufferausgabe werden als getrennte
Runtime-Komponenten modelliert. Der ARM7 fuehrt in diesem Profil keine
Instruktionen aus.

`AicaExecutionController` implementiert diesen Vertrag. Der Modus
`HighLevelAudio` ist der einzige unterstuetzte Modus. Eine Anforderung von
`LowLevelArm7` scheitert vor jeder Zustandsaenderung sichtbar; es gibt weder
einen stillen Dummy-Prozessor noch eine vorgetaeuschte erfolgreiche
Firmwareausfuehrung.

Die drei AICA-Timer laufen auf explizit uebergebenen Audiozyklen. Ihre
Ueberlaeufe setzen getrennte Pending-Bits, die erst nach Aktivierung der
Interruptmaske sichtbar zugestellt und ausdruecklich quittiert werden.
Damit bleibt der HLE-Pfad deterministisch und kann spaeter an den zentralen
Phase-6-Scheduler angebunden werden.

Direkter Execution-, Register- und Schedulerreset verwenden denselben
vollstaendigen Resetvertrag. Er stellt den HLE-Modus wieder her, gibt den
ARM7-Resetzustand frei, loescht alle Timerteiler/-reste sowie Pending- und
Enablebits und verwirft das alte Audiotickereignis. Bei einem weiter lebenden
Scheduler wird danach genau ein neuer Tick geplant; alte Ereignis-IDs koennen
keinen neuen Geraetezustand mehr veraendern. Die Resetgrenze bleibt
`noexcept`: Kann der Scheduler den Ersatztick nicht aufnehmen, zeigt der
Snapshot `TickScheduleFailure` ohne lebendes Tickereignis. Ein spaeterer
erfolgreicher Schedulerreset loescht den Fehler und stellt die
Ein-Tick-Invariante wieder her.

Byte-, Halfword- und Wordwrites leiten ihre Wirkung aus den danach
rekonstruierten Registerbytes ab. Das gilt fuer Channel-Key-On/-Off,
Timerkonfiguration, Interruptenable/-quittierung und ARM7-Reset; insbesondere
wirken auch Writes auf das jeweilige Highbyte. Identische finale
Registerbelegungen erzeugen damit denselben ausgefuehrten Geraetezustand.

Ein gastseitig ausserhalb des Sound-RAM programmierter PCM16-, PCM8- oder
ADPCM-Zugriff beendet nicht den Host-Audiocallback. Nur die betroffene Voice
wird deaktiviert, waehrend die uebrigen Voices und der Ausgabepuffer
weiterlaufen. Zaehler und erster strukturierter Fehler speichern Format,
Channel, Sampleadresse und absoluten Renderframe und sind Bestandteil des
AICA-Snapshots; Reset loescht diesen Fehlerzustand. Fehlendes gemeinsames
Sound-RAM bleibt davon getrennt ein interner Lebenszyklusfehler.

## Spaeterer optionaler LLE-Pfad

Ein ARM7-LLE-Profil ist nicht Voraussetzung fuer BIOS-freien Homebrew oder
die Phase-6-Gates. Eine spaetere Implementierung braucht einen eigenen
ARM7-Interpreter oder ein kompatibles Backend, definierte AICA-RAM- und
Registerarbitrierung, Interrupt- und Resetsemantik sowie Differenztests gegen
das HLE-Profil. Nutzerbereitgestellte Firmware bleibt dabei externe,
read-only Eingabe und darf nie in Repository, Tests oder Releases gelangen.
