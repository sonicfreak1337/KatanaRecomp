# Changelog

## [Unreleased]

### Hinzugefuegt

- KR-2001: Ein konservativer Constant-Folding-Pass faltet beweisbare status- und speicherneutrale 32-Bit-Integerausdruecke mit definiertem Wraparound.

- KR-2002: Copy Propagation ersetzt lokale Registerkopien nur bis zum naechsten Schreibzugriff auf Quelle oder Ziel und verwirft Aliase bei unbekannten Effekten.

- KR-2003: Dead-Code-Elimination entfernt innerhalb eines Blocks reine Registerdefinitionen, die vor jeder Nutzung eindeutig ueberschrieben werden.

## [0.19.0] - 2026-07-14

### Hinzugefuegt

- KR-1901: Jede Katana-IR-Instruktion traegt explizite Breiten fuer semantisches Ergebnis und Eingaben, kodierte Immediate- und Displacement-Felder, Speichertransfers und effektive Adressen.

- KR-1902: Statusregister-Lese- und Schreibeffekte fuer T, S, Q, M und vollstaendige SR-Transfers sind pro IR-Instruktion explizit modelliert.

- KR-1903: Speicherzugriffsart, Transferbreite, Zugriffszahl sowie Pre-Decrement- und Post-Increment-Registerupdates sind als IR-Seiteneffekte sichtbar.

- KR-1904: Delay Slots sind in der Katana-IR als normalisierte Owner/Slot-Beziehung mit gegenseitigen Instruktionsadressen dargestellt.

- KR-1905: Ein pro Funktion nutzbarer IR-Verifier prueft Struktur, Metadaten, Register, Kontrollflussziele und Delay-Slot-Beziehungen; ungueltige IR wird vor dem C++-Codegenerator abgelehnt.

- KR-1906: Katana-IR besitzt eine vollstaendige, deterministisch sortierte Textausgabe und eine maschinenlesbare JSON-Ausgabe ueber den neuen CLI-Befehl `ir-json`.

### Behoben

- Operandbreiten fuer DIV0S und RTE, S-abhaengige MACH/MACL-Wirkungen, Full-SR-Abfragen, registerabhaengiges Post-Increment sowie die opcodebasierte Delay-Slot-Verifikation bilden die SH-4-Semantik nun widerspruchsfrei ab.

- Sicher aufgeloeste indirekte Spruenge, Calls, Overrides und vollstaendig validierte Jump Tables werden bis zum deterministischen Fixpunkt in die rekursive Codeentdeckung zurueckgefuehrt. Eine gemeinsame committed-Code-Pruefung lehnt Zero-Fill- und Segmentgrenzen einheitlich ab; unbekannte Opcodes beenden ihren Analysepfad mit sichtbarer Diagnose.

- `jump` und `jump_table` sind fuer dieselbe Dispatch-Adresse nun gegenseitig ausgeschlossen; die Parserdiagnose nennt Datei, beide betroffenen Zeilen und die Adresse.

## [0.18.0] - 2026-07-14

### Hinzugefuegt

- KR-1801: Eine konservative lokale Konstantenpropagation verfolgt Immediate-Werte, Additionen und Registerkopien mit definiertem 32-Bit-Wraparound und verwirft Annahmen bei unmodellierten Effekten.

- KR-1802: Eine adressbezogene Registerwertanalyse erweitert sichere arithmetische und logische Transfers und zeichnet beweisbare oder explizit unbekannte Registerwerte an indirekten `JMP`-/`JSR`-Stellen auf.

- KR-1803: Einfache indirekte Calls und Spruenge werden nur bei beweisbaren konstanten Registerwerten in committed ausfuehrbaren Code aufgeloest; unbekannte oder ungueltige Ziele bleiben mit getrennten Gruenden sichtbar.

- KR-1804: Eine beschraenkte Jump-Table-Analyse validiert bekannte absolute 32-Bit-Tabellen vollstaendig und weist fehlende, ungerade oder nicht ausfuehrbare Ziele explizit zurueck.

- KR-1805: Eine strikt versionierte Override-Datei nimmt deterministisch sortierte Funktions-, Sprung- und Jump-Table-Hinweise auf und diagnostiziert unbekannte oder doppelte Angaben.

- KR-1806: Der Kontrollflussbericht trennt sichere und ungeloeste Ziele, nennt stabile Gruende und liefert passende Override-Hinweise fuer jede offene Stelle.

## [0.17.0] - 2026-07-14

### Behoben

- ELF32-SH-Einstiegspunkte werden nur noch uebernommen, wenn sie zwei Byte ausgerichtet sind und innerhalb committed Daten eines ausfuehrbaren Code-Segments liegen; Zero-Fill- und Segmentgrenzen werden diagnostisch abgelehnt.

### Hinzugefuegt

- KR-1701: Eine deterministische Worklist verfolgt bekannten Code ab den Einstiegspunkten eines Executable Images ueber direkte Spruenge, Calls, Fallthrough und Delay Slots, ohne nicht erreichbare Segmentbytes linear zu dekodieren.

- KR-1702: Rekursive Analyseergebnisse klassifizieren normalisierte Adressbereiche als erreichbaren Code, deklarierte Daten oder unbekannte Bytes; nicht erreichter Code und Zero-Fill bleiben sichtbar unbekannt.

- KR-1703: Funktionskandidaten tragen zusammengefuehrte Herkunftsevidenz aus Image-Einstiegspunkten, direkten Calls und Funktionssymbolen sowie eine deterministische Konfidenzstufe.

- KR-1704: Nicht durch die Worklist erreichte committed Bytes aus ausfuehrbaren Code-Segmenten werden separat als unerreichbare Codebereiche ausgewiesen, ohne Zero-Fill oder Datensegmente einzubeziehen.

- KR-1705: Mehrdeutige Adressen, die zugleich Funktionskandidat und Delay Slot sind, werden als deterministische Analysekonflikte ausgewiesen statt doppelt oder stillschweigend interpretiert.

- KR-1706: Ein deterministischer rekursiver Analysebericht erklaert Funktionsherkunft und Konfidenz sowie Code-/Datenbereiche, unerreichbaren Code und Analysekonflikte mit stabilen Adressen und Gruenden.

### Geaendert

- KR-1707: Basic-Block-, Funktions-, IR- und Codegen-Pfade verwenden die rekursive Worklist; `disasm` bleibt als linearer Diagnosemodus erhalten und `analyze <Manifest>` gibt den begruendeten Bericht aus. Version und Release-Dokumentation wurden auf v0.17.0 aktualisiert.

## [0.16.0] - 2026-07-14

### Hinzugefuegt

- KR-1601: Ein neutrales Executable-Image- und Segmentmodell bildet virtuelle Adressen, Dateioffsets, Speicher- und Dateigroessen, Code-/Datenklassifikation, Berechtigungen und Einstiegspunkte mit validierten 32-Bit-Adressbereichen ab.

- KR-1602: Ein konfigurierbarer Raw-Binary-Loader bildet Dateien mit Basisadresse, Segmentklasse, Berechtigungen und optionalem Einstiegspunkt auf das Executable-Image-Modell ab und meldet Pfad-, Offset- und Adressraumfehler sichtbar.

- KR-1603: Ein validierender Little-Endian-ELF32-SH-Loader uebernimmt `PT_LOAD`-Segmente, Einstiegspunkt, Datei-/Speichergroessen und `PF_R/PF_W/PF_X` in das neutrale Image-Modell und meldet Loaderfehler mit Datei, Offset und Ursache.

- KR-1604: Executable Images speichern deterministisch sortierte Funktions- und Objektsymbole; ELF32-`SHT_SYMTAB`/`SHT_DYNSYM` sowie optionale Katana-Map-Dateien werden mit Bindung, Groesse und diagnostischen Datei-/Zeilenfehlern geladen.

- KR-1605: ELF32-SH-`SHT_REL`-Eintraege werden im Image-Modell sichtbar; `R_SH_DIR32` und `R_SH_REL32` werden mit implizitem Addend angewendet, waehrend unbekannte Typen unveraendert als nicht unterstuetzt erhalten bleiben.

- KR-1606: Ein strikt versioniertes Projektmanifest v1 waehlt Raw- oder ELF32-SH-Eingaben, beschreibt Raw-Adresslayout und Berechtigungen, loest relative Eingabe- und Map-Pfade am Manifest auf und weist unbekannte oder doppelte Felder diagnostisch zurueck.

### Geaendert

- KR-1607: Der normale CLI-Analyse- und Codegen-Pfad laedt Raw-Binaries als Executable Images; der Analyzer verarbeitet nur ausfuehrbare Code-Segmente. Version, Roadmap, Status und Release-Dokumentation wurden auf v0.16.0 und den naechsten Phase-2-Meilenstein aktualisiert.

## [0.15.0] - 2026-07-14

### Hinzugefuegt

- KR-1501: Zentrale, unveraenderliche Instruktionsmetadaten fuer Opcode-Masken, Operandenformate, Kontrollfluss, Delay Slots und Privilegstatus eingefuehrt; Systemregisterkodierungen verwenden dieselbe Quelle im Decoder und in Tests.

- KR-1502: Eine vollstaendige paarweise Decoder-Kollisionspruefung erkennt auch Teilmengen ueber unterschiedlich breite Opcode-Masken und sichert die aktuelle Regeltabelle als mehrdeutigkeitsfrei ab.

- KR-1503: `katana-recomp isa-report` zaehlt deterministisch den gesamten 16-Bit-Opcode-Raum und listet jede implementierte Instruktionsart mit Regel-, Opcode- und Privileginformationen auf.

- KR-1504: Manuell aus dem offiziellen Renesas-SH-4-Handbuch abgeleitete, von der Metadatentabelle unabhaengige Decodervektoren pruefen Format-, Grenz-, Privileg- und Unknown-Faelle.

- KR-1505: Ein reproduzierbarer korpusbasierter Decoder-Mutationsfuzzer prueft Determinismus, eindeutige Metadatenzuordnung, Unknown-Verhalten, Operandenbereiche und Disassembly mit festem Seed.

### Geaendert

- KR-1506: Alle normalen Decoderbedingungen beziehen Opcode-Masken und Muster aus der zentralen Metadatenquelle; Version, Roadmap, Status und Release-Dokumentation wurden auf v0.15.0 und den Beginn von Phase 2 aktualisiert.

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
