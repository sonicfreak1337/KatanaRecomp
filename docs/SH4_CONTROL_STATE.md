# SH-4-Kontrollzustand

KR-4611 korrigiert den gemeinsamen Kontrollzustand von Runtime, generiertem
C++ und Portdispatcher. Referenzgrundlage ist das
[Renesas SH-4A Software Manual](https://www.renesas.com/en/document/mas/sh-4a-software-manual),
insbesondere die Registerbankbeschreibung, BSR/BSRF/JSR, RTE, SLEEP sowie der
Exception- und Interrupt-Eintritt.

## Semantikvertrag

- R0 bis R7 zeigen Bank 1 genau dann, wenn `SR.MD` und `SR.RB` gesetzt sind.
  Eine RB-Aenderung im User-Modus tauscht die sichtbaren Register nicht.
- BSR, BSRF und JSR schreiben `PC + 4` nach PR, bevor der Delay Slot
  ausgefuehrt wird. Der Slot liest diesen Wert und darf ihn dauerhaft
  ueberschreiben.
- RTE restauriert SR und SPC vor seinem Delay Slot. Der Slot sieht damit die
  restaurierte Registerbank; anschliessend wird bei SPC weiterdispatcht.
- SLEEP setzt den Folge-PC, haelt aber die Blockausfuehrung an. Scheduler und
  Interruptcontroller duerfen weiter fortschreiten. Erst ein tatsaechlich
  angenommener Interrupt beendet den Schlafzustand und dispatcht den Handler.
- Normale Gast-Exceptions und Interrupts sind kein fataler Hostfehler. Ein
  aktiver Handler bleibt durch `trap_pending` sichtbar und endet mit RTE.
- Exceptionursache, EXPEVT/INTEVT und Vektor stammen aus einer gemeinsamen
  Metadatentabelle. TEA und der Delay-Slot-Owner werden am Eintritt gemeinsam
  mit SPC gesichert.

Block-ABI 2 fuehrt dafuer die Endtypen `ExceptionReturn` und `Sleep` ein.
`Exception` bezeichnet den Uebergang in einen normalen Gast-Handler;
nachfolgende Handlerbloecke behalten ihre regulaere Abschlussart. `Return`
bleibt der normale Subroutine-Return.

## Gesammelte Testanforderungen

Die Tests werden gemaess dem Gate-Arbeitsmodell erst in KR-4617 umgesetzt und
in KR-4618 mit frischen Debug- und RelWithDebInfo-Builds ausgefuehrt.

1. Eine unabhaengige Vier-Zustaende-Matrix prueft User/Privileged kombiniert
   mit RB0/RB1 sowie alle Uebergaenge und die Identitaet beider Registerbaenke.
2. BSR, BSRF und JSR werden jeweils mit `STS PR,Rn` und `LDS Rm,PR` im Delay
   Slot gegen feste Referenzvektoren verglichen; Callziel, Slotwert, PR und
   Rueckkehr-PC sind getrennte Erwartungen.
3. RTE wird mit RB-Wechsel, PR-Zugriff und einem fehlerausloesenden Delay Slot
   im Referenz-, IR-, generierten C++- und Portdispatchpfad verglichen.
4. SLEEP prueft maskierte, durch BL gesperrte und akzeptierte Interrupts. Vor
   Annahme darf kein Folgeblock laufen; nach Annahme muessen Handler, RTE und
   Folge-PC erreicht werden.
5. Eine tabellengetriebene Matrix prueft Trap, Illegal/Slot-Illegal,
   FPU/Slot-FPU, Lese-/Schreib-Speicherfehler und Interrupts gemeinsam auf
   Cause, EXPEVT/INTEVT, Vektor, TEA, SPC, SSR, SGR und Slotkontext.
6. Fehlende Handlerbloecke und erschoepfte Scheduler-/Blockbudgets muessen
   sichtbar fehlschlagen; normale Handler duerfen dagegen keinen fatalen
   Hostabbruch oder stillen Erfolg erzeugen.
7. Debug und RelWithDebInfo muessen fuer alle Vektoren bytegleiche
   Gastzustaende und dieselbe Handlerreihenfolge liefern.

## Bekannte Abgrenzung

Die genaue gemeinsame Gastzyklus- und Geraetereihenfolge folgt in KR-4616.
KR-4611 verwendet fuer den SLEEP-Wartepfad bereits Gast-Schedulerfortschritt,
definiert aber noch keine neuen Instruktionskosten oder globalen Laufbudgets.
