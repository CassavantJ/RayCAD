/* Writes the dmath reference table (tests/unit/core/dmath_table.bin) from the vendored code in third_party/dmath.
 * Built and run by tools/gen_dmath_table.py; see that script for the flags. Usage: gen_dmath_table <output file>
 *
 * Inputs are built from bit patterns of a fixed-seed generator (no floating-point arithmetic except the exact
 * k * pi/2 products), so the inputs are the same on every platform. The outputs are what the vendored code
 * returns for them.
 *
 * File layout, every integer and double little-endian:
 *   "RAYDMATH"                                   8 bytes
 *   u32 version (1), u32 group count (8)
 *   per group: u32 function id, u32 arity, u32 record count, u32 reserved (0)
 *   then the records of each group in order: arity input doubles, then the result double
 * Function ids: 0 sin, 1 cos, 2 tan, 3 asin, 4 acos, 5 atan2(y, x), 6 sqrt, 7 hypot(x, y).
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "dmath_musl.h"

double ray_dmath_musl_sqrt(double x)
{
    return sqrt(x);
}

enum { kGroups = 8, kPerGroup = 12500 };

static uint64_t g_state = 0x5241594341440001ULL;

static uint64_t next(void)
{
    uint64_t z = (g_state += 0x9e3779b97f4a7c15ULL);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
}

static double fromBits(uint64_t bits)
{
    double d;
    memcpy(&d, &bits, sizeof d);
    return d;
}

static uint64_t toBits(double d)
{
    uint64_t bits;
    memcpy(&bits, &d, sizeof bits);
    return bits;
}

static uint64_t randomSign(void)
{
    return (next() >> 63) << 63;
}

/* |x| in [2^emin, 2^(emax+1)) with a random mantissa; emin >= -1022, emax <= 1023. */
static double logUniform(int emin, int emax, int withSign)
{
    const int e = emin + (int)(next() % (uint64_t)(emax - emin + 1));
    const uint64_t mantissa = next() & ((1ULL << 52) - 1);
    const uint64_t sign = withSign ? randomSign() : 0;
    return fromBits(sign | ((uint64_t)(e + 1023) << 52) | mantissa);
}

static double subnormal(int withSign)
{
    uint64_t mantissa = next() & ((1ULL << 52) - 1);
    if (mantissa == 0) {
        mantissa = 1;
    }
    return fromBits((withSign ? randomSign() : 0) | mantissa);
}

static double anyFinite(void)
{
    for (;;) {
        const uint64_t bits = next();
        if (((bits >> 52) & 0x7ff) != 0x7ff) {
            return fromBits(bits);
        }
    }
}

static const double kPio2 = 1.5707963267948966; /* nearest double to pi/2 */

/* Near k * pi/2 for an integer k below 2^20, nudged by up to 8 ulps either way. */
static double nearPio2Multiple(void)
{
    const double k = (double)(int64_t)(next() % (1u << 20)) + 1.0;
    const double x = k * kPio2;
    const int64_t nudge = (int64_t)(next() % 17) - 8;
    return fromBits((uint64_t)((int64_t)toBits(x) + nudge)) * ((next() & 1) ? 1.0 : -1.0);
}


static double trigInput(void)
{
    const uint64_t c = next() % 100;
    if (c < 30) return logUniform(-3, 2, 1);
    if (c < 50) return logUniform(-60, 20, 1);
    if (c < 70) return nearPio2Multiple();
    if (c < 85) return logUniform(20, 1023, 1);
    if (c < 95) return anyFinite();
    return subnormal(1);
}

static double inverseTrigInput(void)
{
    const uint64_t c = next() % 100;
    if (c < 50) return logUniform(-10, -1, 1);
    if (c < 65) return fromBits(randomSign() | (0x3ff0000000000000ULL - 1 - next() % (1ULL << 30)));
    if (c < 80) return logUniform(-60, -1, 1);
    if (c < 90) return subnormal(1);
    return anyFinite();
}

static void pairInput(double* a, double* b, int hypotMode)
{
    const uint64_t c = next() % 100;
    if (c < 40) {
        *a = logUniform(-3, 3, 1);
        *b = logUniform(-3, 3, 1);
    } else if (c < 65) {
        *a = logUniform(-100, 100, 1);
        *b = logUniform(-100, 100, 1);
    } else if (c < 80) {
        *a = logUniform(-1022, 1023, 1);
        *b = logUniform(-1022, 1023, 1);
    } else if (c < 88) {
        *a = hypotMode ? logUniform(1000, 1023, 1) : subnormal(1);
        *b = hypotMode ? logUniform(1000, 1023, 1) : logUniform(-3, 3, 1);
    } else if (c < 94) {
        *a = subnormal(1);
        *b = subnormal(1);
    } else {
        *a = anyFinite();
        *b = anyFinite();
    }
}

static double sqrtInput(void)
{
    const uint64_t c = next() % 100;
    if (c < 60) return logUniform(-1022, 1023, 0);
    if (c < 75) return subnormal(0);
    if (c < 85) {
        const double k = (double)(int64_t)(next() % (1u << 26));
        return k * k; /* exact: below 2^52 */
    }
    return anyFinite();
}

static const uint64_t kUnarySpecials[] = {
    0x0000000000000000ULL, 0x8000000000000000ULL, /* +-0 */
    0x0000000000000001ULL, 0x8000000000000001ULL, /* +-smallest subnormal */
    0x0010000000000000ULL, 0x8010000000000000ULL, /* +-DBL_MIN */
    0x3e50000000000000ULL, 0x3e40000000000000ULL, /* 2^-26, 2^-27 */
    0x3fe921fb54442d18ULL, 0x3fe921fb54442d19ULL, /* pi/4 and the next double */
    0x3ff921fb54442d18ULL, 0x400921fb54442d18ULL, /* pi/2, pi */
    0x401921fb54442d18ULL, 0x4012d97c7f3321d2ULL, /* 2 pi, 3 pi/2 */
    0x3fe0000000000000ULL, 0xbfe0000000000000ULL, /* +-0.5 */
    0x3fefffffffffffffULL, 0xbfefffffffffffffULL, /* +-largest below 1 */
    0x3ff0000000000000ULL, 0xbff0000000000000ULL, /* +-1 */
    0x3ff0000000000001ULL, 0x4000000000000000ULL, /* 1 + ulp, 2 */
    0x412e848000000000ULL, 0x4330000000000000ULL, /* 1e6, 2^52 */
    0x44b52d02c7e14af6ULL, 0x7e37e43c8800759cULL, /* 1e22, 1e300 */
    0x7fe0000000000000ULL, 0x7fefffffffffffffULL, /* 2^1023, DBL_MAX */
    0xffefffffffffffffULL,                        /* -DBL_MAX */
    0x7ff0000000000000ULL, 0xfff0000000000000ULL, /* +-inf */
    0x7ff8000000000000ULL,                        /* NaN */
};
static const uint64_t kPairSpecials[] = {
    0x0000000000000000ULL, 0x8000000000000000ULL, 0x3ff0000000000000ULL,
    0xbff0000000000000ULL, 0x7ff0000000000000ULL, 0xfff0000000000000ULL,
    0x7ff8000000000000ULL, 0x0000000000000001ULL, 0x7fefffffffffffffULL,
};
#define COUNT(a) (sizeof(a) / sizeof((a)[0]))

static double g_in[kGroups][kPerGroup][2];
static double g_out[kGroups][kPerGroup];

static double call(int f, double a, double b)
{
    switch (f) {
    case 0: return ray_dmath_musl_sin(a);
    case 1: return ray_dmath_musl_cos(a);
    case 2: return ray_dmath_musl_tan(a);
    case 3: return ray_dmath_musl_asin(a);
    case 4: return ray_dmath_musl_acos(a);
    case 5: return ray_dmath_musl_atan2(a, b);
    case 6: return ray_dmath_musl_sqrt(a);
    default: return ray_dmath_musl_hypot(a, b);
    }
}

static int arity(int f)
{
    return (f == 5 || f == 7) ? 2 : 1;
}

static void putU32(FILE* file, uint32_t v)
{
    unsigned char b[4];
    for (int i = 0; i < 4; ++i) b[i] = (unsigned char)(v >> (8 * i));
    fwrite(b, 1, 4, file);
}

static void putF64(FILE* file, double d)
{
    const uint64_t v = toBits(d);
    unsigned char b[8];
    for (int i = 0; i < 8; ++i) b[i] = (unsigned char)(v >> (8 * i));
    fwrite(b, 1, 8, file);
}

int main(int argc, char** argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: gen_dmath_table <output file>\n");
        return 2;
    }
    for (int f = 0; f < kGroups; ++f) {
        int n = 0;
        if (arity(f) == 1) {
            for (size_t i = 0; i < COUNT(kUnarySpecials); ++i, ++n) {
                g_in[f][n][0] = fromBits(kUnarySpecials[i]);
                g_in[f][n][1] = 0.0;
            }
        } else {
            for (size_t i = 0; i < COUNT(kPairSpecials); ++i) {
                for (size_t j = 0; j < COUNT(kPairSpecials); ++j, ++n) {
                    g_in[f][n][0] = fromBits(kPairSpecials[i]);
                    g_in[f][n][1] = fromBits(kPairSpecials[j]);
                }
            }
        }
        for (; n < kPerGroup; ++n) {
            double a = 0.0, b = 0.0;
            if (f <= 2) a = trigInput();
            else if (f <= 4) a = inverseTrigInput();
            else if (f == 6) a = sqrtInput();
            else pairInput(&a, &b, f == 7);
            g_in[f][n][0] = a;
            g_in[f][n][1] = b;
        }
        for (n = 0; n < kPerGroup; ++n) {
            g_out[f][n] = call(f, g_in[f][n][0], g_in[f][n][1]);
        }
    }

    FILE* file = fopen(argv[1], "wb");
    if (!file) {
        fprintf(stderr, "gen_dmath_table: cannot write %s\n", argv[1]);
        return 1;
    }
    fwrite("RAYDMATH", 1, 8, file);
    putU32(file, 1);
    putU32(file, kGroups);
    for (int f = 0; f < kGroups; ++f) {
        putU32(file, (uint32_t)f);
        putU32(file, (uint32_t)arity(f));
        putU32(file, kPerGroup);
        putU32(file, 0);
    }
    for (int f = 0; f < kGroups; ++f) {
        for (int n = 0; n < kPerGroup; ++n) {
            for (int a = 0; a < arity(f); ++a) putF64(file, g_in[f][n][a]);
            putF64(file, g_out[f][n]);
        }
    }
    const int bad = ferror(file);
    if (fclose(file) != 0 || bad) {
        fprintf(stderr, "gen_dmath_table: write failed\n");
        return 1;
    }
    printf("gen_dmath_table: %d records in %d groups\n", kGroups * kPerGroup, kGroups);
    return 0;
}
