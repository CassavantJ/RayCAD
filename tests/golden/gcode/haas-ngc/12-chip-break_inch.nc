%
O01112 (Matrix: chip-break drilling G73)
(Document post matrix setup Op 1 top)
(Machine Test VMC Haas NGC)
(T4 D0.1969 drill 5 mm drill)
G20
G17 G40 G49 G80 G90 G94
(chip-break drilling G73)
G00 G53 G49 Z0.
(T4 5 mm drill in CAT40 ER32)
T4 M06
G20
G00 G90 G54 G17 X1.378 Y1.378
S2400 M03
G43 H4 Z1.378
M08
G73 G99 X1.378 Y1.378 Z-0.0985 Q0.0591 R0.8661 F7.87
X2.0472 Y1.4961 Z0.1771
G98 X2.7559 Y0.4724 Z-0.0985
G80
G00 G53 G49 Z0.
M05
M09
G00 G53 Y0.
M30
%
