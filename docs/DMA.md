# Deterministischer SH-4-DMAC

KR-3103 bildet die vier Kanaele des SH7750-DMAC auf dem zentralen
Gastzyklus-Scheduler ab. Grundlage sind SAR, DAR, DMATCR, CHCR und DMAOR aus
Abschnitt 14 des Renesas SH7750/SH7750S/SH7750R Hardware-Handbuchs.

## Runtimevertrag

- Jeder Transfer wird als explizites Scheduler-Ereignis ausgefuehrt. Die
  konfigurierbare Frist ist `guest_cycles_per_byte * transfer_size` und nutzt
  keine Hostzeit.
- Byte-, 16-Bit-, 32-Bit-, 64-Bit- und 32-Byte-Transfers sowie feste,
  inkrementierende und dekrementierende Quell-/Zieladressen werden abgebildet.
- DMATCR wird nach jeder Einheit sichtbar heruntergezaehlt. Der Abschluss setzt
  CHCR.TE und bei CHCR.IE einen abfragbaren Interruptzustand.
- DMAOR.DME, AE und NMIF stoppen alle Kanaele. Ausrichtungs-, Bereichs- und
  verbotene Modusfehler setzen AE, behalten SAR/DAR/DMATCR fuer die naechste
  Einheit bei und erzeugen keine vorgetaeuschte TE-Fertigstellung.
- Auto-Requests starten mit DE und DME; externe und Modulrequests benoetigen
  `request_transfer()`. Gleichzeitige Kanaele folgen dem DMAOR-Prioritaetsmodus.
- `DMAOR.DDT` wechselt ohne Hostsonderfall in den On-Demand-Modus. Externe
  DDT-Anforderungen verwenden `request_on_demand_transfer()`; Kanal 0 besitzt
  genau einen laufenden Slot, Kanaele 1 bis 3 jeweils die dokumentierte
  Viererqueue. `repeat_on_demand_transfer()` bildet den TR-only-Handshake fuer
  den zuletzt angenommenen Kanal ab. NMI, AE, Reset und das Verlassen des
  DDT-Modus verwerfen die externen DDT-Queues deterministisch.
- Das 32-Bit-Registerfenster ist unter `0xFFA00000` und `0x1FA00000` erreichbar.
  Schmalere Registerzugriffe scheitern sichtbar.

## Dreamcast-Channel-2-TA-Vertrag

Der produktive TA-Pfad verwendet auf SH-4-DMAC-Kanal 2 den externen
Memory-to-Device-Request `CHCR.RS=2`. Er akzeptiert nur 32-Byte-Einheiten,
inkrementierende Quelle, festes Geraeteziel, Burstmodus, gesetztes `DE` sowie
`DMAOR.DME+DDT`. Die Quelle muss aus einem der vier physischen Area-3-Haupt-RAM-
Spiegel `0x0C` bis `0x0F` stammen; Transferadresse und Laenge muessen 32-Byte-
ausgerichtet sein und duerfen Area 3 nicht verlassen. Andere Richtung,
Cycle-Steal oder ein anderer Requesttyp werden vor einer Scheinfertigstellung
sichtbar abgelehnt.

Der Systembus setzt fuer `SB_C2DSTAT` die feste Area-4-Zielregion und fuehrt den
Transfer bis zum gemeinsamen TA-FIFO. Eine Runtime-End-to-End-Regression
beweist RAM -> SH-4-DMAC -> Channel-2-Systembus -> TA-Object-List/EOL samt
Residue, `TE` und getrenntem ASIC-Channel-2-Ereignis. Die Direct-Texture-
Zielbereiche `0x11`/`0x13` brauchen fuer Transfers ueber mehr als eine Einheit
noch einen eigenen fortschreitenden Zielvertrag und bleiben als generische
P1-Luecke offen.

Nicht Teil des Hostvertrags sind elektrische DREQ-/DACK-/DBREQ-/BAVL-/TR-Pins
und das Einlesen eines 64-Bit-DTR-Worts von einem physischen Datenbus. Ein
angebundenes natives Geraet reicht die daraus dekodierte Kanal-Anforderung ueber
die DDT-Schnittstelle ein; ein bloss gesetztes DDT-Bit erfindet keinen Transfer.
MMU-/Cache-Kohaerenz wird durch die gemeinsamen Memory-/Codewrite-Pfade
abgedeckt. DMTE/DMAE werden ueber den zentralen Plattformrouter zugestellt.

Primaerreferenz:

- Renesas, *SH7750, SH7750S, SH7750R Group User's Manual: Hardware*, Abschnitt
  14, Direct Memory Access Controller (DMAC), Rev. 7.02, 2013:
  <https://www.renesas.com/en/document/mah/sh7750-sh7750s-sh7750r-group-users-manual-hardware>
- KallistiOS, oeffentlicher SH-4-DMAC-Vertrag (Channel 2 fuer PVR,
  External-Memory-to-Device `RS=2`, 32-Byte-Einheiten und Burstmodus):
  <https://kos-docs.dreamcast.wiki/group__dmac.html>
- KallistiOS, oeffentliche PVR-DMA-Schnittstelle als unabhaengiger
  Plausibilitaetscheck der TA/PVR-Grenze:
  <https://kos-docs.dreamcast.wiki/pvr__dma_8h.html>

KatanaRecomp uebernimmt keinen KallistiOS-Code oder dessen interne Struktur;
die sichtbaren Bedingungen sind als eigenstaendiger Runtimevertrag mit
synthetischen Tests formuliert.
