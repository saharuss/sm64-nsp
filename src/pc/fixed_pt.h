#ifndef FIXED_PT_H
#define FIXED_PT_H

#include <stdint.h>

typedef int64_t fix64;

#define FRAC_WIDTH 32 // non configurable for mult operation

#define GET_INT(fix) ((fix) >> FRAC_WIDTH)
#define GET_FRAC(fix) ((fix) & ((1LL << FRAC_WIDTH) - 1))

#define FIX_ONE (1LL << FRAC_WIDTH)
#define FIX_ONE_HALF (1LL << (FRAC_WIDTH - 1))
#define FIX_MAX (fix64)((1ULL << 63) - 1) // full fraction, max value integer
#define FIX_MIN (fix64)(1ULL << 63)       // full fraction, min value integer

#define FIX_2_INT(fix) (((fix) >> FRAC_WIDTH)) // non rounding
#define FIX_2_FLOAT(fix) ((float) (fix) / (1LL << FRAC_WIDTH))
#define FIX_2_DOUBLE(fix) ((double) (fix) / (1LL << FRAC_WIDTH))

#define INT_2_FIX(num)                                                                                 \
    ((fix64) (num) << FRAC_WIDTH) // all fixed point operations must involve two fix64 numbers
#define FLOAT_2_FIX(f) ((fix64) ((f) * (1LL << FRAC_WIDTH)))  // shifting float left FRAC_WIDTH bits
#define DOUBLE_2_FIX(d) ((fix64) ((d) * (1LL << FRAC_WIDTH))) // same as above

#define FIX_INV(fix) ((1ULL << 63) / (fix) << 1) // only loses one bit of precision (?)

static inline fix64 fix_mult(const fix64 fix1, const fix64 fix2) { // multiply 2 fixes, return fix
    fix64 i1 = GET_INT(fix1);
    fix64 f1 = GET_FRAC(fix1);
    fix64 i2 = GET_INT(fix2);
    fix64 f2 = GET_FRAC(fix2);

    return (((uint64_t) f1 * f2) >> 32) + ((int64_t) (i1 * i2) << 32) + i1 * f2 + i2 * f1;
}

static inline int32_t fix_mult_i64(const fix64 fix1,
                                   const fix64 fix2) { // multiply two fixes, return integer part. saves
                                                       // time not calculating decimal * decimal
    fix64 i1 = GET_INT(fix1);
    fix64 f1 = GET_FRAC(fix1);
    fix64 i2 = GET_INT(fix2);
    fix64 f2 = GET_FRAC(fix2);

    return (int32_t) i1 * (int32_t) i2 + (int32_t) ((i1 * f2 + i2 * f1) >> 32);
}

static inline int32_t fix_mult_i32(const fix64 fix1,
                                   const fix64 fix2) { // save more time by "casting" to 16.16 fixed
                                                       // point. Loss of precision, obviously
    return ((fix1 >> 16) * (fix2 >> 16)) >> 32;
}

// Count leading zeros for 64-bit integer
static inline int fix_clz64(uint64_t x) {
    if (x == 0)
        return 64;
#if defined(__GNUC__) || defined(__clang__)
    return __builtin_clzll(x);
#else
    int n = 0;
    if (x <= 0x00000000FFFFFFFFULL) {
        n += 32;
        x <<= 32;
    }
    if (x <= 0x0000FFFFFFFFFFFFULL) {
        n += 16;
        x <<= 16;
    }
    if (x <= 0x00FFFFFFFFFFFFFFULL) {
        n += 8;
        x <<= 8;
    }
    if (x <= 0x0FFFFFFFFFFFFFFFULL) {
        n += 4;
        x <<= 4;
    }
    if (x <= 0x3FFFFFFFFFFFFFFFULL) {
        n += 2;
        x <<= 2;
    }
    if (x <= 0x7FFFFFFFFFFFFFFFULL) {
        n += 1;
    }
    return n;
#endif
}

// Pure integer fixed-point division: num / denom, both in 32.32 fixed point
// Uses 64-bit integer arithmetic only — no float
static inline fix64 fix_div_s(const fix64 num, const fix64 denom) {
    if (denom == 0)
        return (num >= 0) ? FIX_MAX : FIX_MIN;

    // Handle signs
    int neg = 0;
    uint64_t a = (uint64_t) num;
    uint64_t b = (uint64_t) denom;
    if (num < 0) {
        a = (uint64_t) (-(int64_t) a);
        neg ^= 1;
    }
    if (denom < 0) {
        b = (uint64_t) (-(int64_t) b);
        neg ^= 1;
    }

    // We want (a << 32) / b, but a << 32 can overflow 64 bits.
    // Split: result = (a / b) << 32  +  ((a % b) << 32) / b
    uint64_t quot_hi = a / b;
    uint64_t rem = a % b;

    // Shift remainder up by 32 bits for fractional part
    // To avoid overflow, do it in stages
    uint64_t quot_lo = 0;
    for (int i = 0; i < 32; i++) {
        rem <<= 1;
        quot_lo <<= 1;
        if (rem >= b) {
            rem -= b;
            quot_lo |= 1;
        }
    }

    fix64 result = (fix64) ((quot_hi << 32) | quot_lo);
    return neg ? -result : result;
}

// Fast reciprocal: computes FIX_ONE / x using Newton-Raphson in integer math.
// Optimized for the per-pixel 1/w perspective correction case.
// Uses 2 iterations of Newton-Raphson: x_{n+1} = x_n * (2 - d * x_n)
static inline fix64 fix_recip(const fix64 denom) {
    if (denom == 0)
        return FIX_MAX;
    if (denom == FIX_ONE)
        return FIX_ONE;
    if (denom == -FIX_ONE)
        return -FIX_ONE;

    int neg = 0;
    uint64_t d = (uint64_t) denom;
    if (denom < 0) {
        d = (uint64_t) (-(int64_t) d);
        neg = 1;
    }

    // Normalize: shift d so its MSB is at bit 63
    int shift = fix_clz64(d);
    uint64_t dn = d << shift; // dn is now in [2^63, 2^64)

    // Initial estimate: for normalized input in [0.5, 1.0),
    // use linear approximation: 1/x ≈ 2.9142 - 2*x (in the normalized range)
    // In Q0.64: estimate ≈ (48/17) - (32/17)*dn
    // Simpler: use 2^64 / dn as starting point (integer division)
    // But that's expensive. Use a coarser estimate instead.
    // For a value with MSB at bit 63, reciprocal is approximately:
    // ~2^64 / dn. We approximate as (0xFFFFFFFFFFFFFFFF / dn) + 1
    uint64_t x = (0xFFFFFFFFFFFFFFFFULL / dn) + 1;

    // Newton-Raphson iteration: x = x * (2 - d*x)
    // All in Q0.64 arithmetic
    // Iteration 1
    {
        // Compute d*x in Q0.64: take top 64 bits of 128-bit product
        // Approximate: (dn >> 32) * (x >> 32) gives top ~64 bits
        uint64_t dx_hi = ((dn >> 32) * (x >> 32)); // Q0.64 approx
        // 2 - d*x: in Q0.64, "2" = 0 with borrow, so 2-dx = ~dx + 1 (negate)
        uint64_t two_minus_dx = ~dx_hi + 1; // since dx should be near 1.0 in Q0.64
        // x = x * (2 - d*x)
        x = (x >> 32) * (two_minus_dx >> 32); // keep in Q0.64
        x <<= 1;                              // correct for halved precision
    }
    // Iteration 2
    {
        uint64_t dx_hi = ((dn >> 32) * (x >> 32));
        uint64_t two_minus_dx = ~dx_hi + 1;
        x = (x >> 32) * (two_minus_dx >> 32);
        x <<= 1;
    }

    // x is now approximately 2^64 / dn (in Q0.64)
    // We need FIX_ONE / denom = 2^32 / denom (in 32.32 fixed point)
    // Since dn = d << shift, and x ≈ 2^64 / dn = 2^64 / (d << shift) = 2^(64-shift) / d
    // We need 2^64 / d = x << shift (in Q0.64)
    // Convert from Q0.64 to Q32.32: result = (x << shift) >> 32
    // But x << shift can overflow, so: result = x >> (32 - shift)
    int rshift = 32 - shift;
    fix64 result;
    if (rshift >= 0) {
        result = (fix64) (x >> rshift);
    } else {
        result = (fix64) (x << (-rshift));
    }

    return neg ? -result : result;
}

// Keep float-based division as a fallback for non-performance-critical paths
static inline fix64 fix_div_slow(const fix64 num, const fix64 denom) {
    return (fix64) ((float) num / FIX_2_FLOAT(denom));
}

#endif