# Port-Runtime-Vertrauensvertrag

Der mit KR-4508 eingefuehrte Portprojektvertrag Version 3 trennt
Analyseerfolg, Eingabeidentitaet und tatsaechliche Gastausfuehrung. Der
aktuelle kumulative Stand verwendet Portprojektvertrag 24, Runtime-ABI 40,
Block-ABI 3 und Backend-Interface-ABI 3. Keine der Vertrauensaussagen wird aus
der blossen Erzeugung oder dem Start eines Hostprozesses abgeleitet.

## Eingabeidentitaet

Das erzeugte Programm enthaelt SHA-256-Erwartungswerte fuer den
GDI-Descriptor, jeden referenzierten Track und die bei der Analyse extrahierte
Bootdatei. Beim Start wird dieselbe GDI erneut geladen und vollstaendig
gehasht. Jede Abweichung oder ein Ladefehler endet vor der ersten
Gastinstruktion stabil mit `source-identity-mismatch`.

Absolute lokale Quellpfade gehoeren nicht zum Vertrag und werden nicht in den
Port geschrieben. Laufzeitdiagnosen redigieren nur belastbare kanonische
absolute Pfade; relative Komponenten wie `.` oder `..` sind keine globalen
Ersetzungsmuster.

## Ausfuehrungsnachweis

Jeder IR-Basic-Block wird mit seiner echten Startadresse, Bytegroesse und
Abschlussart in der Runtime-Blocktabelle registriert. Ein Wrapper fuehrt genau
einen Block aus und liefert `Fallthrough`, statischen oder dynamischen Sprung,
Call, Return, Exception oder Interrupt-Safepoint an den zentralen Dispatcher
zurueck.

`KR_GUEST_PROGRAM_ENTERED` und `silent_failures=0` sind nur erlaubt, nachdem mindestens
ein Block jenseits des initialen Entry-Blocks einen Gast-Checkpoint erreicht
hat. Ein sofortiges `TRAPA`, eine Exception, ein kontrollierter Fallback, ein
erschoepftes Schedulerbudget oder fehlender Fortschritt ist ein sichtbarer
Fehler. Die synthetische Trap-Fixture prueft explizit, dass beide
Erfolgsmeldungen ausbleiben.

## Plattformdienste

PlatformServices-ABI 4 fuehrte diese Bindung an die vorhandenen
Dreamcast-Runtimekomponenten ein; der aktuelle kumulative Vertrag steht auf
PlatformServices-ABI 9:

- Interrupts werden ueber den Interrupt-Router angenommen.
- DMA wird ueber den SH-4-DMAC geplant und bleibt bis zur Schedulerausfuehrung
  asynchron.
- `PREF` uebertraegt echte Store-Queue-Inhalte unter Beruecksichtigung von
  QACR0/QACR1 und invalidiert ueberdeckten generierten Code.
- Gastschreibzugriffe und registrierte Codebereiche laufen durch den
  Executable-Code-Tracker; der Port bindet seine lokale Blocktabelle an dessen
  Gueltigkeitszustand.

Bewachte Kontrollflusskandidaten sind analysierbar und kompilierbar, aber kein
statischer Erreichbarkeitsbeweis. Der Bericht weist `resolved`, `guarded` und
`unresolved` getrennt aus; der Port behaelt fuer Guarded-Stellen den
dynamischen Default. Nur Stellen ohne endliche Kandidaten blockieren die
Vollstaendigkeit.

## Konsistenzgrenzen

`BRAF` und `BSRF` berechnen relative Ziele in Analyse und Backend modulo
2 hoch 32. Der ISA-Bericht misst pro Instruktion Decoder-, IR-, Backend- und
Runtimeunterstuetzung; der oeffentliche Status ist ausschliesslich deren
Schnittmenge. Eine absichtlich abgelehnte Schicht muss deshalb den
Gesamtstatus ablehnen.

Der reproduzierbare Nachweis besteht aus den CTests fuer Wertanalyse,
ISA-Abdeckung, Portexport, Port-CLI, Application und PlatformServices sowie dem
vollstaendigen CTest-Zwischengate der x64-Debug-Desktop-GUI-off-Konfiguration
mit ASan, konfiguriertem MSVC-Coverage-Backend und `/analyze /WX`. Es besteht
183/183 Eintraege in 312,97 Sekunden, darunter 181 regulaere Passes und zwei
erwartete Regex-Erfolge. Ein Coverage-Bericht wurde in diesem direkten CTest-
Lauf nicht erhoben; Desktop-GUI- und Harness-Tests sind nicht Teil der 183.
Alle Fixtures sind synthetisch; Spielinhalte und private Adressen sind nicht
Bestandteil des Repository. Das Zwischengate ist kein Abschluss von `KR-4852`,
`KR-4853` oder `KR-4854`.
