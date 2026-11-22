%
O01114 (Matrix: reaming G85)
(Document post matrix setup Op 1 top)
(Machine Test VMC Haas NGC)
(T10 D0.2598 reamer 6.6 mm reamer)
G20
G17 G40 G49 G80 G90 G94
(reaming G85)
G00 G53 G49 Z0.
(T10 6.6 mm reamer in CAT40 ER32)
T10 M06
G20
G00 G90 G54 G17 X0.5906 Y0.5906
S400 M03
G43 H10 Z1.378
M08
G85 G99 X0.5906 Y0.5906 Z-0.0394 R0.8661 F3.15
G98 Y1.378
G80
G00 G53 G49 Z0.
M05
M09
G00 G53 Y0.
M30
%
