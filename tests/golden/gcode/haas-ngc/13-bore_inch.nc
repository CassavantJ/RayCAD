%
O01113 (Matrix: boring with dwell G89)
(Document post matrix setup Op 1 top)
(Machine Test VMC Haas NGC)
(T9 D0.2362 boringBar 6 mm boring bar)
G20
G17 G40 G49 G80 G90 G94
(boring with dwell G89)
G00 G53 G49 Z0.
(T9 6 mm boring bar in CAT40 ER32)
T9 M06
G20
G00 G90 G54 G17 X0.5906 Y0.5906
S900 M03
G43 H9 Z1.378
M08
G89 G99 X0.5906 Y0.5906 Z-0.0394 P0.200 R0.8661 F2.36
G98 Y1.378
G80
G00 G53 G49 Z0.
M05
M09
G00 G53 Y0.
M30
%
