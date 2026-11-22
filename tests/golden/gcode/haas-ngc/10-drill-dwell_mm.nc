%
O01110 (Matrix: drilling with dwell G82)
(Document post matrix setup Op 1 top)
(Machine Test VMC Haas NGC)
(T4 D5. drill 5 mm drill)
G21
G17 G40 G49 G80 G90 G94
(drilling with dwell G82)
G00 G53 G49 Z0.
(T4 5 mm drill in CAT40 ER32)
T4 M06
G21
G00 G90 G54 G17 X15. Y15.
S2000 M03
G43 H4 Z35.
M08
G82 G99 X15. Y15. Z-2.502 P0.300 R22. F160.
X35. Z6.197
X55. Z10.
X70. Y12. Z-2.502
G98 Y38. Z4.498 R16.
G99 X52. R22.
X35. Y35. Z-2.502
G98 X15.
G80
G00 G53 G49 Z0.
M05
M09
G00 G53 Y0.
M30
%
