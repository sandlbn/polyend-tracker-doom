// Tables for fmath.h (docs/OPTIMIZATION_2X.md OP-4).
#include "fmath.h"

#include <cmath>
#include <cstring>

namespace ot {
namespace fm {

namespace {

// 2^k for -1022 <= k <= 1023, built from the exponent bits.
double pow2i(int k) {
    const uint64_t bits = uint64_t(k + 1023) << 52;
    double d;
    std::memcpy(&d, &bits, sizeof d);
    return d;
}

// Cody-Waite split constants (the high parts have trailing zero bits, so
// k * hi is exact for the k used here).
constexpr double kLn2Hi = 6.93147180369123816490e-01;   // 0x3FE62E42FEE00000
constexpr double kLn2Lo = 1.90821492927058770002e-10;
constexpr double kInvLn2 = 1.44269504088896338700e+00;
constexpr double kPio2Hi = 1.57079632673412561417e+00;  // pi/2, first 33 bits
constexpr double kPio2Lo = 6.07710050650619224932e-11;
constexpr double kTwoOverPi = 6.36619772367581382433e-01;
constexpr double kLn10Hi = 0x1.26bb1bbap+1;              // ln 10, first 32 bits
constexpr double kLn10Lo = 6.208533736847395e-10;

// exp(r) for |r| <= ln2/2 + tiny: Taylor to r^14 (error < 1e-18).
double expPoly(double r) {
    double t = 1.0;
    for (int n = 14; n >= 1; --n) t = 1.0 + r * t / double(n);
    return t;
}

double expSplit(double hi, double lo) {
    // x = hi + lo; k = round(x / ln2).
    const double x = hi + lo;
    const double kf = double(lroundD(x * kInvLn2));
    const double r = (hi - kf * kLn2Hi) + (lo - kf * kLn2Lo);
    const int k = int(kf);
    const double e = expPoly(r);
    if (k > 1023) return e * pow2i(1023) * pow2i(k - 1023);
    if (k < -1022) return e * pow2i(-1022) * pow2i(k + 1022);
    return e * pow2i(k);
}

// sin / cos of |r| <= pi/4 (Taylor with Horner, error ~1 ulp).
double sinPoly(double r) {
    const double z = r * r;
    double t = 1.0;
    for (int n = 21; n >= 3; n -= 2) t = 1.0 - z * t / double(n * (n - 1));
    return r * t;
}
double cosPoly(double r) {
    const double z = r * r;
    double t = 1.0;
    for (int n = 20; n >= 2; n -= 2) t = 1.0 - z * t / double(n * (n - 1));
    return t;
}

// x = q * pi/2 + r, |r| <= pi/4 (plenty accurate for |x| < 1e6).
double reduce(double x, int& q) {
    const double kf = double(lroundD(x * kTwoOverPi));
    q = int(int64_t(kf) & 3);
    return (x - kf * kPio2Hi) - kf * kPio2Lo;
}

}  // namespace

double expD(double x) { return expSplit(x, 0.0); }

double pow10D(double x) {
    // x * ln10 as hi + lo: x * kLn10Hi in two halves keeps the product exact
    // enough (x has at most 53 bits; the error term stays below 1 ulp).
    const double hi = x * kLn10Hi;
    const double lo = x * kLn10Lo;
    return expSplit(hi, lo);
}

double sinD(double x) {
    int q;
    const double r = reduce(x, q);
    switch (q) {
        case 0: return sinPoly(r);
        case 1: return cosPoly(r);
        case 2: return -sinPoly(r);
        default: return -cosPoly(r);
    }
}

double cosD(double x) {
    int q;
    const double r = reduce(x, q);
    switch (q) {
        case 0: return cosPoly(r);
        case 1: return -sinPoly(r);
        case 2: return -cosPoly(r);
        default: return sinPoly(r);
    }
}

double sqrtD(double x) {
    if (!(x > 0.0)) return x == 0.0 ? x : (x - x) / (x - x);
    // Single-precision seed (VSQRT.F32 on the M4), two Newton steps.
    double y = double(std::sqrt(float(x)));
    if (!(y > 0.0) || y > 1e300) {
        // Outside float range: scale by 2^200.
        return x > 1.0 ? sqrtD(x * pow2i(-400)) * pow2i(200) : sqrtD(x * pow2i(400)) * pow2i(-200);
    }
    y = 0.5 * (y + x / y);
    y = 0.5 * (y + x / y);
    return y;
}

// Preview transposition ratio 2^((note - 60) / 12) for notes 0..127, the
// correctly rounded pow(2.0, (note - 60) / 12.0) as exact doubles: no double
// libm on the device (docs/OPTIMIZATION_2X.md OP-4; tests/hw_math_test.cpp
// checks every entry against std::pow).
const double kNoteRatio[128] = {
    0x1p-5, 0x1.0f38f92d97962p-5, 0x1.1f59ac3c7d6c1p-5, 0x1.306fe0a31b715p-5,
    0x1.428a2f98d728ap-5, 0x1.55b8108f0ec5fp-5, 0x1.6a09e667f3bcdp-5, 0x1.7f910d768cfafp-5,
    0x1.965fea53d6e3ep-5, 0x1.ae89f995ad3adp-5, 0x1.c823e074ec128p-5, 0x1.e3437e7101345p-5,
    0x1p-4, 0x1.0f38f92d97963p-4, 0x1.1f59ac3c7d6bfp-4, 0x1.306fe0a31b715p-4,
    0x1.428a2f98d728bp-4, 0x1.55b8108f0ec5dp-4, 0x1.6a09e667f3bcdp-4, 0x1.7f910d768cfb1p-4,
    0x1.965fea53d6e3cp-4, 0x1.ae89f995ad3adp-4, 0x1.c823e074ec12ap-4, 0x1.e3437e7101343p-4,
    0x1p-3, 0x1.0f38f92d97963p-3, 0x1.1f59ac3c7d6bfp-3, 0x1.306fe0a31b715p-3,
    0x1.428a2f98d728bp-3, 0x1.55b8108f0ec5dp-3, 0x1.6a09e667f3bcdp-3, 0x1.7f910d768cfb1p-3,
    0x1.965fea53d6e3cp-3, 0x1.ae89f995ad3adp-3, 0x1.c823e074ec12ap-3, 0x1.e3437e7101343p-3,
    0x1p-2, 0x1.0f38f92d97963p-2, 0x1.1f59ac3c7d6cp-2, 0x1.306fe0a31b715p-2,
    0x1.428a2f98d728bp-2, 0x1.55b8108f0ec5ep-2, 0x1.6a09e667f3bcdp-2, 0x1.7f910d768cfbp-2,
    0x1.965fea53d6e3dp-2, 0x1.ae89f995ad3adp-2, 0x1.c823e074ec129p-2, 0x1.e3437e7101344p-2,
    0x1p-1, 0x1.0f38f92d97963p-1, 0x1.1f59ac3c7d6cp-1, 0x1.306fe0a31b715p-1,
    0x1.428a2f98d728bp-1, 0x1.55b8108f0ec5ep-1, 0x1.6a09e667f3bcdp-1, 0x1.7f910d768cfbp-1,
    0x1.965fea53d6e3dp-1, 0x1.ae89f995ad3adp-1, 0x1.c823e074ec129p-1, 0x1.e3437e7101344p-1,
    0x1p+0, 0x1.0f38f92d97963p+0, 0x1.1f59ac3c7d6cp+0, 0x1.306fe0a31b715p+0,
    0x1.428a2f98d728bp+0, 0x1.55b8108f0ec5ep+0, 0x1.6a09e667f3bcdp+0, 0x1.7f910d768cfbp+0,
    0x1.965fea53d6e3cp+0, 0x1.ae89f995ad3adp+0, 0x1.c823e074ec129p+0, 0x1.e3437e7101343p+0,
    0x1p+1, 0x1.0f38f92d97963p+1, 0x1.1f59ac3c7d6cp+1, 0x1.306fe0a31b715p+1,
    0x1.428a2f98d728bp+1, 0x1.55b8108f0ec5ep+1, 0x1.6a09e667f3bcdp+1, 0x1.7f910d768cfbp+1,
    0x1.965fea53d6e3dp+1, 0x1.ae89f995ad3adp+1, 0x1.c823e074ec129p+1, 0x1.e3437e7101344p+1,
    0x1p+2, 0x1.0f38f92d97963p+2, 0x1.1f59ac3c7d6bfp+2, 0x1.306fe0a31b715p+2,
    0x1.428a2f98d728bp+2, 0x1.55b8108f0ec5dp+2, 0x1.6a09e667f3bcdp+2, 0x1.7f910d768cfb1p+2,
    0x1.965fea53d6e3cp+2, 0x1.ae89f995ad3adp+2, 0x1.c823e074ec12ap+2, 0x1.e3437e7101343p+2,
    0x1p+3, 0x1.0f38f92d97963p+3, 0x1.1f59ac3c7d6bfp+3, 0x1.306fe0a31b715p+3,
    0x1.428a2f98d728bp+3, 0x1.55b8108f0ec5dp+3, 0x1.6a09e667f3bcdp+3, 0x1.7f910d768cfb1p+3,
    0x1.965fea53d6e3cp+3, 0x1.ae89f995ad3adp+3, 0x1.c823e074ec12ap+3, 0x1.e3437e7101343p+3,
    0x1p+4, 0x1.0f38f92d97962p+4, 0x1.1f59ac3c7d6c1p+4, 0x1.306fe0a31b715p+4,
    0x1.428a2f98d728ap+4, 0x1.55b8108f0ec5fp+4, 0x1.6a09e667f3bcdp+4, 0x1.7f910d768cfafp+4,
    0x1.965fea53d6e3ep+4, 0x1.ae89f995ad3adp+4, 0x1.c823e074ec128p+4, 0x1.e3437e7101345p+4,
    0x1p+5, 0x1.0f38f92d97962p+5, 0x1.1f59ac3c7d6c1p+5, 0x1.306fe0a31b715p+5,
    0x1.428a2f98d728ap+5, 0x1.55b8108f0ec5fp+5, 0x1.6a09e667f3bcdp+5, 0x1.7f910d768cfafp+5
};

}  // namespace fm
}  // namespace ot
