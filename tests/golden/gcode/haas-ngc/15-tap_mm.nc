%
O01115 (Matrix: tapping M6x1, right hand G84 and left hand G74)
(Document post matrix setup Op 1 top)
(Machine Test VMC Haas NGC)
(T15 D6. tap M6x1 tap)
(T16 D6. tap M6x1 LH tap)
G21
G17 G40 G49 G80 G90 G94
(Tap M6x1)
G00 G53 G49 Z0.
(T15 M6x1 tap in CAT40 ER32)
T15 M06
T16
G21
G00 G90 G54 G17 X52. Y38.
S500 M03
G43 H15 Z35.
M08
G84 G99 X52. Y38. Z6. R22. F500.
G98 X70. Y12. Z-3.
G80
G00 G53 G49 Z0.
M05
M09
(Tap M6x1 LH)
(T16 M6x1 LH tap in CAT40 ER32)
T16 M06
G21
G00 G90 G54 G17 X35. Y15.
S500 M04
G43 H16 Z35.
M08
G74 G98 X35. Y15. Z8. R22. F500.
G80
G00 G53 G49 Z0.
M05
M09
G00 G53 Y0.
M30
%
