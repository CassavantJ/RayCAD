%
O01105 (Matrix: cutter compensation G41 and G42 with linear leads)
(Document post matrix setup Op 1 top)
(Machine Test VMC Haas classic)
(T4 D12. flatEndMill 12 mm 4FL end mill)
G21
G17 G40 G49 G80 G90 G94
(Contour with comp)
G00 G53 G49 Z0.
(T4 12 mm 4FL end mill in CAT40 ER32)
T4 M06
G21
G00 G90 G54 G17 X0. Y0.
S5000 M03
G43 H4 Z25.
M08
X-10. Y-10.
Z2.
G01 Z-5. F200.
G41 X0. Y-5. D14 F600.
Y40.
G02 X10. Y50. I10. J0.
G01 X50.
Y0.
X-5.
G40 X-10. Y10.
G00 Z2.
X70. Y-10.
G01 Z-5. F200.
G42 X60. Y0. D14 F600.
Y30.
X80.
G40 X90. Y40.
G00 Z25.
G00 G53 G49 Z0.
M05
M09
G00 G53 Y0.
M30
%
