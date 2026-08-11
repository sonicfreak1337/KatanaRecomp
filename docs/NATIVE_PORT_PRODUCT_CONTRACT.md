# Verbindlicher Native-Port-Produktvertrag

Status: verbindliche Architekturentscheidung ab 11. August 2026.

Dieses Dokument hat fuer den Produktpfad Vorrang vor aelteren Bring-up-,
RuntimeOnly-, AICA-, PVR-, Performance- und Handoff-Beschreibungen. Alte
Messungen bleiben historische Evidenz, definieren aber nicht mehr die
Zielarchitektur.

## Entscheidung

KatanaRecomp baut native PC-Ports. Das ausgelieferte Spiel ist kein Emulator
und enthaelt keinen emulierten Dreamcast als Laufzeitumgebung.

```text
statisch analysierter SH-4-Spielcode
  -> statisch erzeugtes natives C++
  -> validierte native Spiel-/SDK-Hooks
  -> native PC-Grafik, -Audio, -Dateien, -Eingabe und -Speicherstaende
  -> natives Spielbinary
```

Der SH-4-Spielcode wird vor dem Build statisch rekompiliert und danach direkt
auf der Host-CPU ausgefuehrt. Plattform- und Middlewaregrenzen werden an der
hoechsten sicher identifizierten Schnittstelle auf native Hostdienste
abgebildet. Ein allgemeiner Dreamcast-Geraeteapparat ist kein Bestandteil des
Produktpfads.

## Im Produktpfad verboten

- interpretierte Gast-CPUs, insbesondere ein AICA-ARM7-Interpreter;
- JIT, Runtime-Dekodierung oder ein Interpreterfallback fuer SH-4-Code;
- ein CPU-PVR-Softwarerasterizer oder ein emulierter PVR als Produktrenderer;
- zyklusweises Ausfuehren von Gast-Geraetefirmware;
- ein vollstaendiger emulierter ASIC-/AICA-/PVR-/GD-ROM-/Maple-Geraeteverbund;
- erfundene Completion-Flags, automatische Bildwechsel, Movie-Skips,
  vorgerenderte Ersatzframes oder titelbezogene Adresshacks im Katana-Kern;
- ein stiller Rueckfall auf historische Geraetemodelle, wenn ein nativer Hook
  fehlt.

Fehlt eine erforderliche native Bindung, endet der Produktbuild oder der
Produktlauf typisiert und fail-closed. Er wird nicht durch Emulation gerettet.

## Native Zielschichten

### CPU

- SH-4-Spielcode wird statisch in C++ beziehungsweise nativen Hostcode
  ueberfuehrt.
- Statisch bekannte Kanten werden direkt verkettet; zentrale Dispatchgrenzen
  bleiben nur an wirklich dynamischem Kontrollfluss und nativen Hookgrenzen.
- Die Adress-/Funktionskarte des privaten Spielprojekts dient der statischen
  Bindung, nicht einer Laufzeitinterpretation.

### Grafik

- Grafikarbeit wird an der hoechsten belegten NINJA-/Kamui-/Render- oder
  notfalls TA-Uebergabe abgefangen und in eine native GPU-API uebersetzt.
- Transformation, Rasterisierung, Texturierung, Blending und Present laufen
  auf der PC-GPU.
- Ein CPU-PVR darf nur noch in einem getrennten Diagnose-/Referenzprofil
  existieren und darf nicht in ein Produktbinary gelinkt werden.

### Audio und Movie

- mwSnd-/CRI-/ADXT-/Sofdec-Aufrufe werden vor dem AICA-Kommandoring auf eine
  native Audio-/Movie-Implementierung abgebildet.
- Decoding, Mixing, Pufferung und Ausgabe verwenden native Hostbibliotheken
  und das PC-Audiogeraet. Normale CPU-Decodierung oder -Mischung eines PC-
  Ports ist erlaubt; AICA-Instruktions- oder Geraeteemulation ist es nicht.
- Das Opening bleibt vollstaendig und ungeskippt. Status, Callbacks und
  Lebenszyklus werden aus echter nativer Verarbeitung erzeugt, nicht direkt
  gesetzt.

### Dateien, Eingabe und Speicherstaende

- Discdateien werden aus der lokalen Originalinstallation ueber native
  Dateisystemzugriffe bereitgestellt.
- Maple-Eingabe wird auf native Controller-APIs abgebildet.
- VMU-/Flash-Speicherstaende werden ueber eine native, atomare und
  versionsgebundene Speicherdatei umgesetzt.
- Retaildaten, private Adressen und titelgebundene Hooktabellen bleiben im
  externen privaten Spielprojekt und werden nicht in KatanaRecomp eingecheckt.

## Erlaubte Kompatibilitaetsbruecken

Ein nativer Port braucht weiterhin klar definierte Daten- und Kontrollgrenzen.
Erlaubt sind deshalb kleine, explizite Adapter fuer:

- ABI-, Pointer-, Speicherlayout- und Endian-Konvertierung;
- Rueckrufe in statisch rekompilierten Spielcode;
- Hostzeit, Framepacing und asynchrone Completion;
- die minimale fuer den gebundenen Spielcode sichtbare Statusprojektion;
- Validierung, Identitaet, Save-Autoritaet und typisierte Fehler.

Diese Adapter duerfen keine Gastinstruktionen interpretieren, keinen
Dreamcast-Chip softwareseitig nachbauen und keinen allgemeinen
Emulationsfallback bilden. Die Grenze lautet: Daten uebersetzen und native
Dienste aufrufen, nicht Konsolenhardware ausfuehren.

## Historischer RuntimeOnly-Stand

Der Checkpoint `001f3c2` und der sichtbare No-Skip-Sonic-Lauf bei
`24,2926 MHz` bleiben wertvolle Bring-up-Evidenz. Sie haben unter anderem
belegt:

- die statische SH-4-AOT-Abdeckung bis in den Moviepfad;
- reale Kontrollfluss-, Funktions- und Adressbindungen;
- den erwarteten No-Skip-Audio-/Videolebenszyklus;
- relevante Render-, Audio-, YUV-, Callback- und Post-Movie-Grenzen;
- den nachgelagerten Identity-Miss `0x8C054008 -> 0x8C9000E8`.

Der dabei verwendete ARM7-/AICA- und CPU-PVR-Pfad ist ab jetzt nur historische
Referenz und Diagnoseevidenz. Er wird weder zum Produktpfad erklaert noch
weiter auf Produktperformance optimiert. Seine Messwerte sind keine
Abnahmebasis fuer den nativen Port.

## Umschaltbarkeit und Linkgrenze

Soweit ohne Architekturaufweitung moeglich, bleiben alte Geraetemodelle hinter
einem expliziten Buildprofil als Diagnosewerkzeug erhalten. Dabei gilt:

- `native-port` ist das Produktprofil und der einzige Releasepfad;
- ein historisches Geraeteprofil ist opt-in, nicht installierbar und nicht
  verteilbar;
- Produktartefakte duerfen keine ARM7-Interpreter-, SkyEmu-,
  PVR-Softwarerasterizer- oder Diagnoseinterpreter-Symbole enthalten;
- native Hooks duerfen nicht zur Laufzeit auf historische Geraetemodelle
  zurueckfallen.

## Verbindliche Taskreihenfolge

1. `KR-5000`: Native Produktgrenze und Linkisolation durchsetzen.
2. `KR-5001`: private SH-4-Spiel-/SDK-Grenzen und native Hookbindung
   vollstaendig ableiten.
3. `KR-5002`: Audio-/Moviepfad nativ anbinden und ARM7/AICA aus dem
   Produktprofil entfernen.
4. `KR-5003`: Grafikpfad an eine native GPU-API anbinden und CPU-PVR aus dem
   Produktprofil entfernen.
5. `KR-5004`: Disc, Eingabe, Save und verbleibende Plattformdienste nativ
   anbinden.
6. `KR-5005`: echter No-Skip-Sonic-Lauf bis Hauptmenue mit nativer Bild-,
   Ton- und Eingabekette; erst dieses Gate gibt `v0.50.0 Alpha` frei.

Der erste Implementierungsschritt ist die hoechste verifizierbare Hookgrenze,
nicht ein weiterer Umbau der alten Geraeteemulation. XenonRecomp ist das
Architekturvorbild: statische Recompilation plus portprojektspezifische native
Implementierungen; nicht Flycast oder ein anderer Emulator.

## Produktabnahme

Ein Meilenstein gilt nur, wenn derselbe native Produktpfad alle Punkte erfuellt:

- kein ARM7-Interpreter und kein CPU-PVR im gelinkten Produkt;
- statisch rekompilierter SH-4-Code mit fail-closed nativen Bindungen;
- Opening ohne Skip, Ersatzbild, erzwungenen Status oder privaten Bildhack;
- korrektes Bild ueber die PC-GPU und korrekter Ton ueber das PC-Audiogeraet;
- 60-Hz-PAL-Auswahl und korrekter weiterer Spielzustand;
- Memory-Card-Screen beziehungsweise Hauptmenue mit nativer Eingabe;
- inkrementeller Portbuild fuer normale Iterationen statt historischem
  Vollreexport.

`v0.49.1` bleibt Pre-Alpha, bis dieser rein native Pfad das Hauptmenue
erreicht. Genau dieser Nachweis gibt `v0.50.0 Alpha` frei. Ein Hauptmenue, das
noch ARM7, CPU-PVR oder andere emulierte Dreamcast-Geraete verwendet, erfuellt
das Gate nicht.

Dreamcast-MHz sind ab diesem Architekturwechsel kein Produktgate mehr. Sie
waren eine Kennzahl des emulationsnahen RuntimeOnly-Pfads. Fuer den nativen
Port werden reale Ladezeit, Framezeit/Framerate, Audio-Stabilitaet, CPU-/GPU-
Zeit, Hostauslastung und Eingabelatenz gemessen. Gastzyklen duerfen intern
weiter die Zeitbasis des rekompilierten Spielcodes bilden, sind aber keine
Behauptung, eine Dreamcast-CPU oder -Konsole zu emulieren.

200 MHz nachhaltiger AOT-Durchsatz und 250 MHz unpaced Reserve bleiben
nachgelagerte Optimierungsziele, soweit diese interne Messung fuer den
rekompilierten CPU-Pfad noch sinnvoll ist. Sie blockieren nicht die
Versionsgrenze: `v0.50.0 Alpha` wird durch das native Hauptmenue freigegeben.
