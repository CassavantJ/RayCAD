%
O01116 (Matrix: cycle code and values changing inside one run)
(Document post matrix setup Op 1 top)
(Machine Test VMC Haas classic)
(T4 D5. drill 5 mm drill)
G21
G17 G40 G49 G80 G90 G94
(Drill, mixed)
G00 G53 G49 Z0.
(T4 5 mm drill in CAT40 ER32)
T4 M06
G21
G00 G90 G54 G17 X10. Y10.
S2000 M03
G43 H4 Z30.
M08
G81 G99 X10. Y10. Z12. R22. F150.
G80
G00 Z30.
G82 G99 X20. Y10. Z12. P0.200 R22. F150.
X30. Z8. R23.
G80
G00 Z30.
G81 G98 X40. Y10. Z8. R23. F150.
G80
G00 G53 G49 Z0.
M05
M09
G00 G53 Y0.
M30
%
