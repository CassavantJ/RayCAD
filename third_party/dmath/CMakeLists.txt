# dmath: the vendored musl libm subset behind ray::dmath (see README.md). Added from src/core/CMakeLists.txt,
# after it includes EnableC.cmake; its objects are compiled into ray_core.
#
# The sources are C and are compiled as C, as upstream intends. Bit-identical results on MSVC, GCC and Clang
# need: IEEE double arithmetic without excess precision (SSE2 or NEON, never x87; libm.h checks
# FLT_EVAL_METHOD), no contraction into fused multiply-add, no fast math. The flags are set at the end.

# Third-party code: build it as upstream wrote it, without the project's warning flags (which are also partly
# C++-only). Sanitizer and other non-warning options stay.
get_property(_ray_options DIRECTORY PROPERTY COMPILE_OPTIONS)
list(FILTER _ray_options EXCLUDE REGEX "^(/W[0-4X]|/permissive-|/Zc:__cplusplus|-W.*)$")
set_property(DIRECTORY PROPERTY COMPILE_OPTIONS ${_ray_options})

add_library(dmath_musl OBJECT
    src/__cos.c
    src/__rem_pio2.c
    src/__rem_pio2_large.c
    src/__sin.c
    src/__tan.c
    src/acos.c
    src/asin.c
    src/atan.c
    src/atan2.c
    src/cos.c
    src/fabs.c
    src/floor.c
    src/hypot.c
    src/scalbn.c
    src/sin.c
    src/tan.c
)
target_include_directories(dmath_musl PRIVATE src PUBLIC include)
set_target_properties(dmath_musl PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF FOLDER "third_party")
# Floating-point flags. MSVC: /fp:precise, not the /fp:strict of ray_deterministic(), because /fp:strict turns
# on FENV_ACCESS and the C compiler then rejects constant initialisers such as "1.5/DBL_EPSILON". /fp:precise
# keeps IEEE semantics, does not reassociate, and since Visual Studio 2022 does not contract into fused
# multiply-add unless /fp:contract is given (and x64 without /arch:AVX2 has no FMA instruction anyway).
if(MSVC)
    target_compile_options(dmath_musl PRIVATE /W0 /fp:precise)
else()
    target_compile_options(dmath_musl PRIVATE -w -ffp-contract=off -fno-fast-math)
endif()
