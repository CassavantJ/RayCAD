%
O01102 (Matrix: arcs in the XZ G18 and YZ G19 planes)
(Document post matrix setup Op 1 top)
(Machine Test VMC Haas NGC)
(T2 D6. ballEndMill 6 mm ball end mill)
G21
G17 G40 G49 G80 G90 G94
(XZ and YZ arcs)
G00 G53 G49 Z0.
(T2 6 mm ball end mill in CAT40 ER32)
T2 M06
G21
G00 G90 G54 G17 X0. Y0.
S8000 M03
G43 H2 Z25.
M08
X10. Y10.
Z2.
G01 Z0. F200.
G18 G02 X30. Z0. I10. K0. F600.
G03 X10. Z0. I-10. K0.
G19 G02 Y30. Z0. J10. K0.
G03 Y10. Z0. J-10. K0.
G00 Z25.
G00 G53 G49 Z0.
M05
M09
G00 G53 Y0.
M30
%
