%
O00042 (Bracket rev B 10 bore 100 check op 1/2 *top*)
(Document post matrix setup Op 1 top vise)
(Machine Test VMC Haas NGC)
(T8 D10. flatEndMill 10mm 3FL end-mill, ALTiN coated m)
G21
G17 G40 G49 G80 G90 G94
(Pocket rough A side)
G00 G53 G49 Z0.
(T8 10mm 3FL end-mill, ALTiN coated m in CAT40 ER32)
T8 M06
G21
G00 G90 G54 G17 X0. Y0.
S6000 M03
G43 H8 Z25.
M08
X10. Y10.
Z2.
G01 Z-1. F200.
G00 Z25.
G00 G53 G49 Z0.
M05
M09
G00 G53 Y0.
M30
%
