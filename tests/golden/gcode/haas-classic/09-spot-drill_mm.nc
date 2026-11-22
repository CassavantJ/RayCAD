%
O01109 (Matrix: spot drilling G81)
(Document post matrix setup Op 1 top)
(Machine Test VMC Haas classic)
(T3 D10. spotDrill 10 mm spot drill)
G21
G17 G40 G49 G80 G90 G94
(spot drilling G81)
G00 G53 G49 Z0.
(T3 10 mm spot drill in CAT40 ER32)
T3 M06
G21
G00 G90 G54 G17 X15. Y15.
S1500 M03
G43 H3 Z35.
M08
G81 G99 X15. Y15. Z16.7 R22. F100.
X35. Z17.
X55. Z16.
X70. Y12. Z17.5
G98 Y38. Z11.5 R16.
G99 X52. Z17.5 R22.
X35. Y35. Z15.
G98 X15. Z16.7
G80
G00 G53 G49 Z0.
M05
M09
G00 G53 Y0.
M30
%
