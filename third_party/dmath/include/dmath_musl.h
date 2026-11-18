/* Public entry points of the vendored musl libm subset (RayCAD dmath). MIT licence, see ../COPYRIGHT.
 * Only src/core calls these, through ray::dmath (include/ray/core.h).
 *
 * The linking code must define ray_dmath_musl_sqrt as the IEEE-754 correctly rounded square root;
 * asin, acos and hypot call it.
 */
#ifndef RAY_DMATH_MUSL_H
#define RAY_DMATH_MUSL_H

#ifdef __cplusplus
extern "C" {
#endif

double ray_dmath_musl_sin(double);
double ray_dmath_musl_cos(double);
double ray_dmath_musl_tan(double);
double ray_dmath_musl_asin(double);
double ray_dmath_musl_acos(double);
double ray_dmath_musl_atan(double);
double ray_dmath_musl_atan2(double, double);
double ray_dmath_musl_hypot(double, double);
double ray_dmath_musl_sqrt(double);

#ifdef __cplusplus
}
#endif

#endif
