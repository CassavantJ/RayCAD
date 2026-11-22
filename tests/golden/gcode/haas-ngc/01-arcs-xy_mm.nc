%
O01101 (Matrix: XY arcs in four quadrants, full circles, 135 and 270 degrees)
(Document post matrix setup Op 1 top)
(Machine Test VMC Haas NGC)
(T1 D10. flatEndMill 10 mm 3FL end mill)
G21
G17 G40 G49 G80 G90 G94
(XY arcs)
G00 G53 G49 Z0.
(T1 10 mm 3FL end mill in CAT40 ER32)
T1 M06
G21
G00 G90 G54 G17 X0. Y0.
S6000 M03
G43 H1 Z25.
M08
X30. Y10.
Z5.
G01 Z-2. F200.
G03 X40. Y20. I0. J10. F800.
X30. Y30. I-10. J0.
X20. Y20. I0. J-10.
X30. Y10. I10. J0.
G02 X20. Y20. I0. J10.
X30. Y30. I10. J0.
X40. Y20. I0. J-10.
X30. Y10. I-10. J0.
G03 X30. Y10. I0. J10.
G02 X30. Y10. I0. J10.
G03 X37.071 Y27.071 I0. J10.
G02 X22.929 Y27.071 I-7.071 J-7.071
G00 Z25.
G00 G53 G49 Z0.
M05
M09
G00 G53 Y0.
M30
%
