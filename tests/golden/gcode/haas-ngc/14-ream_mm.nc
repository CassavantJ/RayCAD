%
O01114 (Matrix: reaming G85)
(Document post matrix setup Op 1 top)
(Machine Test VMC Haas NGC)
(T10 D6.6 reamer 6.6 mm reamer)
G21
G17 G40 G49 G80 G90 G94
(reaming G85)
G00 G53 G49 Z0.
(T10 6.6 mm reamer in CAT40 ER32)
T10 M06
G21
G00 G90 G54 G17 X15. Y15.
S400 M03
G43 H10 Z35.
M08
G85 G99 X15. Y15. Z-1. R22. F80.
G98 Y35.
G80
G00 G53 G49 Z0.
M05
M09
G00 G53 Y0.
M30
%
