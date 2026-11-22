%
O01102 (Matrix: arcs in the XZ G18 and YZ G19 planes)
(Document post matrix setup Op 1 top)
(Machine Test VMC Haas classic)
(T2 D0.2362 ballEndMill 6 mm ball end mill)
G20
G17 G40 G49 G80 G90 G94
(XZ and YZ arcs)
G00 G53 G49 Z0.
(T2 6 mm ball end mill in CAT40 ER32)
T2 M06
G20
G00 G90 G54 G17 X0. Y0.
S8000 M03
G43 H2 Z0.9843
M08
X0.3937 Y0.3937
Z0.0787
G01 Z0. F7.87
G18 G02 X1.1811 Z0. I0.3937 K0. F23.62
G03 X0.3937 Z0. I-0.3937 K0.
G19 G02 Y1.1811 Z0. J0.3937 K0.
G03 Y0.3937 Z0. J-0.3937 K0.
G00 Z0.9843
G00 G53 G49 Z0.
M05
M09
G00 G53 Y0.
M30
%
