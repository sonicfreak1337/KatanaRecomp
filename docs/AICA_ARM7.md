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

## Spaeterer optionaler LLE-Pfad

Ein ARM7-LLE-Profil ist nicht Voraussetzung fuer BIOS-freien Homebrew oder
die Phase-6-Gates. Eine spaetere Implementierung braucht einen eigenen
ARM7-Interpreter oder ein kompatibles Backend, definierte AICA-RAM- und
Registerarbitrierung, Interrupt- und Resetsemantik sowie Differenztests gegen
das HLE-Profil. Nutzerbereitgestellte Firmware bleibt dabei externe,
read-only Eingabe und darf nie in Repository, Tests oder Releases gelangen.
