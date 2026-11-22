%
O01131 (Haas: G83 with I J K peck reduction)
(Document post matrix setup Op 1 top)
(Machine Test VMC Haas NGC)
(T4 D0.2598 drill 6.6 mm drill)
G20
G17 G40 G49 G80 G90 G94
(G83 with I J K peck reduction)
G00 G53 G49 Z0.
(T4 6.6 mm drill in CAT40 ER32)
T4 M06
G20
G00 G90 G54 G17 X1.1319 Y0.6496
S2200 M03
G43 H4 Z1.1811
M08
G83 G99 X1.1319 Y0.6496 Z-0.1174 I0.1181 J0.0295 K0.0295 R0.6693 F7.09
Y1.5157
X2.2146
G98 Y0.6496
G80
G00 G53 G49 Z0.
M05
M09
G00 G53 Y0.
M30
%
