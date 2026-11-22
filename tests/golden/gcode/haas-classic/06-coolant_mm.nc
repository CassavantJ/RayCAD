%
O01106 (Matrix: coolant modes flood, mist, through-spindle, air and off)
(Document post matrix setup Op 1 top)
(Machine Test VMC Haas classic)
(T6 D8. flatEndMill 8 mm through-coolant end mill)
(T7 D4. flatEndMill 4 mm end mill)
G21
G17 G40 G49 G80 G90 G94
(Flood)
G00 G53 G49 Z0.
(T6 8 mm through-coolant end mill in CAT40 ER32)
T6 M06
T7
G21
G00 G90 G54 G17 X0. Y0.
S6000 M03
G43 H6 Z25.
M08
X10. Y10.
Z2.
G01 Z-2. F200.
Y30. F800.
G00 Z10.
(Mist)
M09
G00 Z25.
G90 G54 G17 X0. Y0.
S6000 M03
M07
X20. Y10.
Z2.
G01 Z-2. F200.
Y30. F800.
G00 Z10.
(Through spindle)
M09
G00 Z25.
G90 G54 G17 X0. Y0.
S6000 M03
M88
X30. Y10.
Z2.
G01 Z-2. F200.
Y30. F800.
G00 Z10.
(Air)
M89
G00 Z25.
G90 G54 G17 X0. Y0.
S6000 M03
M83
X40. Y10.
Z2.
G01 Z-2. F200.
Y30. F800.
G00 Z10.
(No coolant)
M84
G00 Z25.
G90 G54 G17 X0. Y0.
S6000 M03
X50. Y10.
Z2.
G01 Z-2. F200.
Y30. F800.
G00 Z10.
G00 G53 G49 Z0.
M05
(Flood, next tool)
(T7 4 mm end mill in CAT40 ER32)
T7 M06
G21
G00 G90 G54 G17 X0. Y0.
S10000 M03
G43 H7 Z25.
M08
X70. Y10.
Z2.
G01 Z-1. F100.
G00 Z25.
G00 G53 G49 Z0.
M05
M09
G00 G53 Y0.
M30
%
