# Changelog

## [Unreleased]

### Hinzugefuegt

- KR-1501: Zentrale, unveraenderliche Instruktionsmetadaten fuer Opcode-Masken, Operandenformate, Kontrollfluss, Delay Slots und Privilegstatus eingefuehrt; Systemregisterkodierungen verwenden dieselbe Quelle im Decoder und in Tests.

- KR-1502: Eine vollstaendige paarweise Decoder-Kollisionspruefung erkennt auch Teilmengen ueber unterschiedlich breite Opcode-Masken und sichert die aktuelle Regeltabelle als mehrdeutigkeitsfrei ab.

- KR-1503: `katana-recomp isa-report` zaehlt deterministisch den gesamten 16-Bit-Opcode-Raum und listet jede implementierte Instruktionsart mit Regel-, Opcode- und Privileginformationen auf.

- KR-1504: Manuell aus dem offiziellen Renesas-SH-4-Handbuch abgeleitete, von der Metadatentabelle unabhaengige Decodervektoren pruefen Format-, Grenz-, Privileg- und Unknown-Faelle.

## [0.14.0] - 2026-07-14

### Hinzugefuegt

- KR-1401: Byte-, Word- und Long-Formen von `MOV Rm,@-Rn` und `MOV @Rm+,Rn` mit korrekter Registerreihenfolge, Vorzeichenerweiterung und identischen Registerpaaren implementiert.

- KR-1402: Register-Displacement-Formen von `MOV` mit unsigned 4-Bit-Displacement, breitenabhaengiger Skalierung, R0-Sonderformen und definiertem Adress-Wraparound implementiert.

- KR-1403: Byte-, Word- und Long-Formen der R0-indexierten `MOV`-Adressierung mit definiertem 32-Bit-Wraparound und R0-Ueberlappungsfaellen implementiert.

- KR-1404: GBR-relative Byte-, Word- und Long-Formen von `MOV` mit explizitem GBR im generierten CPU-Zustand, 8-Bit-Displacement und breitenabhaengiger Skalierung implementiert.

- KR-1405: PC-relative `MOV.W`- und `MOV.L`-Loads sowie `MOVA` mit korrekter PC-Ausrichtung, Displacement-Skalierung und Vorzeichenerweiterung implementiert.

- KR-1406: Direkte und speicherbasierte `STS`, `LDS`, `STC` und `LDC` fuer System-, Kontroll- und Bankregister mit SR/FPSCR-Maskierung und expliziter Privilegmarkierung implementiert.

- KR-1407: `TRAPA`, `RTE` und `SLEEP` mit sichtbarem Trap- und Schlafzustand, SH-4-Ausnahmeregistern, Registerbankwechsel und korrekter RTE-Delay-Slot-Reihenfolge implementiert.

### Geaendert

- KR-1408: Die End-to-End-Tests fuer KR-1402 bis KR-1405 verwenden committed Binaer-Fixtures und den vollstaendigen Pfad ueber Binary Reader und `katana-recomp emit-cpp`; synthetische KR-1405-Grenzfaelle sind separat gekennzeichnet.

## [0.13.0] - 2026-07-14

### Hinzugefuegt

- KR-1304: `DIV0U`, `DIV0S` und `DIV1` mit expliziten Q-, M- und T-Bits sowie bitgenauen Carry-/Borrow-Referenzvektoren implementiert.

- KR-1303: `MAC.W`, `MAC.L`, `SETS` und `CLRS` mit Post-Inkrement, identischen Adressregistern sowie 32- und 48-Bit-Saettigung implementiert.

- KR-1302: `DMULS.L` und `DMULU.L` sowie das generierte CPU-Register `MACH` mit portabler signed und unsigned 64-Bit-Produktsemantik implementiert.

- KR-1301: `MUL.L`, `MULS.W` und `MULU.W` sowie das generierte CPU-Register `MACL` mit portabler 16- und 32-Bit-Produktsemantik implementiert.

## [0.12.0] - 2026-07-13

### Hinzugefuegt





- KR-1204: `SHAD` und `SHLD` mit positiven, negativen und grossen Shiftzaehlern sowie dokumentierten Sonderfaellen fuer negative Vielfache von 32 implementiert.

- KR-1203: `ROTL`, `ROTR`, `ROTCL` und `ROTCR` mit bitgenauer Register- und T-Bit-Semantik implementiert.

- KR-1202: `SHLL2`, `SHLL8`, `SHLL16`, `SHLR2`, `SHLR8` und `SHLR16` mit plattformunabhaengiger 32-Bit-Semantik und unveraendertem T-Bit implementiert.
- KR-1201: `SHLL`, `SHLR`, `SHAL` und `SHAR` mit bitgenauer T-Bit-Semantik durch Decoder, Katana-IR, C++-Codegenerator und End-to-End-Tests implementiert.

## [0.11.0] - 2026-07-13

### Hinzugefuegt


- KR-1106: `DT Rn` und `MOVT Rn` mit 32-Bit-Wraparound, T-Bit-Semantik und End-to-End-Tests implementiert.

- KR-1105: `EXTU.B`, `EXTU.W`, `EXTS.B`, `EXTS.W`, `SWAP.B`, `SWAP.W` und `XTRCT` mit bitgenauen End-to-End-Tests implementiert.

- KR-1104: `ADDC`, `SUBC`, `NEGC`, `ADDV` und `SUBV` mit dokumentierter Carry-, Borrow- und Overflow-Semantik implementiert.

- KR-1103: `CMP/HS`, `CMP/GE`, `CMP/HI`, `CMP/GT`, `CMP/PZ`, `CMP/PL` und `CMP/STR` durch Decoder, Katana-IR, C++-Codegenerator und End-to-End-Tests implementiert.

- KR-1102: Register- und Immediate-Formen von `AND`, `OR` und `XOR` durch Decoder, Katana-IR, C++-Codegenerator und End-to-End-Tests implementiert.

- KR-1101: `SUB Rm,Rn`, `NEG Rm,Rn` und `NOT Rm,Rn` durch Decoder, Katana-IR, C++-Codegenerator und End-to-End-Tests implementiert.
