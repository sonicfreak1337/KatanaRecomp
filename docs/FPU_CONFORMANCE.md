# FPU-Konformanzvertrag v0.25

KatanaRecomp bildet die SH-4-FPU deterministisch auf IEC-60559-Hostoperationen ab.
`FSCA` verwendet die unteren 16 Bit von FPUL als Vollkreis. Die Winkel 0, pi/2,
pi und 3pi/2 sind exakt; andere Ergebnisse duerfen hoechstens `2e-7` absolut vom
Single-Precision-Referenzwert abweichen. `FSRRA` darf innerhalb derselben absoluten
Toleranz vom korrekt gerundeten Wert liegen.

`FIPR` und `FTRV` sichern alle Eingaben vor dem ersten Schreibzugriff. Dadurch sind
ueberlappende FV-Register definiert. XMTRX liegt in XF0 bis XF15 und wird
spaltenweise gelesen. Host-FMA darf Zwischenergebnisse verbessern; Tests verlangen
fuer bekannte Grafikvektoren eine absolute Abweichung von hoechstens `2e-6`.

NaN-Ergebnisse werden auf `0x7FBFFFFF` kanonisiert. FPSCR.DN spuelt denormale
Operanden und Ergebnisse auf vorzeichenbehaftete Null. Nur RM=0 (nearest-even) und
RM=1 (gegen null) sind ausfuehrbar; RM=2/3 sowie PR=1 fuer Grafikoperationen nehmen
vor jeder Registerwirkung den strukturierten Illegal-Instruction-Pfad.

Die Tests setzen diese Grenzen explizit. Dadurch werden abweichende Ergebnisse auf
anderen Hostplattformen als Regression sichtbar, ohne nicht garantierte Bitgleichheit
mit den approximierenden Hardwareinstruktionen zu behaupten.
