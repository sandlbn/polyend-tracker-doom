// Double-precision math helpers that do not pull the double libm into the
// firmware (docs/OPTIMIZATION_2X.md OP-4: the Cortex-M4F has a
// single-precision FPU only; newlib's double floor/ceil/lround/pow bring in
// ~12 KB of soft-float library code). The rounding helpers return bit for
// bit what the libm function returns for |v| < 2^52 (tests/fmath_test.cpp).
#pragma once

#include <cstdint>

namespace ot {
namespace fm {

// floor(v): the integer part is exact for |v| < 2^63; -0.0 comes back as +0.0
// (callers only use the value arithmetically).
inline double floorD(double v) {
    if (!(v > -9.0e18 && v < 9.0e18)) return v;   // NaN / huge: already integral
    const double t = double(int64_t(v));
    return t > v ? t - 1.0 : t;
}

inline double ceilD(double v) {
    if (!(v > -9.0e18 && v < 9.0e18)) return v;
    const double t = double(int64_t(v));
    return t < v ? t + 1.0 : t;
}

// lround(v): round half away from zero. v - trunc(v) is exact for |v| < 2^52.
inline long lroundD(double v) {
    const int64_t i = int64_t(v);
    const double f = v - double(i);
    return long(f >= 0.5 ? i + 1 : (f <= -0.5 ? i - 1 : i));
}

// Double-precision transcendentals for the Sample Editor effects (sample_fx)
// in plain double arithmetic (fmath.cpp): range reduction + polynomial, within
// a few ulp of the libm (tests/fmath_test.cpp), so the int16 results match
// the former libm-based code on every tested sample.
double expD(double x);            // |x| < 700
double pow10D(double x);          // 10^x, |x| < 300
double sinD(double x);            // |x| < 1e6
double cosD(double x);
double sqrtD(double x);           // x >= 0

// 2^((note - 60) / 12) for notes 0..127 (fmath.cpp): the correctly rounded
// pow(2.0, (note - 60) / 12.0) for every note.
extern const double kNoteRatio[128];

}  // namespace fm
}  // namespace ot
