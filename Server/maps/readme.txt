Spēles laukuma konfigurācijas faila sintakse. Pirmajā rindā:
Laukuma izmērs (rindas, kolonas), piemēram: 11 13
Splētāja ātrums (bloki/ticks),
Sprādziena efekta bīstamības laiks (ticks).
Sprādziena attālums (bloki)
Sprādziena atskaites laiks (ticks). Lai autors var aizmukt.


Seko laukuma karte. Elementi katrā laukuma šūnā:
    H cietais bloks,
    S mīkstais bloks,
    1..8 spēlētājs ar attiecīgu numuru,
    B bumba,
    A spēlētāja pārvietošanās ātruma palielināšana +1, 
    R bumbas sprādziena rādiusa palielināšana +1,
    T bumbas atskaites laika palielināšana +1.
    
Šos elemntus izmanto, kad detonējas bumbas:
    @ bumbas lāzera centrs
    - bumbas lāzers horizontāli
    > lāzera gals labajā pusē
    < lāzera gals kreisajā pusē
    | bumbas lāzers vertikāli
    ^ lāzera gals apakšpusē
    v lāzera gals augšpusē

Piemērs konfigurācijai failā:
    6 9 3 5 3 10
    H 1 . . . . A . . 
    . . S S . R H H . 
    T . . 2 . . . H 6 
    . H . . . . . . . 
    H . 3 . . . . 5 . 
    7 . . . 4 H 8 . . 


