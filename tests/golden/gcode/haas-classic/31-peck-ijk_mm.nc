%
O01131 (Haas: G83 with I J K peck reduction)
(Document post matrix setup Op 1 top)
(Machine Test VMC Haas classic)
(T4 D6.6 drill 6.6 mm drill)
G21
G17 G40 G49 G80 G90 G94
(G83 with I J K peck reduction)
G00 G53 G49 Z0.
(T4 6.6 mm drill in CAT40 ER32)
T4 M06
G21
G00 G90 G54 G17 X28.75 Y16.5
S2200 M03
G43 H4 Z30.
M08
G83 G99 X28.75 Y16.5 Z-2.983 I3. J0.75 K0.75 R17. F180.
Y38.5
X56.25
G98 Y16.5
G80
G00 G53 G49 Z0.
M05
M09
G00 G53 Y0.
M30
%
