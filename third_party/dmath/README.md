# dmath: vendored musl libm subset

The implementation behind `ray::dmath` (`include/ray/core.h`, PLAN.md section 8): the double-precision
`sin`, `cos`, `tan`, `asin`, `acos`, `atan2` and `hypot` of musl, with the helpers they call. They are
compiled from the same C source, with the same strict floating-point flags, on every platform, so their results
do not depend on the platform C library.

## Origin

- Project: musl libc, https://musl.libc.org/
- Version: 1.2.6, release tarball `musl-1.2.6.tar.gz` from https://musl.libc.org/releases/
  (SHA-256 `d585fd3b613c66151fc3249e8ed44f77020cb5e6c1e635a616d3f9f82460512a`),
  tag `v1.2.6` of https://git.musl-libc.org/git/musl, commit `9fa28ece75d8a2191de7c5bb53bed224c5947417`.
- Licence: MIT. `COPYRIGHT` is musl's copyright file, unchanged. The trigonometric code comes from FreeBSD's
  msun and carries the Sun Microsystems permission notice in each file header.

## Files

`src/*.c` are copies of musl's `src/math/` files of the same name: `__cos.c`, `__rem_pio2.c`,
`__rem_pio2_large.c`, `__sin.c`, `__tan.c`, `acos.c`, `asin.c`, `atan.c`, `atan2.c`, `cos.c`, `fabs.c`,
`floor.c`, `hypot.c`, `scalbn.c`, `sin.c`, `tan.c`. The generic C versions are used everywhere, including
the functions musl implements in assembly on some architectures (`fabs`, `floor` on AArch64).

Changes to those files: in `fabs.c`, `hypot.c` and `scalbn.c` the line `#include <math.h>` is replaced by
`#include "libm.h"`. Nothing else is changed.

`src/libm.h` replaces musl's internal `src/internal/libm.h`. It keeps the macros the files use, does not
include the platform `<math.h>`, and renames every function to a `ray_dmath_musl_` prefix (so `sin` is
compiled as `ray_dmath_musl_sin`), which keeps these definitions apart from the platform library and from
compiler built-ins. Its header comment lists every difference from upstream. `include/dmath_musl.h`
declares the entry points.

`sqrt` is not vendored: IEEE-754 requires it to be correctly rounded, and every supported platform's is.
The linking code supplies `ray_dmath_musl_sqrt` (`src/core/dmath.cpp` forwards to `std::sqrt`).

## Build and determinism

`CMakeLists.txt` builds an object library that `src/core` compiles into `ray_core`. The files are compiled as
C11 with: no x87 (`libm.h` requires `FLT_EVAL_METHOD == 0`), no contraction into fused multiply-add
(`-ffp-contract=off`; MSVC `/fp:precise`, which does not contract since Visual Studio 2022), no fast math.
Upstream warnings are not fixed; warnings are off for these files.

`tests/unit/core/dmath_table.bin` holds 100,000 inputs and the results of this code, written by
`tools/gen_dmath_table.py`. `ray_core_tests` checks every build against it bit for bit. Regenerate the table
only when this code is changed on purpose.
