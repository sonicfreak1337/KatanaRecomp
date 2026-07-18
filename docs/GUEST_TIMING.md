# Einheitlicher Gastzeitvertrag

Dieses Dokument beschreibt Gastzeitvertrag 1 aus `KR-4616`. Die ausfuehrbaren
Cross-Engine- und Reihenfolgeregressionen werden gemaess
`docs/CODEX_HANDOFF.md` bei `KR-4617` implementiert und im frischen Gate-Build
von `KR-4618` ausgefuehrt.

## Eine Uhr

`EventScheduler::current_cycle()` ist die einzige semantische Gastzeitquelle.
Sie ist eine monotone 64-Bit-Zahl ohne Hostzeit, Sleep oder Hintergrundthread.
Gastzeitvertrag 1 ist ueber `guest_cycle_contract_version` versioniert und wird
zusammen mit PlatformServices-ABI 5 validiert.

Der generierte Backendpfad reicht relative Instruktionskosten mit
`consume_guest_cycles` ein. Die Basiskosten betragen in Vertrag 1 einen
Gastzyklus pro unterschiedlicher Gastinstruktionsadresse im IR-Block;
gepaarte Delay Slots zaehlen damit genau einmal als eigene Instruktion. Ein
Basic Block ist der Interrupt-Safepoint. Der Fallbackpfad verwendet dieselbe
Uhr ueber `SchedulerSafepoints`; Blockaustritt, Replay und Runtimebericht lesen
den danach erreichten Schedulerzyklus.

TMU, RTC und DMA planen ihre Fristen direkt auf dem Scheduler. Die
Dreamcast-Runtime verwendet die gemeinsame 200-MHz-Gastfrequenz, den
SH-4-Peripherieteiler und die bestehenden DMA-Kosten statt beschleunigter
lokaler Testuhren.

## GD-ROM und PVR

`GdRomAsyncReader` besitzt keinen veraenderbaren `current_cycle_` und keine
oeffentliche `advance_to`-Grenze mehr. `submit` berechnet seine Frist aus dem
aktuellen Schedulerzyklus und plant genau ein Completionereignis. Die Antwort
wird vor dem Completionobserver sichtbar; ein dort geplantes ASIC-Ereignis
fuer denselben Zyklus folgt aufgrund seiner hoeheren Scheduler-ID danach.

Ein PVR-`StartRender` plant ebenfalls ein Completionereignis. Erst dessen
Callback erhoeht den Completionzaehler und meldet `PvrRenderDone` an den
System-ASIC. Softreset, Destruktion und Schedulerreset koennen keine alten
Callbacks auf neue Geraetezustaende anwenden.

## Budget und SLEEP

Ein optionales Schedulerbudget ist ein absoluter maximaler Gastzyklus. Ein
Advance ueber diese Grenze verarbeitet nur Ereignisse bis zur Grenze, setzt
die Uhr exakt dorthin und meldet `GuestCycleBudgetExhausted`. Das getrennte
Ereignisbudget bleibt erhalten und meldet weiterhin
`EventBudgetExhausted` am letzten verarbeiteten Callback.

Der Port liest `KATANA_GUEST_CYCLE_BUDGET` genau einmal beim Aufbau seiner
PlatformServices. Nur eine nichtleere positive dezimale 64-Bit-Ganzzahl ist
gueltig. Der gemeldete Verbrauch ist immer der Schedulerstand und niemals die
konfigurierte Obergrenze.

SLEEP prueft zuerst einen bereits annehmbaren Interrupt. Andernfalls wird bis
zum naechsten geplanten Schedulerereignis fortgeschaltet und danach erneut
geprueft. Eine fehlende Wakeupquelle, ein Ereignisbudgetende oder das
Gastzyklusbudgetende sind verschiedene sichtbare Fehler.

## Nachzuholende Regressionen

`KR-4617` muss mindestens folgende unabhaengige Vektoren implementieren:

1. Zwei identische Laeufe mit gleichzeitigen TMU-, DMA-, GD-ROM-, PVR- und
   Interruptfristen liefern dieselben Zyklen, Scheduler-IDs und sichtbaren
   Zustandsuebergaenge.
2. Eine unabhaengig berechnete Fristentabelle prueft DMA-Einheiten, TMU-
   Unterlauf, RTC-Phase und Interruptannahme; keine Produktfunktion dient als
   Orakel.
3. Ein GD-ROM-Request wird allein durch `scheduler.advance_to` fertig. Es gibt
   keinen zweiten Geraete-Clock-Aufruf, und Antwort, Observer sowie ASIC folgen
   in der festgelegten Reihenfolge.
4. PVR-Start ist vor seiner Frist nicht abgeschlossen, meldet exakt einmal und
   hinterlaesst nach Soft- oder Schedulerreset keinen stale Callback.
5. Generierter und Fallbackpfad verbrauchen fuer dieselbe Instruktionsfolge
   dieselben Gastzyklen; gepaarte Delay Slots werden genau einmal berechnet.
6. Budgets unter, auf und ueber einer Ereignisfrist pruefen exaktes Kappen,
   getrennte Statuswerte und den gemeldeten `guest_cycles`-Wert. Null,
   Vorzeichen, Leerraum, Text und 64-Bit-Ueberlauf werden abgewiesen.
7. SLEEP deckt bereits pending, spaeter pending, maskiert, ohne Wakeupquelle,
   Ereignisbudgetende und Gastbudgetende ab.
8. Schedulerreset prueft deterministische Event-IDs sowie die Reaktivierung
   laufender TMU-/RTC-/DMA-Fristen und das Verwerfen alter GD-ROM-/PVR-Requests.

`KR-4618` erstellt danach den einzigen frischen Debug- und
RelWithDebInfo-Gate-Build und fuehrt diese Vektoren mit der vollstaendigen
Core-Suite aus.
