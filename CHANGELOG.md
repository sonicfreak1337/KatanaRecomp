# Changelog

## [Unreleased]

### Hinzugefuegt

- KR-1401: Byte-, Word- und Long-Formen von `MOV Rm,@-Rn` und `MOV @Rm+,Rn` mit korrekter Registerreihenfolge, Vorzeichenerweiterung und identischen Registerpaaren implementiert.

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
