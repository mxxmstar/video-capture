#include "media/media_timestamp.h"
#include <limits>
std::int64_t RescaleMediaTimestamp(std::int64_t value, Rational source, Rational target) noexcept {
    if (!IsValidTimestamp(value) || !IsValidTimeBase(source) || !IsValidTimeBase(target)) return kNoTimestamp;
    const auto a = static_cast<std::uint64_t>(value < 0 ? -value : value);
    const auto b = static_cast<std::uint64_t>(source.num) * target.den;
    const auto d = static_cast<std::uint64_t>(source.den) * target.num;
    // Portable 128-bit product; no compiler-specific integer extension required.
    std::uint64_t hi = 0, lo = 0, xhi = 0, xlo = a;
    for (auto bits = b; bits; bits >>= 1) {
        if (bits & 1) { const auto before = lo; lo += xlo; hi += xhi + (lo < before); }
        xhi = (xhi << 1) | (xlo >> 63); xlo <<= 1;
    }
    std::uint64_t remainder = 0, quotient = 0;
    for (int bit = 127; bit >= 0; --bit) {
        remainder = (remainder << 1) | ((bit >= 64 ? hi >> (bit - 64) : lo >> bit) & 1);
        if (remainder >= d) {
            remainder -= d;
            if (bit >= 63) return kNoTimestamp;
            quotient |= std::uint64_t{1} << bit;
        }
    }
    if (remainder >= (d + 1) / 2) ++quotient;
    if (quotient > static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)())) return kNoTimestamp;
    return value < 0 ? -static_cast<std::int64_t>(quotient) : static_cast<std::int64_t>(quotient);
}
