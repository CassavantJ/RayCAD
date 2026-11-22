%
O01104 (Matrix: tool changes with pre-select, G43 length offsets, a repeated tool)
(Document post matrix setup Op 1 top)
(Machine Test VMC Haas NGC)
(T1 D50. faceMill 50 mm face mill)
(T5 D6. flatEndMill 6 mm 2FL end mill)
(T3 D10. spotDrill 10 mm spot drill)
G21
G17 G40 G49 G80 G90 G94
(Face)
G00 G53 G49 Z0.
(T1 50 mm face mill in CAT40 ER32)
T1 M06
T5
G21
G00 G90 G54 G17 X0. Y0.
S3000 M03
G43 H1 Z25.
M08
X-30. Y10.
Z2.
G01 Z0. F300.
X110. F1500.
G00 Z25.
G00 G53 G49 Z0.
M05
M09
(Slot A)
(T5 6 mm 2FL end mill in CAT40 ER32)
T5 M06
T3
G21
G00 G90 G54 G17 X0. Y0.
S9000 M03
G43 H25 Z25.
M08
X10. Y20.
Z2.
G01 Z-3. F150.
X60. F700.
G00 Z5.
(Slot B)
G00 Z25.
G90 G54 G17 X0. Y0.
S9000 M03
X10. Y30.
Z2.
G01 Z-3. F150.
X60. F700.
G00 Z25.
G00 G53 G49 Z0.
M05
M09
(Spot)
(T3 10 mm spot drill in CAT40 ER32)
T3 M06
G21
G00 G90 G54 G17 X0. Y0.
S2000 M03
G43 H3 Z25.
M08
X20. Y45.
Z5.
G01 Z-1. F100.
G00 Z25.
G00 G53 G49 Z0.
M05
M09
G00 G53 Y0.
M30
%
