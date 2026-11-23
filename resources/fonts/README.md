# Fonts

RayCAD's themes name two font families (`fonts` in `resources/themes/*.json`, PLAN.md 2.1 and 6.5):

| Role | Family | Use | Licence | Source |
|-|-|-|-|-|
| `ui` | Inter | All UI text, 13 px | SIL Open Font License 1.1 | https://github.com/rsms/inter |
| `mono` | JetBrains Mono | Numbers, coordinates and G-code | SIL Open Font License 1.1 | https://github.com/JetBrains/JetBrainsMono |

The font files are not in the repository yet. When they are added (a later task), this folder gets the font
files, each family's `OFL.txt` licence text, and a row per family in `THIRD_PARTY.md`. The OFL allows bundling
the fonts with RayCAD as long as the licence text ships with them and the fonts are not sold on their own.
Until then Qt falls back to a system font.

The `mono` role's fallbacks (src/ui/theme_object.cpp) are fonts every platform already has, each with a zero
that cannot be read as the letter O, which matters in G-code ("O1001" is a program number, "01001" is not):
Consolas on Windows (slashed zero; every Windows since Vista), Menlo or SF Mono on macOS (slashed), DejaVu Sans
Mono or Liberation Mono on Linux (dotted). Cascadia Mono, Windows 11's own, comes last: its dot is a pixel or two
at 13 px. The G-code viewer's test checks that its O and 0 are drawn differently (tests/ui/cam/post_test.cpp).

## Engraving font: RayCAD Single Line (`raycad-single.rsf`)

The single-stroke (centre-line) font of the engraving operation (task T-056, `src/cam/ops/engrave_font.*`). It is
**original work drawn for RayCAD** and released under the project's licence, LGPL-2.1-or-later; no glyph data was taken
from any other font, CAD or CAM product, or the Hershey sets. Every glyph was designed for this file on a plain grid
of straight lines and circular arcs (capitals and digits 12 units high, x-height 8, descenders to -4), so a tool
follows it exactly with G1, G2 and G3 moves and nothing has to be approximated.

It has A to Z, a to z, 0 to 9 (a slashed zero, so it never reads as the letter O), space, `. , : ; - + = / # ( ) [ ]
< > _ ! ? * ' " %`, the degree sign, the diameter sign and capital O with stroke. Text the font cannot draw is refused
by name; nothing is substituted.

The format is plain text, one statement per line (the file's header is the reference):

    font <name>              the font's name
    cap <units>              the cap height in font units
    glyph U+XXXX <advance>   a glyph: its code point and advance width (units, including the space after it)
    M x y L x y A x y cx cy +|-  one stroke per line: start, then lines to (x, y) and arcs to (x, y) about (cx, cy),
                             counter-clockwise (+) or clockwise (-); an arc that ends where it starts is a circle
    end                      ends the glyph

Numbers have at most three decimals and are read exactly (thousandths of a unit). `src/cam/CMakeLists.txt` compiles
the file into the program, so a toolpath never depends on a file on disk, and `tests/unit/cam/engrave_test.cpp`
checks that the compiled-in text is this file, that every promised character is there and that every arc's ends lie
on its circle.
