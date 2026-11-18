/* Build header for the vendored musl libm subset (RayCAD dmath).
 *
 * This file replaces musl's src/internal/libm.h (musl 1.2.6, MIT licence, see ../COPYRIGHT). The parts
 * the vendored functions use (FORCE_EVAL, the word-access macros) are copied from it unchanged. It differs
 * from upstream in these ways, and nothing else:
 *   - it does not include the platform <math.h>, <endian.h> or fp_arch.h, and defines double_t and isnan
 *     itself, so the functions build the same way on MSVC, GCC and Clang;
 *   - it renames every function to a ray_dmath_musl_ prefix with macros, so these definitions never clash
 *     with, or are replaced by, the platform's sin, cos, floor and so on (compilers treat calls to those
 *     names as built-ins);
 *   - sqrt is not vendored: ray_dmath_musl_sqrt is supplied by the code that links this library and must
 *     be the IEEE-754 correctly rounded square root (src/core/dmath.cpp uses std::sqrt);
 *   - it drops the long double shapes, the float and long double prototypes and the error helpers, which
 *     the vendored subset does not use.
 */
#ifndef RAY_DMATH_LIBM_H
#define RAY_DMATH_LIBM_H

#include <float.h>
#include <stdint.h>

#include "dmath_musl.h"

/* Every expression must be evaluated in double precision: SSE2 on x86-64, NEON on ARM64, never x87. */
#if defined(FLT_EVAL_METHOD) && FLT_EVAL_METHOD != 0
#error "dmath needs FLT_EVAL_METHOD == 0 (no x87 excess precision)"
#endif
#if DBL_MANT_DIG != 53 || DBL_MAX_EXP != 1024 || FLT_RADIX != 2
#error "dmath needs IEEE-754 binary64 double"
#endif

typedef double double_t;
typedef float float_t;

/* Public names (declared in dmath_musl.h). */
#define sin ray_dmath_musl_sin
#define cos ray_dmath_musl_cos
#define tan ray_dmath_musl_tan
#define asin ray_dmath_musl_asin
#define acos ray_dmath_musl_acos
#define atan ray_dmath_musl_atan
#define atan2 ray_dmath_musl_atan2
#define hypot ray_dmath_musl_hypot
#define sqrt ray_dmath_musl_sqrt
#define floor ray_dmath_musl_floor
#define fabs ray_dmath_musl_fabs
#define scalbn ray_dmath_musl_scalbn

/* Internal kernels. */
#define __sin ray_dmath_musl_k_sin
#define __cos ray_dmath_musl_k_cos
#define __tan ray_dmath_musl_k_tan
#define __rem_pio2 ray_dmath_musl_rem_pio2
#define __rem_pio2_large ray_dmath_musl_rem_pio2_large

/* musl marks internal functions with a visibility attribute; a static library needs none. */
#define hidden

/* Helps static branch prediction so hot path can be better optimized.  */
#ifdef __GNUC__
#define predict_true(x) __builtin_expect(!!(x), 1)
#define predict_false(x) __builtin_expect(x, 0)
#else
#define predict_true(x) (x)
#define predict_false(x) (x)
#endif

/* fp_force_eval ensures that the input value is computed when that's
   otherwise unused.  To prevent the constant folding of the input
   expression, an additional fp_barrier may be needed or a compilation
   mode that does so (e.g. -frounding-math in gcc). Then it can be
   used to evaluate an expression for its fenv side-effects only.   */

static inline void fp_force_evalf(float x)
{
	volatile float y;
	y = x;
}

static inline void fp_force_eval(double x)
{
	volatile double y;
	y = x;
}

static inline void fp_force_evall(long double x)
{
	volatile long double y;
	y = x;
}

#define FORCE_EVAL(x) do {                        \
	if (sizeof(x) == sizeof(float)) {         \
		fp_force_evalf(x);                \
	} else if (sizeof(x) == sizeof(double)) { \
		fp_force_eval(x);                 \
	} else {                                  \
		fp_force_evall(x);                \
	}                                         \
} while(0)

#define asuint(f) ((union{float _f; uint32_t _i;}){f})._i
#define asfloat(i) ((union{uint32_t _i; float _f;}){i})._f
#define asuint64(f) ((union{double _f; uint64_t _i;}){f})._i
#define asdouble(i) ((union{uint64_t _i; double _f;}){i})._f

#define EXTRACT_WORDS(hi,lo,d)                    \
do {                                              \
  uint64_t __u = asuint64(d);                     \
  (hi) = __u >> 32;                               \
  (lo) = (uint32_t)__u;                           \
} while (0)

#define GET_HIGH_WORD(hi,d)                       \
do {                                              \
  (hi) = asuint64(d) >> 32;                       \
} while (0)

#define GET_LOW_WORD(lo,d)                        \
do {                                              \
  (lo) = (uint32_t)asuint64(d);                   \
} while (0)

#define INSERT_WORDS(d,hi,lo)                     \
do {                                              \
  (d) = asdouble(((uint64_t)(hi)<<32) | (uint32_t)(lo)); \
} while (0)

#define SET_HIGH_WORD(d,hi)                       \
  INSERT_WORDS(d, hi, (uint32_t)asuint64(d))

#define SET_LOW_WORD(d,lo)                        \
  INSERT_WORDS(d, asuint64(d)>>32, lo)

/* Used on double arguments only (atan, atan2). */
#define isnan(x) ((asuint64(x) & -1ULL>>1) > 0x7ffULL<<52)

hidden int    __rem_pio2_large(double*,double*,int,int,int);

hidden int    __rem_pio2(double,double*);
hidden double __sin(double,double,int);
hidden double __cos(double,double);
hidden double __tan(double,double,int);

double floor(double);
double fabs(double);
double scalbn(double, int);

#endif
