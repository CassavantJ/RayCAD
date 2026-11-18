// ray::dmath: deterministic math (PLAN.md section 8). Every function except sqrt forwards to the vendored
// musl code in third_party/dmath, which is compiled with the same strict floating-point flags on every platform.
// sqrt is IEEE-754 correctly rounded on every supported platform, so the platform one is used, here and inside
// the vendored asin, acos and hypot.
#include <ray/core.h>

#include <cmath>
#include <dmath_musl.h>

extern "C" double ray_dmath_musl_sqrt(double x)
{
    return std::sqrt(x);
}

namespace ray::dmath {

double sin(double x)
{
    return ray_dmath_musl_sin(x);
}

double cos(double x)
{
    return ray_dmath_musl_cos(x);
}

double tan(double x)
{
    return ray_dmath_musl_tan(x);
}

double asin(double x)
{
    return ray_dmath_musl_asin(x);
}

double acos(double x)
{
    return ray_dmath_musl_acos(x);
}

double atan2(double y, double x)
{
    return ray_dmath_musl_atan2(y, x);
}

double sqrt(double x)
{
    return std::sqrt(x);
}

double hypot(double x, double y)
{
    return ray_dmath_musl_hypot(x, y);
}

} // namespace ray::dmath
