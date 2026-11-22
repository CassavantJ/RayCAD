%
O01113 (Matrix: boring with dwell G89)
(Document post matrix setup Op 1 top)
(Machine Test VMC Haas NGC)
(T9 D6. boringBar 6 mm boring bar)
G21
G17 G40 G49 G80 G90 G94
(boring with dwell G89)
G00 G53 G49 Z0.
(T9 6 mm boring bar in CAT40 ER32)
T9 M06
G21
G00 G90 G54 G17 X15. Y15.
S900 M03
G43 H9 Z35.
M08
G89 G99 X15. Y15. Z-1. P0.200 R22. F60.
G98 Y35.
G80
G00 G53 G49 Z0.
M05
M09
G00 G53 Y0.
M30
%
