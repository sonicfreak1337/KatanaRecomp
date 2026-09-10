# Aktueller Projektstand

Stand: 10. September 2026, r351/0.49.9 inkrementell exportiert.
Drei AOT-Dateien kompiliert, 1111 Quellpartitionen wiederverwendet,
Linkaudit bestanden. EXE SHA-256:
`4fb01348d201ca3012308e76605d9a071ea5e6dbb46579d3f8524d2cbd1768e6`.

Der r350-Nutzerlauf erreicht die Super-Sonic-Transformation und stoppt
an einem fehlenden Effektinitialisierer in B_CHAOS7. Der Originalcode
registriert vier zusammengehoerige Initialisierer an fuenf Primary-Aufrufen.
Diese und ihre lokalen Folgefunktionen ergeben 13 neue Funktionswurzeln,
einschliesslich Update, Darstellung und Aufraeumen. Die Capsule belegt den
ersten Initialisierer und seine unveraenderten Originalbytes im Speicher.
Kein Textur-, Shader- oder Runtimefix und kein Ueberspringen von Spielcode.

Alle 13 neuen Roots und zehn Callback-Publikationen sind im fertigen Produkt
nachgewiesen; 17 verschiedene Zielblockhashes stimmen mit den Originalbytes
ueberein. B_CHAOS7 waechst von 7821 auf 8270 Einstiege (+449). Alle 249
Modulidentitaeten, Quellbindungen und alle 724067 bisherigen Moduleinstiege
bleiben erhalten. Globaler Seed260 -> 273 mit bytegleichem Praefix;
die anderen26 Seedpfade bleiben unveraendert. Modell- und Work-Datenzeiger
werden nicht als Task-Funktionen behandelt. Quellgebundene Candidate-Familie,
keine pauschale Strict-Promotion oder universelle Modulvollstaendigkeit.

Ein Export im vorhandenen Performance-Buildbaum: 469.299 ms Exportkern,
487.292 ms kompletter warmer Wrapper (8:07 Minuten), kein Kaltwert.
Keine eigenen Spielstarts, Replays oder Eingaben. Nutzer bestaetigt den
reparierten Transformationspfad und weiteren Super-Sonic-Fortschritt erst
im folgenden Spieltest. Evidence:
`private/diagnostics/r351-super-transform-20260910a/review.md`,
`build-result.json`, `export.stdout.log` und
`private/analysis/sonic-adventure-pal-v1003/r351-super-transform-family-v1/super-transform-family-audit.json`.

## Vorheriger Export r350

Stand: 10. September 2026, r350/0.49.9 inkrementell exportiert.
Drei AOT-Dateien kompiliert, 1111 Quellpartitionen wiederverwendet,
Linkaudit bestanden. EXE SHA-256:
`ac6cbf4abacd3006b22388940f3617e85b6cd8532752e3e831c423ebc9fc1a44`.

Der neue r349-Nutzercrash trifft den fehlenden Task-Update-Einstieg der
braunen Gebaeudefamilie im zerstoerten Station Square (B_CHAOS7). Der
vorherige Scan hatte eine eigene 0x14-Byte-ObjectList nicht erfasst.
Geprueft sind jetzt ihr Count/Pointer-Header, alle 37 Datensaetze und die
Folgefamilien der 20 lokalen Objektcallbacks: Gebaeude, Autos, Strassen,
Truemmer, Tentakel, Absperrungen, Reifen, Signal und Beschleunigungsfeld.
Die Erweiterung enthaelt 20 direkte Tabellenziele, fuenf weitere belegte
Wrapper-Varianten, fuenf gemeinsame Updates und elf lokale Folgefunktionen.

Alle 41 neuen Roots und alle 20 Tentakel-BRAF-Zustandsziele sind als exakte
Einstiege im fertigen Produkt enthalten; ihre 61 Blockhashes stimmen mit
den Originalbytes ueberein. B_CHAOS7 waechst von 6485 auf 7821 Einstiege
(+1336). Alle 249 Modulidentitaeten und Quellbindungen sowie alle 722731
bisherigen Moduleinstiege bleiben erhalten. Die 219 geerbten globalen Roots
bleiben bytegleich als Praefix erhalten; jetzt 260, die 26 anderen Seedpfade
sind unveraendert. Namen, Modellzeiger und SET-Daten werden nicht zu Code.
Quellgebundene Candidate-Familie, keine Strict-Promotion und kein Beweis
fuer alle beliebig berechneten Laufzeitziele des gesamten Moduls.

Ein kanonischer Export im bestehenden Performance-Buildbaum: 478.960 ms
Exportkern, 496.847 ms gesamter warmer Wrapper (8:17 Minuten). Die fruehe
Versionspruefung bestaetigt 0.49.9; kein CLI-/Provenance-Neuaufbau und kein
Kaltbuildnachweis. Keine eigenen Spielstarts, Replays oder Eingaben; der
Nutzer prueft den weiteren Super-Sonic-Fortschritt. Keine neue FPS-Messung.
Evidence: `private/diagnostics/r350-chaos7-registry-20260910a/review.md`,
`build-result.json`, `export.stdout.log` und
`private/analysis/sonic-adventure-pal-v1003/r350-chaos7-object-registry-v1/chaos7-object-registry-audit.json`.

## Vorheriger Export r349

Stand: 10. September 2026, r349/0.49.9 inkrementell exportiert.
Vier AOT-Dateien kompiliert, Linkaudit bestanden. EXE SHA-256:
`6748db0745fd7894b6fdd0ff23b841bb1dd1777a04578c0cdfb65781a3b331ad`.

Der normale r348-Nutzerlauf stoppt im zerstoerten Station Square bei einer
fehlenden Task-Update-Funktion in B_CHAOS7. Die Bereichspruefung schliesst
drei verwandte Update-Wrapper mit ihren lokalen Helfern und einer weiteren
Task-Initialisierung sowie zwei Objektfamilien mit je Display/Cleanup ein.
14 explizite Funktionswurzeln liefern 482 weitere Blockeinstiege: B_CHAOS7
steigt von 6003 auf 6485. Alle 249 Modulidentitaeten, ihre Quellbindungen und
alle 722249 bisherigen Moduleinstiege bleiben im ausgelieferten Produkt.
Die 205 bisherigen globalen Roots und 26 weiteren Seeddateien sind erhalten.
Bekannte 35 Task-Ziele und 38 unterschiedliche Zustandszweige waren bereits
vorhanden. Quellgebundene Candidate-Erweiterung; kein universeller Beweis
fuer beliebig berechnete Ziele und keine pauschale Strict-Promotion.

Der erste Export scheiterte vor dem Compile am CLI-Vertrag 0.49.8 gegen
Runtime 0.49.9. Die CLI wurde aktuell gebaut, ihre Disassembly-Provenance
erneuert; der Coverage-Payload ist bytegleich. Eine neue fruehe Versions-
pruefung verhindert denselben spaeten Konflikt. Der erfolgreiche Lauf nutzt
den Analyse-Cache und den bestehenden Buildbaum: 380.047 ms Exportkern,
425.936 ms kompletter warmer Wrapper. Einschliesslich Fehlversuch und
Reparatur ca. 20:27 Minuten vom ersten Exportlog bis zur Artefaktabnahme.
Das Zehn-Minuten-Ziel wurde insgesamt verfehlt; kein Kaltbuildnachweis.

Keine eigenen Spieltests, Replays oder automatischen Eingaben. Der Nutzer
behaelt die Steuerung; der weitere Super-Sonic-Fortschritt bleibt im Spiel
zu bestaetigen. Keine neue FPS-Messung. Evidence:
`private/diagnostics/r349-chaos7-20260910a/review.md`, `build-result.json`,
`export-retry2.stdout.log` und die neue source-bound Familien-Auditdatei.

## Vorheriger Export r348

Stand: 10. September 2026, r348/0.49.9 inkrementell exportiert.
70.156 ms gesamter warmer Wrapper, 62.987 ms Buildhelfer, 1 Adapter,
0 AOT-Compiles, Linkaudit bestanden. Pack und generierte Quellen unveraendert.
EXE SHA-256:
`93cd325d33ec58003fb9c4165f5f44b6a6932b9e9dfbb1dab5336d24f9a33a70`.

Zwei normale r347-Nutzerlaeufe stoppen beim selben TIKAL-Acquire:
PC 8C099690, PR 8C049606, TEXLIST 0CB01814, Frames 8400 und 8193.
Die Liste liegt in TIKAL_PROG, gehoert aber bereits einem aktiven TextureSet.
Der Named-Loader leitete einen zusaetzlichen Modulbesitzer allein aus dieser
Speicherlage ab und verwarf die gemeinsame Benutzung. Der Fix beweist zuerst
die vollstaendige aktive Setzeile samt Katalog/Header/Referenz und erhaelt
dann den Set-Besitz fuer Named-Acquire, Wiederholung und Erneuerung. Die
vorhandenen Freigabe- und Saved-State-Pfade zaehlen die echten Besitzer weiter.
Fremde Kataloge, geaenderte Deskriptoren und echte Modulbesitzer bleiben geprueft.

Die Originalbytes binden den Request an EV0160s Named-Tabelle, Eintrag 8,
und den vorhandenen Primary-SDK-Aufruf. Keine neue AOT-Root erforderlich.
Die Deskriptorbilder 1/2 der Capsule sind aktuelle/publizierte Zeilen;
Bilder 3/4 sind historische Vor-/Ersetzungsbilder, kein aktueller Free-Beweis.
SAOW liefert kuenftig auf diesem Fehlerpfad zusaetzlich getrennte Carrier-
und Besitzerinformationen. Decoder benennt die vier Bildrollen ausdruecklich.

Der Nutzer uebernimmt wieder alle Spieleingaben; r348 startet normal ohne
Replay. Der reparierte TIKAL-Uebergang und der weitere Super-Sonic-Abschluss
sind noch nicht im Spiel bestaetigt. Keine neue FPS- oder Kaltbuildmessung.
Evidence: `private/diagnostics/r348-super-sonic-20260910a/review.md`,
`captured-crashes.json`, `microbuild/build-result.json`,
`microbuild/micro-runtime-contract.json` und `export.log`.

## Vorheriger Export r347

Stand: 10. September 2026, r347/0.49.9 inkrementell exportiert.
94.056 ms Buildhelfer, 1 Adapter, 0 AOT-Compiles; Linkaudit bestanden,
Pack unveraendert. EXE SHA-256:
`c8d0432a5818a4ea86cd43ef61842851eb15a0e6461dca05d04cc6fae70c9749`.

Der eigene r346-Replay reproduziert den neuen Super-Sonic-Crash exakt:
PC, PR, alle GPR, Frame1465 und aktives Event stimmen mit der Nutzer-Capsule
ueberein. Die Konsole belegt eine originale Eintrag-Aliasliste auf einen
geladenen CHAOS_SURFACE-PVM-Descriptor; der bisherige Resolver suchte diese
Autoritaet nur fuer die urspruengliche PVM-Liste. Der gemeinsame Resolver
folgt nun auch fuer nicht-statische Aliaslisten dem ausgewaehlten, weiterhin
vollstaendig geprueften physischen Descriptor. Die Capsule erhaelt auf diesem
Fehlerpfad zusaetzlich Listenauswahl und Owner-/Registry-/PVM-Details.

r347 hat mit identischem aufgezeichnetem Inputpraefix plus neutralem Tail
die alte Fehlerstelle passiert. Der Lauf wurde bei Frame6262 zurueckgegeben.
Ein weiterer Versuch erreichte etwa Frame6899 und wurde ebenfalls fuer die
Nutzersteuerung beendet; keiner bestaetigt den spaeteren TIKAL-Uebergang.
Der Nutzer lehnt weitere Replaysteuerung ab. Evidence:
`private/diagnostics/r347-super-sonic-20260910a/reproduction-evidence.json`,
`texture-review/review.md`, `microbuild/build-result.json` und
`replay-r347-super-sonic-20260910a/game.stderr.log`.

## Vorheriger Export r346

Stand: 10. September 2026, r346/0.49.9 auf Nutzer-"build" als Mikrobuild
exportiert. 0.5 folgt auf Nutzeranordnung erst nach Super Sonic und Polish.
Buildhelfer: 73.715 ms, 1 Adapter-Compile, 0 AOT-Compiles; Linkaudit bestanden.
AOT-Pack und generierte Quellen bleiben bytegleich zum r345-Bestand.

Der Super-Sonic-Stop beim Eventwechsel betrifft die gemeinsame PVM-
Texturverwaltung: identischer Kataloginhalt kann gleichzeitig in mehreren
physischen SDK-Registry-Zeilen publiziert sein. Acquire und Saved-State-
Preflight identifizieren Aliasse jetzt nach Katalog, Ordinal und physischer
Zeile. Jede Publikation behaelt ihren eigenen Resident-Pin. Widerspruechliche
Payloads derselben Zeile und ungueltige aktuelle SDK-Zustaende bleiben Fehler.
Ein Python-Modell mit Quellklauselpruefung sowie C++-Build und Linkaudit
bestehen; Gameplaybestaetigung steht aus.

Die begrenzte Folgepfadpruefung bis zum letzten Super-Sonic-Event sowie Chaos 7
findet keine belegte neue AOT-Root: bekannte Loader-, Task- und Literalziele
sind im aktuellen Produkt vorhanden. Dies beweist nicht jeden dynamischen
Folgezustand oder den abschliessenden Primary-Credits-Uebergang. Der Mikrobuild
aktualisiert ausschliesslich das Host-Versionslabel auf 0.49.9 innerhalb des
erhaltenen CMake-Vertrags; ABI-Pruefungen bleiben aktiv. Kein Agenten-Spielstart.
Der Nutzer uebernimmt den Spieltest. Lokal committen, nicht pushen.

EXE SHA-256: `62519d3709575f2254be43c7ddc818d6dc31bbaca52691958ba4a1cad77c639b`.
Pack-Identitaet: `9e7b16c74a08873e33923af5d289568f12191352bfab9a4e41e197d54f65574a`.
Pack-Metadaten-Datei-SHA: `cf95c62bd9c7226a3b31247d39bdaa34299bb97062bc1564721aeb2746d524a1`.
Evidence: `private/diagnostics/r346-super-sonic-20260910a/microbuild/build-result.json`
und `micro-runtime-contract.json`; Quellenpruefung im uebergeordneten Verzeichnis.

## Letzter Export r345

Stand: 10. September 2026, r345/0.49.8 inkrementell exportiert auf Nutzer-"go".
Der Nutzer bestaetigt anschliessend: "sonic story beendet." Damit sind Sonic,
Tails, Knuckles, Amy, Big und Gamma Story clear durch Nutzertests bestaetigt.
Offener Story-Meilenstein: Super Sonic. Chao Garden/Race bleiben separat offen;
die Storymeldung beweist weder deren Abschluss noch alle Save-/Load-Pfade.

Egg Vipers fehlende Zustands-Callback-Familie umfasst elf neue belegte Roots;
194 bisherige Familienzeilen bleiben bytegleich (205 gesamt). Die benachbarten
zwoelf ASCII-Zustandsnamen werden nicht als Code aufgenommen. Die 21 bereits
vorbereiteten Chao-Race-Roots sind in diesem AOT-Batch ebenfalls enthalten.

Ninja-Plan: 14 AOT-Compiles, Budget 32. Die Generierung verwendet 1098 von
1112 Partitionen wieder. Exportkern 492.871 ms, gesamter warmer Wrapperaufruf
laut Log-Dateizeiten etwa 8:31 Minuten; keine Kaltpfadmessung. Linkaudit besteht.
Keine weiteren Zusatztests und kein Agenten-Spielstart. Boss-/Chao-Gameplay
bleibt beim Nutzer. r344 hat Final Egg/Emblem-Freigabe bereits passiert und
ist erst in Egg Viper an B_EGM3+8AB4 gestoppt. Light Speed Dash ist bestaetigt.

Die begrenzte Quellpruefung erfasst elf Handler und 16 direkt aufgerufene lokale
Helfer; zwei externe SDK-Ziele sind bereits kompiliert. Der anschliessende
Nutzertest bestaetigt Sonics Storyabschluss; Chao Race ist noch unbestaetigt.

EXE SHA-256: `57aea41e65d385b040caba71e3add392f859eaad0b537a302099d9a0396fbde8`.
Pack SHA-256: `9e7b16c74a08873e33923af5d289568f12191352bfab9a4e41e197d54f65574a`.
Evidence: `private/diagnostics/r345-egg-viper-20260910a/build-result.json` und
`export.stdout.log`. Lokal committen, nicht pushen.

## Vorheriger Export r344

Stand: 10. September 2026, r344/0.49.8 inkrementell exportiert.
Der Nutzer hat den Batch nach dem einzigen neuen Sonic-Crash geschlossen.
1 Adapter, 0 AOT-Compiles; 88.429 ms im Buildhelfer. Der gesamte Aufruf dauert
laut Log-Dateizeiten etwa 96 Sekunden (Warmbuild, keine Kaltpfadmessung).
Linkaudit bestanden, AOT-Pack und generierte Quellen bytegleich zu r343.

Der neue Stop nach Final Egg betrifft die Emblem-Aufraeumroutine: eine von
zwei TEXNAME-Referenzen ist null. Der SDK-Release reproduziert jetzt den
quellgebundenen Original-/Flycast-ROM-Pfad als erfolgreichen No-op, waehrend
gueltige Zeilen weiterhin geordnet freigegeben werden. Ungueltige Nicht-Null-
Zeiger bleiben vor Mutation abgewiesen. Vollstaendig leere Listen brauchen
keine Registry. SRLS/SRLJ/SRLP liefern Nullanzahl, genaue Ablehnungsstelle und
begrenzte Descriptor-/SDK-Fehlerdaten vor einer Freigabe. Die Ursache der
unvollstaendigen Emblem-Ladung ist damit nicht behauptet geschlossen.

Sieben Freigabe-Komponentengruppen, quellgebundene SDK-/BIOS-/Flycast-Pruefung,
Adapter-Syntax und Produkt-Linkaudit bestehen. Der Dash-Fix wurde vom Nutzer
in r343 bestaetigt. Kein Agenten-Spielstart; der r344-Storyuebergang ist noch
nicht spielbestaetigt. Die 21 vorbereiteten Chao-Race-Roots bleiben ausserhalb
dieses kleinen Sonic-Mikrobuilds. Version weiterhin 0.49.8.

EXE SHA-256: `3a6f1ef614ee07d79f1079603338d9f9a465309d7651dd06dbc52893d0c251d0`.
Pack SHA-256: `3ccc5a45aeef59a4c58e05e686a0548273ec9f8fed194b3aca84213dd87211f3`.
Adapter SHA-256: `ab717909f9a5614b9164f968594458ba91fc2912784a12d24c947e3e259f9e11`.
Freigabehelper SHA-256: `faea456d1d17888620359ccd912b2315c99212cadc338fc2b5a081bec83196a9`.
Evidence: `private/diagnostics/r344-sdk-release-null-20260910a/` und
`private/diagnostics/r344-final-egg-emblem-20260910a/`. Lokal committen, kein Push.

## Vorheriger Export r343

Stand: 10. September 2026, r343/0.49.8 als Sonic-Testbuild exportiert.
Auf die erneute Anordnung "Sonic ist der Test" wurde der kleine Build vorgezogen:
1 Adapter, 0 AOT-Compiles, 84.207 ms im Produkthelfer; Linkaudit bestanden.
Analyse, generierte Quellen und AOT-Pack sind bytegleich zu r342. Kein Agenten-
Spielstart. Der Nutzer bestaetigt den Light-Speed-Dash-Fix in r343 und
meldet nach Final Egg einen neuen Stop in der Emblem-Aufraeumroutine
(6087FC, TEXLIST 8C1C5398, frame 12028). Die zweite Texturreferenz ist null;
die neue strikte Release-Vorpruefung weist sie ab. Die Kapsel enthaelt zuvor
29 erfolgreiche SDK-Freigaben mit je einer freigegebenen Registry-Zeile.
Der konkrete Folgeuebergang bleibt offen; keine erneute Registry-Erschoepfung
ist fuer diesen Stop belegt.
Der folgende Quellfix reproduziert den durch Original-BIOS und Flycast
belegten Null-Eintrag als erfolgreichen No-op. Gueltige Eintraege bleiben
geordnet freigegeben; ungueltige Nicht-Null-Zeiger werden weiter abgewiesen.
Sieben Komponentengruppen, der Quellenabgleich und Adapter-Syntax bestehen.
Noch kein Folgeexport oder Spieltest dieses Fixes.

Der generische 6087FC-Freigabepfad fuehrt jetzt die kompilierte originale
64DD00-Routine fuer jede TEXNAME-Referenz aus, statt fuer unbekannte native
Owner pauschal -1 zurueckzugeben. Reihenfolge, Referenzen, Aliasse und letzter
Fehlerindex folgen dem SDK. Ausstehende GPU-Freigaben werden korrekt budgetiert;
auch Type0 geladen -> ungeladen mit verbleibenden Registry-Flags entwertet
sein freigegebenes GPU-Backing. Die r342-Final-Egg-Kapsel zeigte weiterhin eine
volle Registry; die erfolgreiche konkrete Storypassage ist noch nicht belegt.

Der graue Dash hat einen nachgewiesenen Materialfehler: Originalmodelle besitzen
einen gueltigen schwarzen Materialeintrag trotz Materialzaehler null. Alle drei
Basic-Owner lesen nun wie das Original die tatsaechlich referenzierte Tabelle.
Deklarierte Metadaten bleiben fuer bestehende Modellidentitaeten getrennt.
15 gezielte Materialfaelle, alle 65.536 Flycast-TA-Farbtabelleneintraege,
6 Freigabe-Komponentengruppen und Adapter-Syntaxpruefung bestehen.

Chao Race: 21 neue quellgebundene Einstiege sind vorbereitet (95 alte Zeilen
bytegleich, 116 insgesamt), aber NICHT im r343-Mikrobuild enthalten. Ihr AOT-
Export folgt erst im naechsten freigegebenen Batch. Der alte Exportverweis auf
entfernte r318-r321-Proofinputs wurde durch einen verifizierten kompakten
Candidate-Checkpoint ersetzt: 10 Familien, 14 bestehende Roots, 0 Promotionen.
Keine historischen Dateihashes wurden ersetzt oder neue Einstiege erfunden.

EXE SHA-256: `4512b3e29949b58b2715596c9f8ff97a2f5acae56f11e503e1c18cfcd556f1d1`.
Pack SHA-256: `3ccc5a45aeef59a4c58e05e686a0548273ec9f8fed194b3aca84213dd87211f3`.
Provider SHA-256: `b973c40a41af675191f355f219bfd3b348d80f98a600c50ef4617e0c393d91ed`.
Evidence: `private/diagnostics/r343-dash-color-20260910a/review.md`,
`source-bindings.json` und `microbuild/build-result.json`. Private Quellaenderungen
bleiben ausserhalb des oeffentlichen Git-Repositories. Lokal committen, kein Push.

## Vorheriger Export r342

Stand: 10. September 2026, r342/0.49.8 inkrementell exportiert.
Der Nutzer hat den r341-Batch mit "batch vollstaendig" geschlossen.
Der Sonic-Crash zeigt eine volle SDK-Registry mit 2.048 belegten Eintraegen.
Eine konkrete Leckquelle ist behoben: authentifizierte Texturtraeger geben
beim Ueberschreiben ihre SDK-Referenzen transaktional frei, bevor die neuen
Bytes geschrieben werden. Reine PVM-Lookup-Aliasse erhalten keine zusaetzlichen
Freigaberechte. Andere moegliche Registry-Leckquellen sind damit nicht bewiesen
geschlossen. Der Chao-Pfad uebergibt erfolgreiche SDK-Texturauswahl nun an den
nativen Indexschatten, statt nur Auswahlfehler zu beobachten. Die Kapsel belegt
nicht abschliessend, dass dies den gemeldeten Crash verursacht hat; begrenzte
SATP-v1-Daten erfassen einen verbleibenden Fehler ohne Draw-Unterdrueckung.

SDK Basic 6214FA verwendet seine eigenen Live-PCW/ISP/TSP-Worte, den originalen
ARC1-Vertrag, Materialzustand und dynamischen ARC1-Cache. Per-Mesh-Fog folgt
seinem Packet. Andere Renderer-Owner behalten ihre Vertraege. Der r341-Witness
zeigte bereits aktive Float-Farben bei weiterhin falschem Dash; die neue
Packet-Korrektur ist daher noch keine bestaetigte visuelle Reparatur.
Acht Packet- und neun Farb-Komponentenfaelle, Carrier-Quell-/Kapselaudit und
Adapter-Syntaxpruefung bestehen. Keine eigenen Spielstarts; der Nutzer testet.

Mikrobuild 81.635 ms im Produkthelfer einschliesslich Retained-Validierung/Kopie,
ohne vorgelagerten Runtime-Refresh. Genau 1 Adapter-Compile, 0 AOT-Compiles,
Linkaudit bestanden. Generierte Quellen und Pack bleiben bytegleich zu r341.
Version bleibt 0.49.8: Tails, Big, Amy, Gamma und Knuckles durchgespielt;
Sonic und Super Sonic offen. Beide neuen Crashpunkte und der Dash-Effekt
brauchen noch die Bestaetigung im Spiel.
EXE SHA-256: `88a632da5aab4bc7d24de60a3d94cc8165e166c455d3b407c33ff4b1b1e28eba`.
Packdatei SHA-256: `3ccc5a45aeef59a4c58e05e686a0548273ec9f8fed194b3aca84213dd87211f3`.
Evidence: `private/diagnostics/r342-dash-followup-20260910a/review.md`
und `microbuild/build-result.json`. Private Adapteraenderungen liegen ausserhalb
des oeffentlichen Git-Repositories; der lokale Commit dokumentiert ihre Evidence.

## Vorheriger Export r341

Stand: 10. September 2026, r341/0.49.8 inkrementell exportiert.
Der Nutzer hat den r340-Batch geschlossen und den Build ausdruecklich gestartet.
Chao: die native Texture-Pruefung akzeptiert nun die originale gefuellte
Flagfamilie einschliesslich 0x80000060. Sonic Final Egg: unbeleuchtete Modelle
ohne Environment Mapping lesen keine unbenutzten Normalen mehr; beleuchtete
und Environment-Verbraucher behalten ihre Validierung. Der SDK-Dash-Pfad
6214FA verwendet seine originale Lichtvorbereitung und Float-Farbquantisierung;
TitleBasic, ResidentLegacy und der andere SDK-Owner 638D72 behalten ihre
Farbpfade. Neun gezielte numerische FPU-/Farbchecks und Adapter-Syntaxpruefung
bestehen. Die sichtbare Dash-Korrektur und beide Crashpunkte sind noch nicht
im Spiel bestaetigt; der Nutzer uebernimmt die Tests. Keine eigenen Spielstarts.
Mikrobuild 91.437 ms einschliesslich Retained-Validierung/Kopie, jedoch ohne
vorgelagerten Runtime-Refresh; genau 1 Adapter-Compile, 0 AOT-Compiles.
Linkaudit bestanden. Alle generierten Quellen und der AOT-Pack bleiben
bytegleich zu r340. Version bleibt 0.49.8; Sonic und Super Sonic sind offen.
EXE SHA-256: `bf2bb7451afe25979f4f1b0e93731d01cda17873611c24dc0d1d584cc3d734c1`.
Packdatei SHA-256: `3ccc5a45aeef59a4c58e05e686a0548273ec9f8fed194b3aca84213dd87211f3`.
Evidence: `private/diagnostics/r341-light-dash-material-20260910a/review.md`
und `microbuild/build-result.json`. Saves wurden nicht restauriert/kopiert.

## Vorheriger Export r340

Stand: 10. September 2026, r340/0.49.8 exportiert, Linkaudit bestanden.
Tails, Big, Amy, Gamma und Knuckles sind vom Nutzer vollstaendig durchgespielt.
Chao: 79 neue quellgebundene Einsprungstellen, 95 inklusive der 16 erhaltenen
Vorgaengereintraege. Darunter 43 Zustandshandler, 11 BRAF-Arme, 6 Rennpfad-
und 19 weitere lokale Literalziele. AL_MAIN besitzt nun 21.153 statt 15.114
exakte AOT-Blockeinstiege. Alle 249 Module und bisherigen Einstiege bleiben
erhalten; keine eigene Spielausfuehrung und keine Strict-Proof-Promotion.
CLI-Export 757.229 ms (ohne Wrapper-Preflight), Hostbuild 89.202 ms:
28 AOT-Dateien, insgesamt 37 Uebersetzungseinheiten neu kompiliert. Das
Zehn-Minuten-Ziel wird weiterhin verfehlt; Analyse/Validierung dominiert.
Der Nutzer testet r340 seit 13:29:45; Save-Hashes haben sich durch seinen
Lauf geaendert. Kein Agenten-Save-Restore und kein eigener Spielstart.
Sonic Final Egg bleibt ohne bestaetigten Ursachenfix; Allocator-Preflight und
Fehlerkapsel liefern nun zusaetzliche Absicherung und Daten. Der Nutzer
bestaetigt den weiterhin falschen Light-Speed-Dash-Effekt; der r340-Witness
ist vorhanden und wird fuer den naechsten Grafikfix ausgewertet.
EXE SHA-256: `3709c9a303fbd3a21ceeaeaa4274d0028dcdff26b0c2f6ea26cbbe1b5b48611d`.
Packdatei SHA-256: `3ccc5a45aeef59a4c58e05e686a0548273ec9f8fed194b3aca84213dd87211f3`.
Profil 47, 1.107 Partitionen, 19.263 Funktionen, 309.592 Bloecke.
Evidence: `private/diagnostics/r340-user-story-crash-batch-20260910a/result.json`.

## Vorheriger Stand und Quellvorbereitung

Stand: 10. September 2026, r339/0.49.6 exportiert und Linkaudit bestanden.
Kein eigener Spielstart. Der Nutzer bestaetigt Gamma und Knuckles zu 100 Prozent;
Tails, Big, Amy, Gamma und Knuckles sind damit durchgespielt. Version 0.49.8 ist fuer
den naechsten Export gesetzt. Sonic Final Egg stoppt weiterhin bei der
SDK-Texturfreigabe. Die neue r339-Kapsel identifiziert den Verwaltungswert
an 0C88F5E0 als abgelehnte Kapazitaetspruefung; dessen Zahlenwert ist nicht
aufgezeichnet. Der separat aufgezeichnete Wert 6283 ist der aktuelle
Texturschluessel an 0C6733CC, keine Slotanzahl. Die Light-Speed-Dash-Diagnose
hat beim Nutzerlauf keine Datei erzeugt und wird ebenfalls untersucht.
Die zentralen Saves sind vor/nach Export SHA-gleich.
Der offene Quellbatch erweitert die Fehlerkapsel um echte Highwater-/Cursor-
Werte und korrigiert die begrenzte Dash-Diagnose vor Cull/Transform samt
Dateifehlernachweis. Adapter-Syntaxpruefung besteht; kein weiterer Export.
Diese Diagnoseaenderungen sind keine bestaetigten Sonic-/Grafikfixes.
Zusaetzlich ist der gemeinsame SDK-Allocator 8C64E55E mit einer quellgebundenen
MayContinueOriginal-Pruefung abgesichert: freie Slotwahl bleibt original,
Full/ungueltige Metadaten stoppen vor dem Retail-One-past-end-Schreiben.
13 gezielte Komponentenfaelle und Adapter-Syntaxpruefung bestehen. Der
Guard ist noch nicht exportiert; ein echter Ressourcenverlust ist damit
weder bewiesen noch behoben. Kein globaler Write-Watchpoint wird aktiviert,
weil dieser die schnellen RAM-Schreibpfade im ganzen Spiel abschalten wuerde.
Evidence: `private/diagnostics/r340-user-story-crash-batch-20260910a/`.
Der nachtraeglich gemeldete Chao-Garden-Crash gehoert zum selben freigegebenen
Batch. Er stoppt in AL_MAIN an einer bisher nicht kompilierten Zustandstabelle.
Auf Nutzeranordnung werden AL_MAIN, die drei Gartenmodule und AL_RACE auf
weitere belegte Tabellen-/Callback-Luecken abgeglichen; die vorhandenen
Objektlisten und bisherigen Einsprungstellen bleiben erhalten. Keine eigenen
Spielstarts; diese Quellpruefung ist keine vollstaendige Laufzeitabnahme.
EXE SHA-256: `a0ede55f39813c41d7ddbb6a73dc0dbc9b705ab94d4e9ca9b22462073a956f4c`.
Packdatei SHA-256: `1b2b4d3d1a331239a22f05796e5a8c69aeb595d8fae74ed3e7800b4f74d85d4b`.
Profil 47, 1.098 Partitionen; genau eine AOT-Quelldatei ist gegen r338 geaendert.
Der letzte Exportversuch dauerte 112,536 s, davon 67,636 s Hostbuild. Dies ist
weder die Gesamtdauer des Batches noch ein Kaltexportwert: eine veraltete
Providerbindung, eine zusaetzliche alte Partitionsdatei und das versehentlich
verlorene Ninja-Log erforderten vorherige Wiederherstellung. Nach nativem
`restat` mit unveraenderten echten Befehls-Hashes mussten 136 AOT-Einheiten
regulaer gebaut werden. Das neue echte Log ist gesichert; Standardbudget
bleibt 32. Details und Artefaktbindungen stehen im privaten Batchreport.

Die r338/0.49.5-Spieltests bestaetigen: Credits zeigen
wieder lesbare Namen/Rollen und die richtigen wechselnden Hintergrundbilder.
Die normale Framefamilie beruecksichtigt jetzt den aktiven Original-Videomodus
und gemeinsam verbrauchte Completion-Slots. Emerald Coast und Windy Valley
bestehen je einen sichtbaren 60-Sekunden-Lauf mit Bewegung. Nicht alle Timing-
Owner und spaeten Storyuebergaenge sind damit abgenommen. Windy Valley hat
weiterhin zu wenig CPU-Leistungspuffer.
Am 10. September bestaetigt der Nutzer in r338 Amys abgeschlossene Story
und korrekt laufende Credits. Nach Tails und Big ist dies die dritte
durchgespielte Kampagne. Der Versionsbump auf 0.49.6 gilt ab r339;
r338 bleibt unveraendert 0.49.5. Die aktuellen Testruns uebernimmt
der Nutzer; kein Agent startet einen konkurrierenden Spieltest.
Der anschliessend geschlossene r338-Crashbatch gibt den Reparaturexport frei.
Diese Credits-Bestaetigung ersetzt nicht die Abnahme aller Kampagnenenden.
Die statische Delta-Nachpruefung bleibt eingeschraenkt. Historische Runs und
Zwischenstaende stehen in Git, `STATUS.md`, `TASKS.md` und `ROADMAP.md`.
Private Produkt- und Laufmanifeste binden die genauen Artefaktidentitaeten.

## Reparaturbatch nach r338

Nach dem Export wurde der private Sonic-Buildablauf gegen den r339-
Wiederherstellungsfehler abgesichert: Micro-Dry-runs verwenden den absoluten
CMake-Ninja-Pfad, bekannte inkompatible Tools werden vor Zugriff auf v7-Logs
abgewiesen, und beide Wrapper sichern die echten Log-/Abhaengigkeits-/
Buildgraphdateien vor sowie nach einem erfolgreichen Build. Der isolierte
Test mit den zwei installierten Ninja-Versionen prueft die Ablehnung ohne
Logaenderung, SHA-genaue Kopien, Ueberschreibschutz und gesperrte Metadaten.
Auch die echte r339-Metadatensicherung besteht ohne Veraenderung des Logs;
der Cachetyp `UNINITIALIZED` wird neben `FILEPATH` und `STRING` korrekt gelesen.
PowerShell-Parserpruefungen bestehen. Ein weiterer Produktexport wurde dafuer
nicht gestartet; die Wrapperintegration ist noch nicht in einem neuen
Produktlauf bestaetigt. r339/game.exe bleibt unveraendert.
Evidence: `private/diagnostics/r339-ninja-recovery-hardening-20260910b/`.

Die drei neuen Nutzerkapseln sind getrennt ausgewertet. Gamma fehlt ein
Callback aus der Partikelfactory des letzten Bossmoduls; die quellgebundene
Pruefung aller elf Bossmodule ergaenzt einen Root und behaelt alle 193
vorherigen Eintraege bei. Knuckles scheitert vor der NINJA-Transformation an
der Hostmatrixpruefung. Die native Projektion und Normalmatrix verwenden
jetzt nur die vom Original konsumierten FTRV-Ausgaben. Unbrauchbare echte
Rasterdreiecke werden verworfen; gueltige, aber nicht abbildbare Geometrie
bleibt ein typisierter Fehler. Die damalige Kapsel enthaelt kein XF und
beweist daher nicht, welcher Matrixwert den konkreten Stop ausloeste.

Sonic Final Egg scheitert an der SDK-Texturfreigabe fuer FINALEGG2, nicht
an einem Draw- oder Descriptorlimit. Der native Freigabeplan bildet die
separaten registrierten, aber ungeladenen Zeilen und SDK-Fehlerpfade ab.
Ein validierter aktueller PVM-View bleibt zur Freigabe berechtigt, nachdem
ein verdraengter Hostalias bereits eingesammelt wurde. Registrygrenzen,
Zeilentypen und Transaktionsvorbilder bleiben validiert.

Zwoelf Freigabe-Komponentenfaelle und der aus dem Adapter extrahierte
Projektionscheck bestehen. Die erneuten Storylaeufe stehen aus.
Der helle Light-Speed-Dash-Effekt bleibt offen: Der gepruefte SDK-Farbpfad
entspricht dem Original. Ein begrenzter Mitschnitt protokolliert im
naechsten Nutzerlauf bis zu 64 verschiedene additive Modell-/Meshpaare
im konfigurierten Benutzerdatenverzeichnis als
`sonic-additive-material-witness.log`, ohne Farben zu aendern. Beim normalen
Windowsstart ist dies `%LOCALAPPDATA%/KatanaRecomp`.
Die ungemessene breite Integer-Load-Codegenoptimierung bleibt zurueckgestellt.
Private Quellevidence: `private/diagnostics/r339-user-story-crash-batch-20260910a/review.md`.

## Neue Messbasis fuer den CPU-Engpass (r338)

Der native Start aus einem exakt gebundenen Windy-Valley-Quicksave mit
synthetischem Input-Replay ist verifiziert. Zwei isolierte Laeufe desselben
Zustands lieferten 1199 beziehungsweise 1223 Spielframes in je rund 61 Sekunden
inklusive Start und Laden. Der heisseste persistente Thread belegte nach der
Startphase etwa 95.5-96.5 Prozent eines CPU-Kerns. Das ist keine neue
Performanceverbesserung und kein festes 60-Sekunden-Fenster nach dem Load.
Ein zusaetzlicher Lauf bestaetigt Vorwaertssteuerung und liefert ein aktuelles
IP-Profil: FPU, Speicher-/Ownerpruefungen und Buchfuehrung pro Gastinstruktion
bleiben relevante Kosten. Quicksave, Replay, EXE und kopierte Linkmap sind
privat mit ihren Identitaeten gebunden; kein neuer Produktexport war noetig.
Details: `private/diagnostics/r338-state-performance-20260910a/review.md`.

## Produktziel und Grenzen

Katana erzeugt statisch rekompilierte native PC-Ports. Der Produktpfad
enthaelt keinen Emulator, Interpreter, JIT, Runtime-Decoder oder geratenen
Kontrollfluss. NativeBringup bleibt nicht releasefaehig; ein funktionierender
Teil des Spiels bedeutet keine vollstaendige Closure oder Releaseabnahme.

Gleichrangige P0-Ziele sind erhaltener Story-/Gameplay-/Savefortschritt,
korrektes Originaltempo (einschliesslich stabiler 30 Updates/s im passenden
60-Hz-Spielpfad und abweichender originaler Szenenkadenz),
weniger CPU-Arbeit pro Titelupdate, unabhaengige 144-Hz-Praesentation, ein
vollstaendiger Kaltexport unter zehn Minuten und kosteneffiziente Umsetzung.
Bildwiederholungen zaehlen zur Praesentation, nicht als neue Simulationsbilder.
Die erneute Bytebindung belegt unterschiedliche originale Completion-Zahlen:
Staffroll fordert einen VBlank-Slot, andere Pfade zwei. Der PAL-Konstruktor
658500 waehlt das 625-Zeilen-Profil, die 525-Zeilen-Konstruktoren sind getrennt.
Der bisherige feste 30-Hz-Hosttakt war deshalb falsch. r334/r335 ergaenzen eine
optionale native Titeluhr neben der unveraenderten AOT-Hostservices-ABI. Der
gebundene frische PAL-Checkpoint beginnt mit 50 Hz; echte Mode-Apply-Pfade
binden 50/60 Hz an die angewandten Videowerte, nicht an den Auswahlindex.
Die 144-Hz-Ausgabe behaelt ihre eigene Uhr. Sondermodus 1 und der separate
604FF0-TMU-Pfad bleiben bei ihrem bisherigen Verhalten und sind noch nicht
ausreichend fuer eine Timingabnahme geklaert. `simulation_rate_hz=30` in der
generischen Snapshot-Struktur bleibt ein Konfigurationswert, kein Messwert
der titelgesteuerten Kadenz. Dazu dienen echte Frame-/Zeitdifferenzen.

## Belegter Fortschritt

- r337 war ein verworfener CPU-Versuch: P1/P2-Fastcase im erhaltenen ABI-
  Wrapper, direkte Nutzung des bestehenden Inlinehelpers in der Coverage-
  Runtime und ein kuerzerer Bucket-Hash mit weiterhin vollstaendiger
  Trefferpruefung. Komponenten- und vier sichtbare 60-s-Windy-Proben bestehen,
  aber beide AB/BA-Paare zeigen keinen CPU-Gewinn. Endpoint-CPU-ms/neuem Frame:
  r336 31,51 / r337 31,95, danach r337 40,30 / r336 36,50.
  Die Aenderungen wurden gemeinsam zurueckgenommen; die genaue Ursache der
  Streuung ist nicht belegt. Kein akzeptierter CPU-Performancefix.
  Der private Microbuild-Prozess wurde dagegen verbessert und real verifiziert:
  ohne veraltete Profil-Datei behaelt er exakt die vorhandene Compiler-Hotliste
  bei. Baseline/Working-Quellen, Pack, 32 Quellhashes, Cache- und EXE-Identitaet,
  Ninja-Budget und Linkaudit bleiben gebunden. r337 entstand so in 68,61 s,
  ohne Analyse/Codegen und mit null AOT-Neucompiles.
  r338 stellt anschliessend die unveraenderten Runtimequellen von e1d72c0
  wieder her: 49,46 s Microbuild, null AOT-Neucompiles, normales Linkaudit,
  bytegleiche generierte Quellen und Pack gegen r336. Produkt-SHA256:
  2415e8853b9b1e4a0ac93886fa5bab5500a284885fb6d61d1b80526f9886036d.
  Sichtbarer 60-s-Windy-Lauf mit Inputprofil 1 besteht ohne Capsule oder
  Forced Stop: 22,92 Titelupdates/s, 143,24 Praesentationen/s, P95 60,12 ms;
  staerkster persistenter Thread im Endpunktmittel 38,10 CPU-ms/neuem Frame.
  Weiterhin kein stabiler CPU-Leistungspuffer und kein Originaltempo-/
  Gesamtspielnachweis. Die Microbuild-Verkuerzung ist belegt; ein CPU-Gewinn
  nicht. Private Evidence: `r337-dispatch-cpu-20260910a`,
  `r338-sonic-action-stages-restored-runtime-20260910a`.
- r336 erweitert den vorbereiteten Admission-Cache um ein getrenntes
  NativeBringup-Profil. Nur Provider-Implementierungsidentitaeten werden fuer
  dessen Semantikschluessel projiziert; aktuelle Source, exakte Archive,
  Vorgaengerartefakte, Hooks, Backend, Guard-Inventar und erneut berechnete
  Hardwarebefunde bleiben gebunden. Offene Bringup-Befunde werden erhalten,
  niemals als Strict-Closure gewertet. Schema/Codec 2 verhindert Profilmischung.
  Die gezielten Codec-/Korruptions-/Identitaetstests bestehen. Der reale Export
  publiziert erstmals den neuen Cache; dessen spaeterer Verbrauch und ein
  Netto-Zeitgewinn sind noch nicht gemessen. Erstbefuellung kostet zusaetzlich.
  Der ebenfalls angebundene persistente FVA-Epoch-Cache ersetzt keine CFA oder
  IR-Validierung. Sonics aktueller Coveragepfad deaktiviert FVA ausdruecklich
  und nutzt diese Optimierung deshalb nicht. Kein Coverage-Zeitgewinn behauptet.
  CLI-Export 329,34 s, Compile/Link 45,87 s, eine kompilierte Einheit und
  1.386 wiederverwendete Einheiten. Wrapper-Vorpruefungen kommen hinzu;
  kein Kaltbuildnachweis. Produkt-SHA256:
  ecbc790053698ba38dd988cac4e3d47f0344d42291be4ad9225b9e939c316cd8.
  Ein sichtbarer 60-s-Windy-Valley-Lauf mit Inputprofil 1, isolierten Saves
  und ohne parallelen Build besteht ohne Capsule oder Forced Stop:
  24,12 Titelupdates/s, 138,89 Praesentationen/s, P95 54,58 ms.
  Der staerkste persistente Thread verbraucht im Endpunktmittel 32,06 ms
  pro neuem Frame; das ist keine P95-CPU- oder Leistungspuffer-Garantie.
  Runtime und Spiel-AOT wurden in diesem Batch nicht optimiert; Unterschiede
  zu r335 sind kein isolierter Performancegewinn. Private Evidence:
  `export-r336-export-cache-20260910a`,
  `r336-sonic-action-stages-export-cache-20260910a`.
- r334 misst im sichtbaren Credits-Fenster 49,9994 Titelupdates/s bei
  active_hz=50 und Release 1; die lesbare Schrift bleibt erhalten. Ein
  90-s-Emerald-Coast-Diagnoselauf speichert einen Quicksave und laedt ihn
  zweimal erfolgreich mit identischem RAM-Digest. Schema 5 speichert den
  aktiven Videomodus; absolute Hostdeadlines werden nach Load neu aufgebaut.
  Alte zulaessige Schemas 3/4 behalten ohne bekannten Modus den bisherigen
  Hostpfad bis zum naechsten echten Mode-Apply. Keine geratenen Modewerte.
  Derselbe Lauf findet aber eine neue gemischte Wait-Familie: Gameplay
  konsumiert Release 2 teilweise in producer-wait, teilweise in frame-begin.
  Der r334-Reset dazwischen addiert faelschlich Wartezeit nach CPU-Arbeit.
  r334 ist daher keine allgemeine Timingabnahme.
- r335 behebt genau diese Familie mit gemeinsamer absoluter Phase. Jeder
  erfolgreich gepruefte originale Ready-Decrement wird einmal verbucht;
  der normale Frame-Owner wartet den Rest oder den letzten bereits
  verbrauchten Slot und praesentiert einmal ohne zusaetzlichen Hostwait.
  Modewechsel, Load, generische Frames und ungebundene direkte Completions
  invalidieren die Phase. Es werden keine Extra-Callbacks oder Updates erzeugt.
  Drei neu kompilierte Einheiten, Hostbuild 47,19 s, CLI-Export 291,41 s;
  Wrapper-Vorpruefungen kommen hinzu. Keine neu kompilierten AOT-unit-v-Dateien.
  Produkt-SHA256: 109a299ab2cd85273c486ce789fd86aad6189357c27adc939a18eec7853e7886.
  Zwei sichtbare 60-s-Laeufe mit Inputprofil 1 und ohne parallelen Build
  bestehen ohne Capsule: Emerald Coast 24,55 Titelupdates/s / 141,12
  Praesentationen/s / P95 53,28 ms; Windy Valley 21,86 / 139,68 / 63,72 ms.
  Der beobachtete PAL-Gameplaypfad verlangt zwei 50-Hz-Slots, also 25 Updates/s.
  Windy Valley erreicht auch diese Kadenz nicht stabil. Keine isolierte
  CPU-Performanceverbesserung und keine stabile-30-/144-FPS-Abnahme.
  Abschliessend besteht dasselbe r335-Binary einen sichtbaren 60-s-Quicksave-
  Lauf: Save bei Frame 600, zwei erfolgreiche Loads mit identischem RAM-
  Digest und weiterer Ausfuehrung; natives Zeitlimit, kein Forced Stop.
  Eine separate sichtbare 30-s-Credits-Probe misst 50,0029 Updates/s im
  stabilen Fenster und zeigt weiterhin lesbare Schrift und Bildwechsel.
  Auch sie endet am nativen Zeitlimit. Der volle Abspann bleibt ungeprueft.
  Private Belege: `r335-sonic-action-stages-shared-completion-cadence-20260910a`,
  `r335-quicksave-roundtrip-20260910a`, `r335-credits-visible-20260910a`.
- r333 bindet SUMMARYs eigenstaendige `staffroll_txt`-Textur an den exakten
  geladenen Modulbesitzer. Registrierte Live-PVM-Deskriptoren haben Vorrang
  vor Bootstrap-Snapshots; letztere muessen auch die originale GBIX erfuellen.
  Damit werden recycelte Registryzeilen nicht mehr als alte SEGA-Textur gelesen.
  Zwei sichtbare Credits-Proben (60 s Diagnose, 40 s spaetere Bildaufnahmen)
  zeigen lesbare Namen/Rollen und korrekte Bildwechsel, ohne erzwungenes
  Prozessende. Sie pruefen nicht den vollstaendigen Abspann oder dessen Ende.
  Die gemeinsame SUMMARY-Schriftbindung gilt fuer alle Charaktere; sichtbar
  geprueft wurde hier Sonic. Keine Save-/Completion-Flags wurden gesetzt.
  Zwei weitere 60-s-Laeufe mit Inputprofil 1, ohne parallelen Build, bestehen
  ohne Capsule: Emerald Coast 29,56 Sim-FPS / 139,25 Present-FPS / P95 40,54 ms;
  Windy Valley 23,99 / 139,05 / 62,40 ms. Das ist eine Regressionsprobe,
  kein Nachweis stabiler 30 Hz oder eines isolierten Performancegewinns.
  Produkt-SHA256: bfdc654c6df9f6b2470820e4230fae4dd3e32dbbfa93bd1c92403fc4a314cf04.
  Vier neu kompilierte Einheiten, Hostbuild 45,61 s, CLI-Export 282,04 s;
  Wrapper-Vorpruefungen kommen hinzu. Analyse-/AOT-Checkpoint wurde erhalten.
- r332 ergaenzt einen gebundenen Credits-Diagnoseeinstieg ueber ADVERTISE-
  Cleanup und den originalen Main-State 20. Der sichtbare Lauf erreicht
  SUMMARY/Staffroll (State 21), spielt SONIC.ADX und endet am nativen
  Zeitlimit ohne Crash oder erzwungenes Prozessende. Keine Save- oder
  Completion-Flags werden gesetzt. Der Timing-Witness zeigt Release 1
  bei weiterhin etwa 33,3 ms pro Titelupdate; die Cadence ist noch falsch.
  Die Bilder zeigen eine alte Sega-Textur beziehungsweise eine hellblaue
  Flaeche statt korrekter Credits. Damit ist der Fehler reproduzierbar,
  im damaligen r332 noch nicht behoben. r333 korrigiert die Schriftbindung
  und recycelte Bootstrap-Texturzeilen. r332 kompiliert vier Einheiten neu.
- r330 enthaelt die STG12-Callbackfamilie, den ausgelassenen Basic-Cull-Pfad,
  korrigierte SDK-Materialsteuerung und Immediate-UI-Zustand, die vorhandenen
  Ressourcen bei einer fehlenden MLT-Datei sowie geteilte SDK-Texturreferenzen.
  Das sind Quellkorrekturen; die vier gemeldeten spaeten Storycrashes und
  Credits-Grafik sind noch nicht durch einen erneuten Lauf bestaetigt.
- Eigener sichtbarer r330-Test mit isolierten Saves und gesteuerter Bewegung:
  Emerald Coast beendet 60 Sekunden ohne Capsule. Windy Valley und Amys Hot
  Shelter stoppen beim Laden an derselben neuen Texturvertragsverletzung.
  Eine bereits geladene SDK-Kennung kann in einem anderen Archiv andere PVRT-
  Daten bezeichnen. Das Original behaelt im geladenen Type0-Pfad die vorhandene
  Publikation und erhoeht ihren Referenzzaehler. r331 erhaelt deshalb deren
  echten Payload, Besitzer und Generation; es etikettiert sie nicht als neue
  Archivtextur um. Die Originaldaten bleiben unveraendert.
- r331 besteht dieselben drei sichtbaren Laeufe ohne Capsule. Neue Baseline,
  jeweils 60 Sekunden mit Inputprofil 1 und ohne gleichzeitigen Build:
  Windy Valley 22,44 Sim-FPS / 142,75 Praesentationen pro Sekunde / P95 65,27 ms;
  Emerald Coast 29,53 / 140,24 / 41,57 ms; Amy Hot Shelter 30,02 / 143,62 /
  34,38 ms. Das Hot-Shelter-Ergebnis betrifft den Einstieg, nicht den spaeteren
  vom Nutzer gemeldeten langsamen Abschnitt. Keine stabile-30-FPS-Abnahme.
  Produkt-SHA256: c7b0bc62dcd6fea0ba7c5754c9e717874bdf15f27498e0d065d48909e583c3b2.
  Der Hostbuild dauert 56,69 s bei drei neu kompilierten Einheiten. Der CLI-
  Export dauert insgesamt 299,85 s; Wrapper-Vorpruefungen kommen hinzu.
  Abdeckungsanalyse, Programmadmission und Codeausgabe laufen trotz
  Analysecheckpoint erneut und bleiben ein konkreter Iterationsengpass.
- Der echte Staffroll-Initialisierer fordert Release 1 an, der Recap-Pfad
  Release 2. Der bisherige feste Hosttakt verliert diese Unterscheidung.
  Originale Moduswahl, Hostkadenz und die davon getrennte 144-Hz-Ausgabe
  werden im anschliessenden Timing-/Performancebatch behandelt. Kein globaler
  60-Hz-Schalter und noch keine Behauptung einer behobenen Abspann-Synchronitaet.

- Der Nutzer bestaetigt Bigs abgeschlossene Story in r329. Nach Tails ist
  dies die zweite bis zum Abspann durchgespielte Kampagne. Sein Screenshot
  zeigt weiterhin zerlegte Credits-Schrift mit schwarzen Rechtecken.
  Der Versionsbump auf 0.49.5 ist quellseitig gesetzt; r329 bleibt 0.49.4.
  Der Abschlussuebergang nach Bigs Abspann ist noch nicht separat bestaetigt.
  Der Nutzer beobachtet ausserdem, dass der Abspann bei vollen 30 Sim-FPS
  langsamer als der in Echtzeit laufende Song fortschreitet. Das ist ein
  eigener Timingbefund: Erreichte konfigurierte Sim-FPS beweisen nicht das
  Originaltempo. Credits-Update-/Timerkopplung wird getrennt von der
  beschaedigten Schrift geprueft; kein pauschaler 60-Hz-Wechsel auf Verdacht.
- Der Nutzer schliesst den r328-Story-Crashbatch mit Big/Sonic bei Chaos 6,
  Knuckles bei Chaos 2, Amy beim Finalboss und Gamma in Hot Shelter.
  Die Ursachen sind getrennt: fehlende Dispatcher-/Initializer-Entries,
  ein unzulaessiger Hostabbruch bei ungueltigen Rasterpositionen und ein
  falsch modellierter SDK-Texturtransfer. Der r329-Quellbatch ist exportiert;
  damit ist noch keine Crashheilung im Produkt bestaetigt. Aktuell geht
  Storyfortschritt vor zusaetzlichen Performancearbeiten, und der Nutzer
  uebernimmt die Spieltests. Der Vergleich umfasst alle Bossmodule und die
  zugehoerigen belegten Uebergangsfamilien. Die Pruefung bindet elf Bossimages,
  17 Dispatcher der untersuchten Form und 276 Auswahlzellen. Zwoelf neue
  Candidate-Einstiege ergaenzen Chaos 6 (zwei), Chaos 2 (acht) und Hot Shelter
  (zwei), unter bytegenauem Erhalt aller 57 vorherigen globalen Seed-Zeilen.
  Die erweiterte Eventpruefung ergaenzt einen weiteren Chaos-6-Einstieg und
  die komplette fuenfteilige Chaos-7-Familie zum Setzen des Eventzustands.
  Fuer Chaos 7 waren die zuvor untersuchten 39 Auswahlzellen, 15 lokalen
  Literalaufrufe und 35 bekannten Task-Einstiege bereits kompiliert; diese
  engere Pruefung erfasste die neuen Eventeinstiege noch nicht. Die
  anschliessende Pointer-State-Pruefung ergaenzt 90 weitere Startpunkte:
  E-101R (42), ZERO (35) und Egg Viper (13). Bei Egg Viper binden bereits
  kompilierte Eventaufrufe die fehlende Initialisierung, fuenf Zustandssetter,
  das Aufraeumen und den Task-Verbraucher seiner fuenfteiligen Tabelle.
  Die anschliessende Pruefung ihrer Taskregistrierungen ergaenzt 26 weitere
  Einstiege: E-101R (13), ZERO (11) und Egg Viper (zwei). Sie bindet 24 neue
  Update-/Anzeige-/Aufraeum-Callbacks plus zwei lokale Literalcallee-Ziele.
  Alle 163 zuvor inventarisierten Boss-Lifecycle-Ziele sind bereits im r328-
  Pack enthalten. Insgesamt 134 neue Candidate-Zeilen in sieben Modulen;
  alle 57 bisherigen globalen Zeilen bleiben bytegleich erhalten, Endstand
  191. Der CFG-Abgleich verfolgt zusaetzlich 36 direkte BSR-Ziele ohne
  redundante Root-Eintraege.
  Nicht eindeutig aufgeloeste indirekte Aufrufe erzeugen keine geratenen
  Einstiege. Die benachbarten Pointerfolgen sind Candidate-Evidence, kein
  Beweis fuer die erlaubte Indexmenge. Nicht belegte Eventproducer und andere
  Dispatcherformen bleiben explizit offen; dies ist keine vollstaendige
  Boss-Gameplayabnahme oder Strict-Cross-Image-Promotion.
  Der private Adapter verwirft ungueltige Rasterdreiecke statt den Big-Lauf
  abzubrechen und bildet die SDK-Surfacetransfers samt Mipmap-Offsets und
  tatsaechlich benoetigten VQ-Codebookdaten ab. Gezielte extrahierte
  Produktionshelper sowie die Adapter-Syntaxpruefung bestehen; Quellen,
  Provideridentitaet und Export-Authorergate sind integriert. r329 ist gebaut,
  der Nutzer bestaetigt Bigs Story bis zum Abspann als Produktwitness.
  Keine gemeinsamen
  Runtime-Header oder Codegeneratoren fuer diesen Crashbatch geaendert.
- r329 baut genau 35 von 1.098 AOT-Dateien neu; 1.063 bleiben bytegleich,
  gemeinsame generierte Header sind unveraendert. Das 32er-Compilegate hat
  vor dem Compiler gestoppt und wurde nach Byte-/Ninja-Pruefung fuer diesen
  Batch auf exakt 35 gesetzt. Hostbuild 1 min 42 s, erfolgreicher CLI-Export
  2 min 17 s, mit Wrapper 3 min 7 s. Einschliesslich abgewiesenem r328-
  Analysecheckpoint, neuer Analyse und Compileplan-Stopp dauerte der gesamte
  Ablauf 16 min 49 s. Das Zehn-Minuten-End-to-End-Ziel bleibt verfehlt;
  dies ist kein Kaltbuild. Der Checkpoint bindet die alte Hintmenge und kann
  die 134 neuen Roots nicht direkt uebernehmen; der normale Pfad verwendet
  den vorhandenen Root-/Modulcache. Der abschliessende Durchlauf trifft den
  neuen Gesamtanalyse-Cache. Alle 249 Module und 6.239 Seed-Eintraege sind
  vorhanden, insgesamt 963.404 Dispatch-Eintraege. Drei weggefallene alte
  Blockstarts, zwei verkuerzte Spans und 492 Ownerwechsel wurden gesondert
  gegen die erzeugten Bodies und Originalbytes geprueft. Die 15 Primary-
  Ownerwechsel behalten alle exakten Cases und acht bytegleiche Blockbodies.
  Die beiden verkuerzten Spans sind vollstaendige Splits. Drei alte
  Middle-Block-Einstiege in E-101R, ZERO und Egg Viper sind als Instruktionen
  weiterhin vorhanden, verlieren aber ihre exakten externen Dispatchcases.
  Bekannte normale Inbounds fuehren ueber den Vorgaenger; ein fehlender
  Exception-/Retry-Reentry ist damit nicht ausgeschlossen. Die automatische
  monotone Entry-Qualification bleibt daher FAIL, bis diese Verfuegbarkeit
  wiederhergestellt oder ein strenger Nicht-Reentry-Beweis vorhanden ist.
  Die benoetigten internen Fortsetzungen bleiben vorhanden. Im geprueften
  normalen Fluss ist kein konkreter Regressionspfad belegt; r329 wird mit
  dieser dokumentierten statischen Einschraenkung weiter getestet. Der
  erfolgreiche Big-Lauf ersetzt den separaten Nicht-Reentry-Beweis nicht.
- r328 enthaelt den geprueften SIMD-FPU-/FMOV-Gruppenbatch, die generische
  Behandlung wiederverwendeter Texture-Deskriptoren (Gamma Hot Shelter und
  Tails nach Credits) sowie 20 neue quellgebundene Callback-Kandidaten in
  Chaos-6-/Chaos-2-/Chaos-0- und zugehoerigen Eventfamilien. Big vor Chaos 6
  trifft dieselbe ergaenzte Chaos-6-Familie wie Sonic. Keine Proof-Promotion.
  Alle 249 geladenen Module, alle bisherigen Blockanfaenge und alle 250.993
  Primary-Dispatch-Tupel bleiben erhalten. 704 zusaetzliche geladene AOT-Bloecke;
  insgesamt 954.177 Dispatch-Eintraege. 6.105 quellgebundene Seed-Zeilen und
  106 Primary-Kandidaten bestehen die statische Produktpruefung.
  20 Dispatch-Owner verschieben sich innerhalb der neu vervollstaendigten
  Chaos-6-/Chaos-2-Funktionskoerper; kein alter Blockspan aendert sich.
  Vier gezielte FPU-/Codegen-Komponentenpruefungen bestehen. Auf Nutzerwunsch
  kein eigener r328-Spieltest; Crashheilung und Leistungsgewinn bleiben bis
  zum Produktlauf unbestaetigt.
- r328-Export: 43 min 33 s CLI-Lauf, davon 33 min 41 s Hostbuild;
  44 min 24 s einschliesslich Wrapper-Vorlauf. Das Zehn-Minuten-Ziel ist
  klar verfehlt. Zwei vorherige Vorbereitungsversuche endeten vor dem
  Produktcompile (Provideridentitaet, danach inkompatibler alter Checkpoint).
  872/1.087 AOT-Quellen enthalten die neue FMOV-Gruppe; 215 weitere sind
  bytegleich, wurden aber durch den gemeinsamen Header ebenfalls invalidiert.
  Generierter AOT-C++-Umfang 4,031 GB statt 4,019 GB. Ein neuer quellgebundener
  Analyse-Checkpoint ist fuer kompatible Folgeaenderungen gesichert.
- Im r328-Batch ist Amys Stop vor dem Finalboss auf fehlende indizierte
  Originaldateien zurueckgefuehrt. Beide NB/PB-Modellpaare, die alternative
  Event-Audiodatei und der alte indizierte Soundtreiber wurden aus dem vorhandenen
  Disc-Abbild ergaenzt und per SHA-256 verifiziert. Alle 2.067 indizierten Dateien
  sind jetzt mit passender Groesse vorhanden. Ein neuer generischer Installations-
  Preflight verhindert weitere Luecken durch Dateiendungsfilter. Dies ist ein
  Daten-/Quellfix; der Bossuebergang wurde damit noch nicht erneut gespielt.
- Am 9. September 2026 bestaetigt der Nutzer die vollstaendig beendete
  Tails-Kampagne in r327. Der Credits-Screenshot belegt den erreichten
  Abspann, zeigt aber zerhackte Schrift und schwarze Rechtecke. Der
  Kampagnenfortschritt bis zum Abspann ist ein Nutzerwitness; der Nutzer
  meldet anschliessend einen Crash nach den Credits. Credits-Darstellung,
  Abschlussuebergang und andere Kampagnen bleiben separat offen.
  Die Entwicklungsversion wurde auf Nutzeranordnung auf 0.49.4 angehoben
  und nach Abschluss des Crashbatches exportiert.
- Intro, Hauptmenue, Character Select und regulaerer Story-Einstieg sind
  erreichbar. Der Nutzer hat im bisherigen Storypfad Chaos 4 besiegt; danach
  wurde der naechste Film angefordert. Der fruehere Stand vor dem ersten
  Story-Intro ist damit ueberholt. Dieser Nutzerlauf bleibt an r315 gebunden.
- Das Spiel erkennt die VMU-Anbindung. Laden hat Storyfortschritt nach
  vorherigen Stops wiederhergestellt. Vollstaendige Save-/Quicksave- und
  Eventfortsetzung in allen Situationen bleibt ein eigenes Abnahmeziel.
- r316 erhaelt alle 249 gebundenen Loaded-AOT-Module und alle vorherigen
  Primary-Blockanfaenge. Drei neue private Callbackroots erschliessen weitere
  1.822 Loaded-AOT-Blockanfaenge in der Flugsequenz-Familie. Das ist
  Candidate-Fortschritt, keine automatische Proof-Promotion.
- r317 erhaelt diese Abdeckung und erschliesst durch die gesamte belegte
  Kameraindex-Familie weitere 208 Blockanfaenge in SHOOTING und 373 in
  B_E101_R. Die drei neuen Entries sind im Produkt vorhanden; ihre spaeteren
  Spielpfade sind noch nicht durch einen Lauf bestaetigt.
- r316 korrigiert die gemeinsame Objekttexturliste: Das Laden einer
  Charaktertexturliste darf die vom Main-Owner veroeffentlichte Liste nicht
  ueberschreiben. Ein begrenzter Produkttrace bestaetigt die korrigierte
  gemeinsame Bindung. Der Nutzer bestaetigt anschliessend: Die Objekte sind
  gefixt. Diese Objektabnahme ist ein r316-Nutzerwitness, kein automatischer
  Beweis fuer die gesamte Grafikpipeline.
- Alle zehn gebundenen Filmdateien sind im privaten Inhalt vorhanden und
  gegen die Originalquelle geprueft. Die acht Storyfilme wurden mit dem
  nativen Decoder vollstaendig dekodiert. Das ersetzt nicht ihren Nachweis
  innerhalb der jeweiligen Spielsequenz.
- r317 ersetzt die feste Drawanzahlgrenze durch ein Budget der tatsaechlich
  reservierten Queue-/Geometriekapazitaet. Der sichtbare Knuckles-Replay
  passiert den Film und den bisherigen Draw-Limit-Abbruch; die Szene am
  Master Emerald laeuft. Nach 145,5 Sekunden folgt ein neuer Stop beim
  naechsten Film, dessen Hook einen noch offenen Grafikframe ablehnt.
- Beide belegten SDK-Textur-Tailcalls in Primary und Casinopolis sind jetzt
  mit ihren exakten Callerbytes und aktiven Ownern gebunden. Der Trial-Replay
  uebersteht mit derselben EXE 301,6 Sekunden, erreicht aber den Abschluss
  nicht: Sonic verbleibt nach mehreren Toden unter einem Steg in Emerald
  Coast. Der eigentliche Rueckkehrfix bleibt zur Laufabnahme offen. Beide
  Runs verwenden isolierte Spielstandkopien; der exakte urspruengliche
  Vorzustand der Aufzeichnungen ist nicht belegt.
- r318 erhaelt alle 249 Loaded-AOT-Modulbindungen, alle 250.699 Primary-
  und alle bisherigen 699.496 Loaded-Blockanfaenge. Eine quellgebundene
  Parent-/Child-Aufraeumfamilie ergaenzt elf Blockanfaenge in SHOOTING;
  der im Nutzercrash fehlende Cleanup-Entry ist jetzt vorkompiliert.
  Die Familieninventur umfasst 250 Programmabbilder. Dies bleibt
  Candidate-Fortschritt ohne automatische Proof-Promotion.
- r318 schliesst beim Movieaufruf den vorhandenen nativen Grafikframe ab,
  ohne einen zusaetzlichen Simulationstick oder Storyskip. Der sichtbare
  Knuckles-Replay passiert den bisherigen Uebergabestop, spielt den
  Folgefilm vollstaendig und erreicht Station Square. Der angeforderte
  Fuenf-Minuten-Lauf endet ohne Crash; inklusive Abschlussarbeiten sind es
  308,5 Sekunden. Die gezielte Moviefortsetzung ist damit bestaetigt,
  nicht die gesamte Knuckles-Story.
- r319/r320 erhalten alle 249 Loaded-AOT-Modulbindungen. r320 enthaelt
  250.806 Primary- und 699.851 Loaded-Blockanfaenge: gegen r319 kommen 107
  beziehungsweise 261 hinzu, ohne einen bisherigen Entry oder eine
  Sourcebindung zu entfernen. Alle 99 privaten Primary-Candidates und
  6.253 Loaded-Seedrecords sind im erzeugten Produkt vorhanden. Neue
  Taskzustands-, Auxwork-, Timer- und Indexfamilien bleiben Candidates;
  die quellseitige Inventur umfasst 250 Programmabbilder.
- Der r320-Batch enthaelt Korrekturen fuer die gemeinsam ausgewerteten
  r319-Nutzercrashes von Sonic, Tails, Knuckles, Amy und Gamma. Ein
  zusaetzlicher Texturfix erkennt eine inzwischen fremd belegte, noch nicht
  geladene SDK-Registryzeile als verdraengte alte Hostbindung; Freigabe und
  Wiederladen ueberschreiben diese fremde Zeile nicht. Die betroffenen
  Laufpfade sind noch nicht abgenommen. Auf aktuelle Nutzeranweisung wurde
  r320 nur gebaut und nicht vom Agenten gestartet.
- r321 ist als Performance-NativeBringup-Produkt gebaut. Alle 249 geladenen
  Modulbindungen und alle bisherigen Primary-Dispatchentries bleiben erhalten.
  Das Produkt enthaelt 250.862 Primary- und 699.948 Loaded-Blockanfaenge sowie
  alle 6.256 geladenen Seedrecords. Die vier vorherigen fehlenden Crashentries
  und Gammas direkter Primary-Helfer sind statisch vorhanden. Bei SBOARD wird
  ein frueherer Innenroot in die vollstaendige Funktion aufgenommen; seine
  Instruktion bleibt im AOT enthalten, die Epilogaufteilung wurde an den
  Originalbytes geprueft. Das ist keine pauschale Laufabnahme.
- r322 erhaelt alle 249 Loaded-AOT-Modulbindungen, jeden bisherigen Loaded-
  Blockanfang samt Bytebereich und alle bisherigen Primary-Dispatchtuples.
  Der Pack enthaelt 1.086 Partitionen, 18.965 Funktionen und 304.710 Bloecke;
  die Dispatchtabelle waechst von 951.746 auf 953.473 Eintraege. Alle 106
  privaten Primary-Candidates und die sourcegebundenen neuen Loaded-Roots
  sind vorhanden. 71 Ownerwechsel liegen ausschliesslich in ADV00: Der
  vollstaendige Callback 29EA nimmt den frueheren Innenroot und seine
  gemeinsame Rueckkehrroutine auf. Alte Resume-PCs behalten ihren jeweiligen
  Eintrittszustand; kein alter Block und keine Gastoperation entfallen.

## Offene Produktfragen

- Der in r321 enthaltene r320-Nutzerbatch betrifft Gamma, den Sky-Chase-2-Boss, Tails,
  Knuckles und Amy. Vier Faelle betreffen AOT-Entries; Amy endet im
  Grafikadapter. Die Gamma-Ursache betrifft eine gemeinsame Primary-
  Funktion mit vier belegten Aufrufern: Die strukturelle Erkennung hat einen
  gueltigen Delayslot ausserhalb ihres 32-Instruktions-Prueffensters verworfen.
  Der Sourcefix prueft diesen gegen die tatsaechliche Image-/Segmentgrenze;
  alle bisherigen Opcode-, Owner- und Entrypruefungen bleiben erhalten.
  Die neuen Entries und ihr direkter Helfer sind jetzt kompiliert; die
  jeweilige Laufabnahme bleibt getrennt. Die sourcegebundenen
  Sky-Chase-Child- und Hub-Zustandsfamilien wurden ueber 250 Images geprueft;
  ihre vorhandenen Geschwister bleiben erhalten. Der vorher fehlende
  SBOARD-Effektentry ist ebenfalls im Produkt enthalten.
- Die SDK-Clippingsemantik erlaubt sechs Ausgabepunkte aus vier
  Eingangspunkten. Der native Puffer fuer diesen Fall waechst von fuenf auf
  sechs Punkte, mit zwoelf statt neun Listvertices. Reihenfolge und Winding
  bleiben erhalten. Der konkrete Amy-Abbruchzweig ist in der alten Capsule
  nicht belegt; neue Fehlertranskripte behalten Zweig, Counts, Flags und
  Kameratiefen. Diese Aenderung ist in r321, der urspruengliche Amy-Abbruch
  ist weiterhin nicht eindeutig diesem Zweig zugeordnet.
- Der abgeschlossene r321-Nutzerbatch enthaelt Gamma, Sonic, einen Stop vor
  Chaos 6, Tails, Knuckles und Amy. Die ersten vier Faelle betreffen weitere Funktionsentries:
  zwei ADV03-Callbacks mit drei gemeinsamen Helfern, ein Primary-Finalizer,
  beide fehlenden Arme einer Chaos-6-Childfamilie und der Folgecallback des
  SBOARD-Effekts. Amy benoetigt einen echten kurzen Task-Finalizer in STG12.
  Diese Familien sind in r322 exportiert. Bei Knuckles lag keine bewiesene
  falsche Generation vor: Ein SDK-verwalteter TEXLIST-Traeger im Heap wurde
  faelschlich wie Code innerhalb des Loaded-AOT-Images geprueft. Der Fix
  trennt beide bereits gebundenen Traegerarten und behaelt RAM-, Owner-,
  Referenzzaehler-, Epoch- und Ueberschreibpruefungen bei. Die jeweiligen
  spaeten Nutzerpfade bleiben bis zum tatsaechlichen Erreichen offen.
- Der globale Callbackscan klassifiziert alle 36 gefundenen fehlenden
  Storekandidaten in 250 Images: 29 Stores gehoeren zu 22 Callbackfamilien,
  sieben zu Daten. Einschliesslich belegter Folgecallbacks ergaenzt der
  globale Vertrag sechs Primary- und 37 Loaded-Roots. Seine 100 bestehenden
  Primary-Candidates bleiben erhalten; insgesamt sind es jetzt 106.
  Nicht bewiesene dynamische Zielmengen bleiben fail-closed. Es gibt keine
  automatische Proof-Promotion. Der Canonical-Coverage-Builder
  revalidiert vorhandene Disassembly-Daten bei unveraenderter CLI und
  unveraenderten Images. Der gemessene Erweiterungslauf dauert 7,6 Sekunden
  und startet keinen neuen Analyzer- oder Compilerprozess.
- Der Nutzer hat den Batch geschlossen und anschliessend autonome Builds,
  FPS-Messungen und gesteuerte 60-Sekunden-Leveltests freigegeben. Die aktuelle
  32er-Matrix verwendet r322, Profil 1, sichtbare Ausgabe, 144-Hz-Anforderung
  und isolierte Saves. 31 Kombinationen bestehen die volle 60-Sekunden-Probe;
  Sonic Final Egg wechselt nach 58,364 Sekunden den Stage-/Owner-Zustand und
  besteht das Zeitfenster deshalb nicht. Alle 32 Laeufe bleiben ohne Capsule.
  Das bestaetigt weder ganze Stages noch spaete Storyfortsetzungen.
  Die langsamsten vollstaendigen Fenster sind Sonic Twinkle Park (21,90),
  Knuckles Lost World (22,23), Amy Twinkle Park (22,39), Sonic Windy Valley
  (24,20), Tails Windy Valley (24,33) und Emerald Coast (24,52 Sim-FPS).
  Danach werden nur betroffene Performance-/Regressionspfade wiederholt.
  Ein spaeterer langsamer Abschnitt in Amys Hot Shelter ist durch den
  anfangs stabilen Debugabschnitt nicht abgenommen.
- r323 verwendet denselben AOT-Pack und bytegleiche generierte Quellen.
  Der Mikrobuild kompiliert genau 32 gemessene AOT-Einheiten neu und dauert
  einschliesslich Produktkopie 126 Sekunden. Keine neue Analyse oder
  Codegenerierung wird aufgerufen. Ein feinerer exakter Speicherbereichsindex
  und Clang/ThinLTO fuer die Runtime wurden im Produkt verglichen. r324 kehrt
  bei identischem AOT-Pack zur MSVC-Runtime zurueck; dieser Mikrobuild dauert
  96 Sekunden bei null AOT-Compiles. Die Messungen zeigen Gewinne, Verluste
  und erhebliche Schwankungen bei Wiederholungen derselben EXE. Ein allgemeiner
  Vorteil des Compilerwechsels ist nicht belegt; MSVC bleibt der Runtime-Default.
  Die gezielten Speicher-/Alias- und FPU-Tests bestehen; die native Linkpruefung
  schliesst weiterhin Decoder, Interpreter und historische Geraeteemulation aus.
- r325 bindet den vorhandenen AVX2/FMA-Matrixpfad in den normalen FTRV-Aufruf
  ein und ergaenzt hardwaregestuetzte FIPR-/FMAC-Pfade. CPUID/OSXSAVE/XGETBV,
  Rundungsmodus, Denormalbehandlung und nichtendliche Fallbacks bleiben erhalten.
  Bitgenaue Vergleiche mit unabhaengigen skalaren Implementierungen bestehen
  auch fuer Ueberlauf, signed zero, NaN, ueberlappende Register und FPSCR-Flags.
  Das erhaelt den bisherigen Katana-Rechenvertrag; es behauptet keine neue
  numerische Gleichheit mit Flycasts abweichender FIPR-/FTRV-Akkumulation.
  Der kanonische Exportwrapper bietet jetzt einen gebundenen Mikrobuild-Pfad:
  aktuelles Runtimebuild, eingefrorene Runtime-/ABI-Hashes, bytegleiche
  generierte Quellen, begrenzter Ninja-Plan und normale native Linkpruefung.
  Der r325-Produktbuild dauert 51,6 Sekunden bei null AOT-Compiles, ohne neue
  Analyse. Dies ist ein inkrementeller Nachweis, kein Kaltexport.
- Alle sechs sichtbaren r325-Wiederholungen bestehen 60 Sekunden ohne Capsule.
  Gemessene Sim-FPS: Emerald Coast 28,55; Amy Hot Shelter 29,62; Sonic Windy
  Valley 23,74; Sonic Twinkle Park 21,44; Amy Twinkle Park 22,51; Knuckles
  Lost World 20,38. Die Praesentation liegt in diesen Fenstern bei 135--143 FPS.
  Es gibt keinen belegten allgemeinen Leistungspuffer: Der staerkste dauerhaft
  aktive Thread benoetigt im Mittel etwa 39,7--46,3 ms pro neuem Bild in den
  vier langsamen Szenen. Thread-Endpunktmessungen beweisen weder P95 noch die
  exklusive Simulationsarbeit. Amy Hot Shelter bleibt auf den Einstieg begrenzt.
- r326 haelt die Adressregister bei den sechs FMOV-Speicherformen im nativen
  Registercache. Observer-, Provider- und Fehlerpfade veroeffentlichen den
  aktuellen Zustand weiterhin vor der Uebergabe. Normale Single-Float-
  Vergleiche benoetigen keinen Wechsel der Host-FPU-Rundung mehr; Sonderwerte
  und Double-Float behalten den bisherigen Pfad. Vier gezielte Komponenten-
  und generierte Native-Tests bestehen, einschliesslich Teilzugriffsfehlern,
  Delay-Slot-Ausnahmen und mutierenden Observer-/Provider-Callbacks.
  Alle 249 Modulbindungen und 953.473 Dispatchentries bleiben unveraendert.
- Sechs sichtbare r326-Gameplayfenster bestehen jeweils 60 Sekunden ohne
  Capsule: Windy Valley 20,51; Emerald Coast 26,68; Sonic Twinkle Park 22,26;
  Knuckles Lost World 21,41; Amy Twinkle Park 22,70; Amy Hot Shelter 29,24
  Sim-FPS. Die Praesentation liegt bei 134--138 FPS. Zwei gezielte Kontrollen
  mit r325 und zwei mit r326 zeigen erhebliche Streuung: Windy Valley erreicht
  mit derselben r326-EXE auch 23,39 Sim-FPS. Emerald Coast bleibt in den
  r326-Messungen langsamer als r325. Ein allgemeiner oder stabiler Gewinn ist
  nicht belegt; 30-Hz-Leistungspuffer und Amys spaeter Abschnitt bleiben offen.
  Diese Debugfenster ersetzen weder Storytests noch das strikte Produktgate.
- Die geaenderte AOT-Semantik von r326 erfordert 1.086 AOT-Neukompilierungen.
  Der gesamte Wrapper dauert 44 Minuten 30 Sekunden; davon entfallen
  38 Minuten 40 Sekunden auf den Hostbuild. Der Lauf verwendet vorhandene
  Analyseartefakte und ist kein sauberer Kaltnachweis. Das Zehn-Minuten-Ziel
  bleibt deutlich verfehlt. Eine isolierte Compileanalyse ordnet etwa 95
  Prozent der Compilerzeit dem Backend zu; Headerverarbeitung ist in dieser
  Probe kein grosser Hebel. Das ist ein Compilerbefund, kein Produktgewinn.
- r327 erhaelt den nativen Registercache auch ueber zwoelf nicht speichernde
  FPU-Operationen. Explizite FPUL-/T-Bruecken verhindern, dass ein FCNVDS-
  Ergebnis innerhalb einer FPU-Epoche durch einen alten Cachewert ersetzt
  wird. Der neue Native-Test reproduziert diesen Fehler vor der Korrektur;
  danach bestehen die drei betroffenen FPU-/Codegen-Targets. Veraendernde
  Zyklusprovider erhalten auch an nicht-FMOV-Speichergrenzen den aktuellen
  Zustand. Modulbindungen und alle 953.473 Dispatchentries bleiben erhalten.
- Alle sechs sichtbaren r327-Bewegungsfenster bestehen 60 Sekunden ohne
  Capsule: Windy Valley 20,88; Emerald Coast 26,39; Sonic Twinkle Park 23,29;
  Knuckles Lost World 21,95; Amy Twinkle Park 23,08; Amy Hot Shelter 29,63
  Sim-FPS. Die Praesentation liegt bei 132--142 FPS. Das sind gegen r326
  ueberwiegend kleine Verbesserungen bei leicht niedrigerem Emerald Coast;
  angesichts der vorherigen Streuung ist kein allgemeiner Leistungspuffer
  belegt. Amys spaeter langsamer Abschnitt bleibt ungeprueft. Der komplette
  Wrapper dauert 40 Minuten 44 Sekunden, der Hostbuild 34 Minuten 26 Sekunden
  bei 16 Jobs. Dies bleibt ein voller AOT-Neubau mit vorhandenen Analyseinputs,
  kein Kaltnachweis und keine Erfuellung des Zehn-Minuten-Ziels.
- Sky Chase, Chao Garden und weitere noch nicht erreichte Storyfortsetzungen
  sind nicht pauschal freigegeben. Ein neuer unbekannter Block endet weiterhin
  fail-closed; ein fehlender Crashrecord ist kein Beweis fuer Fehlerfreiheit.
- Der neue Sky-Chase-Cleanup ist im r318-Produkt gebunden, aber noch nicht
  im betroffenen Spielpfad abgenommen. Der Nutzerreplay weicht mit der
  vorhandenen Savekopie nach Mystic Ruins ab und wird gezielt beendet.
  Dieser Lauf gilt weder als Sky-Chase-Crashabnahme noch als HUD-Diagnose.
  Der mechanische Selektorkatalog enthaelt Sky Chase; der aktuelle
  direkte Launcher bietet jedoch nur Action Stages und einen nicht
  startfaehigen Eventvorschau-Eintrag. Ein gepruefter Sky-Chase-Einstieg
  fehlt. HUD und Fadenkreuz bleiben zur Diagnose offen; die begrenzte,
  standardmaessig ausgeschaltete Spritebeobachtung unterscheidet jetzt
  korrekt zwischen gebundenen und ungebundenen Texturen.
- Untertitel und Rueckblicktexte bleiben zur Nutzerabnahme offen. Die
  Diagnose zeigt wechselnde Pixelpuffer bei wiederverwendeter nativer Ansicht
  und wachsendem SDK-Referenzzaehler. r320 bindet die drei belegten dynamischen
  Textfreigabe-Caller an die vorhandene kompilierte SDK-Freigabe. Erst nach
  deren tatsaechlicher Registryfreigabe werden alte Hostansichten verworfen;
  der Folgetext kann neu hochgeladen werden. Die Diagnose war kein visueller
  Nachweis einer fortschreitenden Cutscene, und r320 hat noch keinen Laufpass.
- Der gemeinsame Objektcluster ist in r316 durch den Nutzer bestaetigt;
  Menuegrafik und andere nicht erneut beurteilte Grafikpfade bleiben offen.
  Bestehende Shader-, Tiefen-,
  Alpha- und Reihenfolgevertraege werden nicht durch pauschale Overrides
  ersetzt. Die lokale Flycast-Referenz und der originale Datenfluss bleiben
  Vergleichsbasis.
- Stabile 30 Simulationsbilder pro Sekunde sind noch nicht durchgehend
  erreicht. Der r317-Trial-Replay liefert ueber seine gesamte Laufzeit rund
  18,4 Simulations- und 142,4 Praesentationsbilder pro Sekunde, einschliesslich
  Start, Menues, Laden und Gameplay. Das ist keine reine Levelmessung und
  kein kompatibler Vergleich zur vorigen Casinopolis-Diagnose.
- r317 benoetigt 594,9 Sekunden im CLI-Export, zusaetzlich zur Wrapper-
  Vorpruefung. Der Gesamtlauf bleibt ueber zehn Minuten. Das ist ein
  inkrementeller Lauf mit Caches und zehn AOT-Compiles, kein Kaltnachweis.
  Countabhaengige Sharddeklarationen stehen jetzt nur im Aggregator; kuenftige
  Ergaenzungen dieser Liste invalidieren damit nicht mehr den gemeinsamen
  Header. Die Umstellung selbst baut dessen bisherige Verbraucher einmal
  neu. Eine gemessene Zeitersparnis durch diese Entkopplung steht noch aus.
- r318 benoetigt 485,6 Sekunden im CLI und 564,3 Sekunden fuer den gesamten
  Export-Wrapper einschliesslich Vorpruefung und Abschlussarbeiten. Die
  vorherige gezielte Komponentenverifikation ist darin nicht enthalten.
  Das ist ein inkrementeller Cachelauf unter zehn Minuten, kein Kaltexport.
  1.083 von 1.084 generierten AOT-Einheiten kommen aus dem Codegen-Cache;
  tatsaechlich werden eine AOT-Einheit, 58 Dispatch-Shards, ein Loaded-Shard
  und drei weitere C++-Dateien kompiliert. Der Hostbuild dauert 107,2 Sekunden.
  Der gemeinsame Dispatchheader bleibt unveraendert. Ein weiterer
  Bauzeitverlust ist konkret belegt: Elf neue Eintraege verschieben die
  festen 8.192-Zeilen-Grenzen von 58 Dispatchdateien. r319 implementiert eine
  Aufteilung nach stabilen Adresspraefixen mit hoechstens 8.192 Eintraegen
  pro Blatt; sein einmaliger Wechsel baut 188 Dispatchdateien. Der gesamte
  r319-Wrapper dauert 644,2 Sekunden und verfehlt das Zehn-Minuten-Ziel.
- Der erfolgreiche r320-Export dauert 513,7 Sekunden einschliesslich
  Vorpruefung und Packaging; davon entfallen 457,1 Sekunden auf den CLI-Lauf
  und 64,0 Sekunden auf den Hostbuild. Der Ninja-Plan umfasst neun
  C++-Compiles: drei AOT-Einheiten, zwei Dispatch-Shards, einen Loaded-Shard
  und drei weitere Dateien. 1.081 von 1.084 AOT-Einheiten kommen aus dem
  Codegen-Cache. Dieser inkrementelle Lauf verwendet auch Artefakte eines
  zuvor gestoppten Versuchs; dessen Zeit sowie gescheiterte Vorpruefungen
  sind in den 513,7 Sekunden nicht enthalten. Das ist kein Kaltnachweis.
- Der erfolgreiche r321-Wrapper dauert 613,5 Sekunden, davon 535,7 Sekunden
  im CLI. Der Hostbuild umfasst 59,4 Sekunden; Kompilieren und Linken davon
  56,7 Sekunden. Zehn von 1.084 AOT-Einheiten und insgesamt 21 C++-Dateien
  werden kompiliert. Die fruehere fehlgeschlagene Vorpruefung und der
  CLI-Neubau sind nicht enthalten. Der inkrementelle Export verfehlt das
  Zehn-Minuten-Ziel; ein Kaltnachweis fehlt. Der groesste Zeitanteil bleibt
  Analyse und Validierung. Kleine belegte Callbackfamilien sollen die
  vorhandene CLI und bytegebundene Erkenntnisse weiterverwenden; allgemeine
  Analyzeraenderungen werden nur gebuendelt mit ihrem realen Bedarf gebaut.
- Acht unveraenderte alte Candidate-Familien mit 534 Roots haben jetzt
  einen gebundenen Quellcheckpoint. Er bewahrt die Originalaudits und
  verifiziert die lebenden Quellen, ohne geloeschte alte Portverzeichnisse
  vorauszusetzen. Historische Missingness bleibt historisch; die aktuelle
  CLI prueft ihre eigenen Image-, Source-, Block- und Ownerbindungen weiter.

## Entwicklungsablauf

Verbindlich sind `AGENTS.md`, `NATIVE_BRINGUP_WORKFLOW.md` und
`NATIVE_PORT_PRODUCT_CONTRACT.md`, unter Vorrang aktueller Nutzeranweisungen.

1. Aktuelle Witnesses und typisierte Stops nach gemeinsamer Ursache
   zusammenfassen. Das gesamte gebundene Spiel nach derselben Familie pruefen;
   aus einer Crashadresse allein entsteht kein Ausfuehrungsrecht.
2. Generische Regeln bei einem belegten adressunabhaengigen Muster verwenden.
   Titelbezogene Familien bleiben privat, source-/byte-/imagegebunden und
   werden in ihrer aktiven Generation validiert. Dynamische Restmengen bleiben
   offen. Vorhandene Evidence wird wiederverwendet.
3. Nur konkrete disjunkte Arbeit delegieren. Keine automatische Fleet-
   Auslastung, doppelten Vollreviews oder Pflicht zum Warten auf unbenutzte
   Tasks. Der Haupttask finalisiert Approach, Implementierung und Abnahme.
4. Kompatible Fixes vor einem Performance-Produktbuild buendeln. Reine native
   Host-/Adapterfehler verwenden den gueltigen AOT-Pack. Neue Roots oder
   geaenderte AOT-Semantik erfordern die grosse Schleife. Der echte Ninja-Plan
   begrenzt die neu kompilierten AOT-Dateien vor Compilerstart.
5. Mit derselben EXE den betroffenen Sonic-Pfad gezielt pruefen: Debuglaeufe
   hoechstens 60 Sekunden, Storylauf hoechstens fuenf Minuten. Vollmatrizen
   sind kein Standardgurt; Grafik-/Engineaenderungen koennen begruendete
   breitere Abdeckung erfordern. Historische Passes behalten ihre Build-ID.
6. Nach funktionierendem Produkt ohne bekannten Fortschrittsrueckschritt den
   geprueften Source-Batch lokal committen. Kein Push. Retaildaten,
   Spielstaende, private Titelvertraege und Buildartefakte bleiben ausserhalb
   des oeffentlichen Repositorys.

## Implementierter Unterbau und Beweisgrenzen

Der Bring-up-Dispatch validiert aktive vorkompilierte Blockanfaenge gegen
versiegelte Identitaeten, aktuelle Owner und Generationen, einschliesslich
Loaded AOT. Native Audio-, Grafik-, Datei-, Eingabe-, Movie- und Saveprovider
ersetzen belegte Plattformvertraege. Crash Capsules korrelieren unter anderem
Providerfehler, Kontrollfluss und Loaded-AOT-Identitaet; Rohadressen oder
Laufzeitbeobachtungen schliessen keine statische Frontier.

`Observed -> Candidate -> Proven | RuntimeContract -> Strict Product` bleibt
die Evidence-Grenze. Eingabereplays sind nur mit gebundenem Vorzustand und
tatsaechlich beobachtetem Meilenstein ein Reproduktionsnachweis. Vollstaendiges
deterministisches Provider-Replay, automatische First-Divergence-Lokalisierung
und universelle Spiel-/Grafikabdeckung sind nicht als fertig abgenommen.
