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
