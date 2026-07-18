# Store Queues und Cacheadressierung

KR-4612 korrigiert die SH-4-P4-Adressierung fuer Store Queues und grenzt die
modellierten Cacheoperationen von nicht modellierten Cachezustaenden ab.
Referenzgrundlage ist das
[Renesas SH-4A Software Manual](https://www.renesas.com/en/document/mas/sh-4a-software-manual),
insbesondere die Abschnitte zu P4, Store Queues, QACR, PREF und
speicherabgebildeten Cachearrays.

## Store-Queue-Vertrag

- Adressbit 5 waehlt SQ0 oder SQ1. Bits 25 bis 6 gehoeren zur spaeteren
  Transferzielbildung und duerfen die Queue nicht waehlen.
- Stores in `0xE0000000` bis `0xE3FFFFFF` schreiben Byte, Word oder Longword
  little-endian in die ausgewaehlte 32-Byte-Queue. Natuerliche Ausrichtung und
  die Queuegrenze werden vor der ersten Aenderung geprueft.
- Das privilegierte Lesefenster `0xFF001000` bis `0xFF00103F` akzeptiert nur
  ausgerichtete Longwords. Bit 5 waehlt auch dort die Queue.
- PREF richtet die Quelladresse auf 32 Byte aus, uebertraegt exakt die
  ausgewaehlte Queue und bildet das physische Ziel aus Adressbits 25 bis 5 und
  QACR[4:2]. Bit 5 bleibt zugleich physisches Zielbit 5.
- QACR0 und QACR1 akzeptieren ausschliesslich ausgerichtete 32-Bit-Zugriffe.
  Reservierte Schreibbits werden sichtbar abgelehnt.

## Cachevertrag

Der aktuelle Runtimepfad modelliert keinen vollstaendigen Operand-Cache mit
Tags, Valid-, Dirty- und Write-back-Zustand. Deshalb gilt:

- ICBI invalidiert die zur Adresse gehoerende ausgerichtete 32-Byte-Zeile im
  generierten Code-Tracker.
- MOVCA.L schreibt das Longword ueber den normalen Speicherpfad und meldet eine
  tatsaechliche Codeinvalidierung nur bei einem ueberdeckten Block.
- OCBI wird ohne Cachetagmodell weiterhin sichtbar als nicht modelliert
  abgelehnt.
- OCBP und OCBWB laufen ueber einen expliziten kohaerenten
  Operand-Cache-Vertrag. Da der Referenzpfad Stores direkt ueber `Memory`
  sichtbar macht und keine verborgenen Dirty-Lines besitzt, melden beide
  Operationen deterministisch `dirty_line_present=false` und
  `wrote_memory=false`. Das ist ein versionierbarer Cachevertrag und kein
  fehlender Backendpfad; ein spaeteres Cachemodell kann ihn ersetzen.
- Der explizit aktivierte Operand-Cache-RAM-Modus besitzt Byte-, Word- und
  Longwordzugriffe mit Little-Endian-, Ausrichtungs- und Grenzvertrag. Ein
  Profil ohne dieses Modell lehnt die Aktivierung ab.

## Gesammelte Testanforderungen

Die Regressionen werden in KR-4617 implementiert und im Gate KR-4618 gebaut
und ausgefuehrt.

1. `0xE0000000` und `0xE0000020` fuellen unterschiedliche Queues; Adressen,
   die sich nur in Bit 25 unterscheiden, waehlen dieselbe Queue.
2. Byte-, Word- und Longword-Stores pruefen Little Endian, Minimal-/Maximal-
   Offsets, Ausrichtung und atomare Ablehnung eines Queueueberlaufs.
3. Das P4-Lesefenster akzeptiert nur acht ausgerichtete Longwords je Queue;
   Reads aus dem Schreibfenster sowie Writes ins Lesefenster schlagen fehl.
4. QACR0/QACR1 und PREF pruefen getrennte RAM-/TA-Ziele, Zielbit 5, exakte
   32-Byte-Inhalte und identische Bytes im Sink- und direkten Speicherpfad.
5. ICBI prueft nicht ausgerichtete Eingaben gegen dieselbe 32-Byte-Codezeile;
   MOVCA.L prueft Speicherbytes und nur tatsaechliche Ueberdeckung als
   Invalidierung.
6. OCBI muss ohne Cachetag-/Dirty-Modell stabil und sichtbar fehlschlagen;
   OCBP und OCBWB muessen Decoder, IR, Backend und den expliziten kohaerenten
   Runtimevertrag erreichen, ohne Dirty-Lines oder Writes zu erfinden.
7. Operand-Cache-RAM prueft alle drei Breiten, beide Enden des 8-KiB-Bereichs,
   Fehlausrichtung, Ueberlauf und deaktivierte Profile.

Die Store-Queue-Transfers bleiben vor KR-4613 noch Nutzer des bestehenden
Code-Trackers. Der einheitliche Gastwrite- und Invalidierungsvertrag folgt im
naechsten Task.
