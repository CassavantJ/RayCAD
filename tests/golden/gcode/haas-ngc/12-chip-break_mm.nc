%
O01112 (Matrix: chip-break drilling G73)
(Document post matrix setup Op 1 top)
(Machine Test VMC Haas NGC)
(T4 D5. drill 5 mm drill)
G21
G17 G40 G49 G80 G90 G94
(chip-break drilling G73)
G00 G53 G49 Z0.
(T4 5 mm drill in CAT40 ER32)
T4 M06
G21
G00 G90 G54 G17 X35. Y35.
S2400 M03
G43 H4 Z35.
M08
G73 G99 X35. Y35. Z-2.502 Q1.5 R22. F200.
X52. Y38. Z4.498
G98 X70. Y12. Z-2.502
G80
G00 G53 G49 Z0.
M05
M09
G00 G53 Y0.
M30
%
