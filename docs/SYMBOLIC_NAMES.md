# Symbolische Namen

KatanaRecomp vereinheitlicht ELF- und Map-Symbole in einem deterministischen
`SymbolNameIndex`. Diagnosen behalten die numerische Gastadresse als primaere
Identitaet und ergaenzen, falls beweisbar, einen symbolischen Namen sowie den
Offset innerhalb eines Symbols mit deklarierter Groesse.

Bei mehreren Symbolen an derselben Adresse gilt eine feste Prioritaet:

1. global vor weak, local und unknown
2. function vor object und unknown
3. groesserer deklarierter Bereich
4. lexikographisch kleinerer Name

Ein Symbol ohne deklarierte Groesse gilt nur an seiner exakten Adresse. Dadurch
wird eine beliebige vorherige Marke nicht faelschlich als Name fuer den ganzen
folgenden Adressraum verwendet. Unbekannte Adressen bleiben unveraendert als
`0xXXXXXXXX` beziehungsweise erhalten im JSON ein `null`-Symbol.

Textberichte verwenden `name+0xOFFSET`. Der Kontrollfluss-JSON-Bericht liefert
Name, Symboladresse, Dezimaloffset, Exaktheit, Art und Binding getrennt, sodass
Werkzeuge weder den Namen zerlegen noch die numerische Gastadresse verlieren.
