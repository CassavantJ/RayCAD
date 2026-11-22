%
O02001 (Realistic: bracket top, six tools)
(Document post matrix setup Op 1 top)
(Machine Test VMC Haas NGC)
(T1 D50. faceMill 50 mm face mill)
(T2 D10. flatEndMill 10 mm 3FL end mill)
(T3 D10. spotDrill 10 mm 90 deg spot drill)
(T4 D5. drill 5 mm drill)
(T5 D6. tap M6x1 tap)
(T6 D10. chamferMill 10 mm 90 deg chamfer mill)
G21
G17 G40 G49 G80 G90 G94
(Face top)
G00 G53 G49 Z0.
(T1 50 mm face mill in CAT40 ER32)
T1 M06
T2
G21
G00 G90 G54 G17 X0. Y0.
S2500 M03
G43 H1 Z25.
M08
X-30. Y12.
Z22.
G01 Z20. F400.
X110. F1200.
Y38.
X-30.
G00 Z35.
G00 G53 G49 Z0.
M05
M09
(Contour outside)
(T2 10 mm 3FL end mill in CAT40 ER32)
T2 M06
T3
G21
G00 G90 G54 G17 X0. Y0.
S6000 M03
G43 H2 Z25.
M08
X-10. Y-10. Z35.
Z22.
G01 Z12. F300.
G41 X0. Y-5. D2 F900.
Y45.
G02 X5. Y50. I5. J0.
G01 X75.
G02 X80. Y45. I0. J-5.
G01 Y5.
G02 X75. Y0. I-5. J0.
G01 X-5.
G40 X-10. Y10.
G00 Z35.
G00 G53 G49 Z0.
M05
M09
(Spot holes)
(T3 10 mm 90 deg spot drill in CAT40 ER32)
T3 M06
T4
G21
G00 G90 G54 G17 X15. Y15.
S1500 M03
G43 H3 Z35.
M08
G81 G99 X15. Y15. Z16.7 R22. F100.
X35. Z17.
X55. Z16.
X70. Y12. Z17.5
G98 Y38. Z11.5 R16.
G99 X52. Z17.5 R22.
X35. Y35. Z15.
G98 X15. Z16.7
G80
G00 G53 G49 Z0.
M05
M09
(Drill 5 mm with dwell)
(T4 5 mm drill in CAT40 ER32)
T4 M06
T5
G21
G00 G90 G54 G17 X15. Y15.
S2000 M03
G43 H4 Z35.
M08
G82 G99 X15. Y15. Z-2.502 P0.300 R22. F160.
X35. Z6.197
X55. Z10.
X70. Y12. Z-2.502
G98 Y38. Z4.498 R16.
G99 X52. R22.
X35. Y35. Z-2.502
G98 X15.
G80
(Chip-break drill)
G00 G90 G54 G17 X35. Y35.
S2400 M03
Z35.
G73 G99 X35. Y35. Z-2.502 Q1.5 R22. F200.
X52. Y38. Z4.498
G98 X70. Y12. Z-2.502
G80
G00 G53 G49 Z0.
M05
M09
(Tap M6x1)
(T5 M6x1 tap in CAT40 ER32)
T5 M06
T6
G21
G00 G90 G54 G17 X52. Y38.
S500 M03
G43 H5 Z35.
M08
G84 G99 X52. Y38. Z6. R22. F500.
G98 X70. Y12. Z-3.
G80
G00 G53 G49 Z0.
M05
M09
(Chamfer top edge)
(T6 10 mm 90 deg chamfer mill in CAT40 ER32)
T6 M06
G21
G00 G90 G54 G17 X0. Y0.
S8000 M03
G43 H6 Z25.
M08
X40. Y-8. Z35.
Z22.
G01 Z19. F300.
G03 X40. Y-8. I0. J33. F700.
G00 Z35.
G00 G53 G49 Z0.
M05
M09
G00 G53 Y0.
M30
%
