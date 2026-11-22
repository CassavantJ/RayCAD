// Exact decimal <-> binary floating-point conversion (see json_number.h).
//
// Reading: the decimal significand M and exponent E give the exact rational M * 10^E as num / den. It is scaled by
// a power of two so the integer quotient has two bits more than the target significand, the quotient is found by
// binary long division, and the remainder becomes a sticky bit for round-half-even.
//
// Writing: the free-format shortest-digit algorithm of Steele and White as refined by Burger and Dybvig ("Printing
// floating-point numbers quickly and accurately", 1996), on exact big integers. The interval of decimals that read
// back to the value includes its end points when the significand is even, matching round-half-even on reading.
// When both the digit d and d + 1 would read back, the closer one is taken, and an exact tie takes the even digit.
#include "json_number.h"

#include <bit>
#include <cstdint>
#include <vector>

namespace ray::cam::detail {
namespace {

// Unsigned big integer, little-endian 32-bit limbs, no high zero limbs.
class Big {
public:
    Big() = default;
    explicit Big(std::uint64_t value)
    {
        while (value != 0) {
            limbs_.push_back(static_cast<std::uint32_t>(value));
            value >>= 32;
        }
    }

    bool zero() const { return limbs_.empty(); }

    int bitLength() const
    {
        if (limbs_.empty()) {
            return 0;
        }
        return static_cast<int>(limbs_.size() - 1) * 32 + static_cast<int>(std::bit_width(limbs_.back()));
    }

    void mulSmall(std::uint32_t factor)
    {
        std::uint64_t carry = 0;
        for (std::uint32_t& limb : limbs_) {
            const std::uint64_t t = static_cast<std::uint64_t>(limb) * factor + carry;
            limb = static_cast<std::uint32_t>(t);
            carry = t >> 32;
        }
        if (carry != 0) {
            limbs_.push_back(static_cast<std::uint32_t>(carry));
        }
        trim();
    }

    void addSmall(std::uint32_t value)
    {
        std::uint64_t carry = value;
        for (std::size_t i = 0; carry != 0; ++i) {
            if (i == limbs_.size()) {
                limbs_.push_back(0);
            }
            const std::uint64_t t = static_cast<std::uint64_t>(limbs_[i]) + carry;
            limbs_[i] = static_cast<std::uint32_t>(t);
            carry = t >> 32;
        }
    }

    void mulPow10(int n)
    {
        static constexpr std::uint32_t kPow10[] = {1, 10, 100, 1000, 10000, 100000, 1000000, 10000000, 100000000};
        while (n >= 9) {
            mulSmall(1000000000u);
            n -= 9;
        }
        if (n > 0) {
            mulSmall(kPow10[n]);
        }
    }

    void shiftLeft(int n)
    {
        if (limbs_.empty() || n <= 0) {
            return;
        }
        const std::size_t words = static_cast<std::size_t>(n / 32);
        const int bits = n % 32;
        if (bits != 0) {
            std::uint32_t carry = 0;
            for (std::uint32_t& limb : limbs_) {
                const std::uint32_t next = limb >> (32 - bits);
                limb = (limb << bits) | carry;
                carry = next;
            }
            if (carry != 0) {
                limbs_.push_back(carry);
            }
        }
        limbs_.insert(limbs_.begin(), words, 0u);
    }

    void shiftRight1()
    {
        for (std::size_t i = 0; i < limbs_.size(); ++i) {
            const std::uint32_t high = i + 1 < limbs_.size() ? limbs_[i + 1] << 31 : 0u;
            limbs_[i] = (limbs_[i] >> 1) | high;
        }
        trim();
    }

    int compare(const Big& other) const
    {
        if (limbs_.size() != other.limbs_.size()) {
            return limbs_.size() < other.limbs_.size() ? -1 : 1;
        }
        for (std::size_t i = limbs_.size(); i-- > 0;) {
            if (limbs_[i] != other.limbs_[i]) {
                return limbs_[i] < other.limbs_[i] ? -1 : 1;
            }
        }
        return 0;
    }

    void add(const Big& other)
    {
        if (limbs_.size() < other.limbs_.size()) {
            limbs_.resize(other.limbs_.size(), 0u);
        }
        std::uint64_t carry = 0;
        for (std::size_t i = 0; i < limbs_.size(); ++i) {
            const std::uint64_t t =
                static_cast<std::uint64_t>(limbs_[i]) + (i < other.limbs_.size() ? other.limbs_[i] : 0u) + carry;
            limbs_[i] = static_cast<std::uint32_t>(t);
            carry = t >> 32;
        }
        if (carry != 0) {
            limbs_.push_back(static_cast<std::uint32_t>(carry));
        }
    }

    // Requires *this >= other.
    void sub(const Big& other)
    {
        std::int64_t borrow = 0;
        for (std::size_t i = 0; i < limbs_.size(); ++i) {
            std::int64_t t = static_cast<std::int64_t>(limbs_[i]) - borrow -
                             static_cast<std::int64_t>(i < other.limbs_.size() ? other.limbs_[i] : 0u);
            borrow = 0;
            if (t < 0) {
                t += static_cast<std::int64_t>(1) << 32;
                borrow = 1;
            }
            limbs_[i] = static_cast<std::uint32_t>(t);
        }
        trim();
    }

private:
    void trim()
    {
        while (!limbs_.empty() && limbs_.back() == 0) {
            limbs_.pop_back();
        }
    }

    std::vector<std::uint32_t> limbs_;
};

// An IEEE-754 binary format. A finite value is m * 2^e with m < 2^mantBits (the hidden bit included) and
// minExp <= e <= maxExp; m >= 2^(mantBits - 1) for normal values.
struct Format {
    int mantBits;
    int expBits;
    int minExp;
    int maxExp;
};
constexpr Format kBinary64{53, 11, -1074, 971};
constexpr Format kBinary32{24, 8, -149, 104};

// ---------- Reading ----------

// Significant digits kept from the input. Any double is decided by its first 768 significant digits, so the
// digits beyond these only matter through whether one of them is non-zero (a sticky digit).
constexpr std::size_t kMaxDigits = 800;

struct Decimal {
    bool negative = false;
    std::string digits; // significant digits, no leading or trailing zeros; empty for zero
    long long exponent = 0;
};

bool isDigit(char c)
{
    return c >= '0' && c <= '9';
}

std::optional<Decimal> parseDecimal(std::string_view text)
{
    if (text.empty() || scanNumber(text) != text.size()) {
        return std::nullopt;
    }
    Decimal out;
    std::size_t i = 0;
    if (text[i] == '-') {
        out.negative = true;
        ++i;
    }
    std::string all;
    long long fractionDigits = 0;
    while (i < text.size() && isDigit(text[i])) {
        all.push_back(text[i++]);
    }
    if (i < text.size() && text[i] == '.') {
        ++i;
        while (i < text.size() && isDigit(text[i])) {
            all.push_back(text[i++]);
            ++fractionDigits;
        }
    }
    long long exponent = 0;
    if (i < text.size() && (text[i] == 'e' || text[i] == 'E')) {
        ++i;
        bool negativeExponent = false;
        if (text[i] == '+' || text[i] == '-') {
            negativeExponent = text[i] == '-';
            ++i;
        }
        while (i < text.size()) {
            // Saturate: anything this large is zero or out of range anyway.
            if (exponent < 100000000) {
                exponent = exponent * 10 + (text[i] - '0');
            }
            ++i;
        }
        if (negativeExponent) {
            exponent = -exponent;
        }
    }
    std::size_t first = all.find_first_not_of('0');
    if (first == std::string::npos) {
        return out; // zero
    }
    std::size_t last = all.find_last_not_of('0');
    out.exponent = exponent - fractionDigits + static_cast<long long>(all.size() - 1 - last);
    out.digits = all.substr(first, last - first + 1);
    if (out.digits.size() > kMaxDigits) {
        const std::size_t dropped = out.digits.size() - kMaxDigits;
        out.digits.resize(kMaxDigits);
        out.exponent += static_cast<long long>(dropped);
        // A dropped digit was non-zero (the last digit is never zero): add a sticky digit.
        out.digits.push_back('1');
        out.exponent -= 1;
    }
    return out;
}

// Returns the bit pattern (without sign), or nullopt on overflow.
std::optional<std::uint64_t> toBinary(const Decimal& decimal, const Format& fmt)
{
    if (decimal.digits.empty()) {
        return 0;
    }
    const long long magnitude = decimal.exponent + static_cast<long long>(decimal.digits.size());
    if (magnitude > 400) {
        return std::nullopt;
    }
    if (magnitude < -400) {
        return 0;
    }
    Big num;
    for (const char c : decimal.digits) {
        num.mulSmall(10);
        num.addSmall(static_cast<std::uint32_t>(c - '0'));
    }
    Big den(1);
    if (decimal.exponent >= 0) {
        num.mulPow10(static_cast<int>(decimal.exponent));
    } else {
        den.mulPow10(static_cast<int>(-decimal.exponent));
    }
    // Scale so q = floor(num / den) has mantBits + 1 or mantBits + 2 bits.
    const int s = fmt.mantBits + 1 - (num.bitLength() - den.bitLength());
    if (s >= 0) {
        num.shiftLeft(s);
    } else {
        den.shiftLeft(-s);
    }
    const int quotientBits = fmt.mantBits + 2;
    Big divisor = den;
    divisor.shiftLeft(quotientBits - 1);
    std::uint64_t q = 0;
    for (int i = quotientBits - 1; i >= 0; --i) {
        if (num.compare(divisor) >= 0) {
            num.sub(divisor);
            q |= std::uint64_t{1} << i;
        }
        divisor.shiftRight1();
    }
    bool sticky = !num.zero();
    const int lsb = -s; // the exact value is (q + fraction) * 2^lsb
    const int qBits = static_cast<int>(std::bit_width(q));
    int target = lsb + qBits - fmt.mantBits; // exponent of the result's lowest significand bit
    if (target < fmt.minExp) {
        target = fmt.minExp;
    }
    const int drop = target - lsb; // >= 1
    std::uint64_t m = 0;
    bool guard = false;
    if (drop >= 64) {
        guard = drop == 64 && (q >> 63) != 0;
        sticky = sticky || (drop == 64 ? (q & ~(std::uint64_t{1} << 63)) != 0 : q != 0);
    } else {
        guard = ((q >> (drop - 1)) & 1u) != 0;
        sticky = sticky || (q & ((std::uint64_t{1} << (drop - 1)) - 1)) != 0;
        m = q >> drop;
    }
    if (guard && (sticky || (m & 1u) != 0)) {
        ++m;
        if (m == (std::uint64_t{1} << fmt.mantBits)) {
            m >>= 1;
            ++target;
        }
    }
    if (target > fmt.maxExp) {
        return std::nullopt;
    }
    const std::uint64_t hidden = std::uint64_t{1} << (fmt.mantBits - 1);
    if (m >= hidden) {
        const auto biased = static_cast<std::uint64_t>(target - fmt.minExp + 1);
        return (biased << (fmt.mantBits - 1)) | (m & (hidden - 1));
    }
    return m; // subnormal or zero; target == minExp
}

// ---------- Writing ----------

// ECMAScript Number::toString layout of digits d1..dk with value 0.d1..dk * 10^n.
std::string layout(bool negative, const std::string& digits, int n)
{
    const int k = static_cast<int>(digits.size());
    std::string out;
    if (negative) {
        out.push_back('-');
    }
    if (k <= n && n <= 21) {
        out += digits;
        out.append(static_cast<std::size_t>(n - k), '0');
    } else if (0 < n && n <= 21) {
        out.append(digits, 0, static_cast<std::size_t>(n));
        out.push_back('.');
        out.append(digits, static_cast<std::size_t>(n), std::string::npos);
    } else if (-6 < n && n <= 0) {
        out += "0.";
        out.append(static_cast<std::size_t>(-n), '0');
        out += digits;
    } else {
        out.push_back(digits[0]);
        if (k > 1) {
            out.push_back('.');
            out.append(digits, 1, std::string::npos);
        }
        out.push_back('e');
        int e = n - 1;
        out.push_back(e < 0 ? '-' : '+');
        if (e < 0) {
            e = -e;
        }
        std::string exponent;
        do {
            exponent.insert(exponent.begin(), static_cast<char>('0' + e % 10));
            e /= 10;
        } while (e != 0);
        out += exponent;
    }
    return out;
}

std::string formatBits(std::uint64_t bits, const Format& fmt)
{
    const int signBit = fmt.mantBits - 1 + fmt.expBits;
    const bool negative = ((bits >> signBit) & 1u) != 0;
    const std::uint64_t fractionMask = (std::uint64_t{1} << (fmt.mantBits - 1)) - 1;
    const auto biased = static_cast<int>((bits >> (fmt.mantBits - 1)) & ((std::uint64_t{1} << fmt.expBits) - 1));
    const std::uint64_t fraction = bits & fractionMask;
    if (biased == (1 << fmt.expBits) - 1) {
        return "null"; // infinity or NaN
    }
    if (biased == 0 && fraction == 0) {
        return "0"; // also -0
    }
    std::uint64_t m = fraction;
    int e = fmt.minExp;
    if (biased != 0) {
        m |= fractionMask + 1;
        e = fmt.minExp + biased - 1;
    }
    const bool even = (m & 1u) == 0;
    const bool unequalGaps = m == fractionMask + 1 && biased > 1;

    // v = r / s, the upper gap is mPlus / s and the lower gap mMinus / s (half the distance to the neighbours).
    Big r(m);
    Big s(1);
    Big mPlus(1);
    Big mMinus(1);
    if (e >= 0) {
        if (!unequalGaps) {
            r.shiftLeft(e + 1);
            s = Big(2);
            mPlus.shiftLeft(e);
            mMinus.shiftLeft(e);
        } else {
            r.shiftLeft(e + 2);
            s = Big(4);
            mPlus.shiftLeft(e + 1);
            mMinus.shiftLeft(e);
        }
    } else {
        if (!unequalGaps) {
            r.shiftLeft(1);
            s.shiftLeft(1 - e);
        } else {
            r.shiftLeft(2);
            s.shiftLeft(2 - e);
            mPlus = Big(2);
        }
    }

    // Estimate k = ceil(log10(v)); never too high, possibly too low, fixed below.
    const int topExponent = e + static_cast<int>(std::bit_width(m)) - 1; // v is in [2^topExponent, 2^(topExponent + 1))
    const double estimate = topExponent * 0.30102999566398119521 - 1e-10;
    int k = static_cast<int>(estimate);
    if (static_cast<double>(k) < estimate) {
        ++k;
    }
    if (k >= 0) {
        s.mulPow10(k);
    } else {
        r.mulPow10(-k);
        mPlus.mulPow10(-k);
        mMinus.mulPow10(-k);
    }
    auto highReached = [&](const Big& remainder) {
        Big high = remainder;
        high.add(mPlus);
        const int c = high.compare(s);
        return even ? c >= 0 : c > 0;
    };
    while (highReached(r)) {
        s.mulSmall(10);
        ++k;
    }

    std::string digits;
    for (;;) {
        r.mulSmall(10);
        mPlus.mulSmall(10);
        mMinus.mulSmall(10);
        int d = 0;
        while (r.compare(s) >= 0) {
            r.sub(s);
            ++d;
        }
        const int lowCompare = r.compare(mMinus);
        const bool low = even ? lowCompare <= 0 : lowCompare < 0;
        const bool high = highReached(r);
        if (!low && !high) {
            digits.push_back(static_cast<char>('0' + d));
            continue;
        }
        if (low && high) {
            Big twice = r;
            twice.shiftLeft(1);
            const int c = twice.compare(s);
            if (c > 0 || (c == 0 && d % 2 != 0)) {
                ++d;
            }
        } else if (high) {
            ++d;
        }
        digits.push_back(static_cast<char>('0' + d));
        break;
    }
    return layout(negative, digits, k);
}

} // namespace

std::size_t scanNumber(std::string_view text)
{
    std::size_t i = 0;
    if (i < text.size() && text[i] == '-') {
        ++i;
    }
    if (i >= text.size() || !isDigit(text[i])) {
        return 0;
    }
    if (text[i] == '0') {
        ++i;
    } else {
        while (i < text.size() && isDigit(text[i])) {
            ++i;
        }
    }
    if (i + 1 < text.size() && text[i] == '.' && isDigit(text[i + 1])) {
        i += 2;
        while (i < text.size() && isDigit(text[i])) {
            ++i;
        }
    }
    if (i < text.size() && (text[i] == 'e' || text[i] == 'E')) {
        std::size_t j = i + 1;
        if (j < text.size() && (text[j] == '+' || text[j] == '-')) {
            ++j;
        }
        if (j < text.size() && isDigit(text[j])) {
            while (j < text.size() && isDigit(text[j])) {
                ++j;
            }
            i = j;
        }
    }
    return i;
}

std::string formatDouble(double value)
{
    return formatBits(std::bit_cast<std::uint64_t>(value), kBinary64);
}

std::string formatFloat(float value)
{
    return formatBits(std::bit_cast<std::uint32_t>(value), kBinary32);
}

std::optional<double> parseDouble(std::string_view text)
{
    const auto decimal = parseDecimal(text);
    if (!decimal) {
        return std::nullopt;
    }
    const auto bits = toBinary(*decimal, kBinary64);
    if (!bits) {
        return std::nullopt;
    }
    return std::bit_cast<double>(*bits | (decimal->negative ? std::uint64_t{1} << 63 : 0u));
}

std::optional<float> parseFloat(std::string_view text)
{
    const auto decimal = parseDecimal(text);
    if (!decimal) {
        return std::nullopt;
    }
    const auto bits = toBinary(*decimal, kBinary32);
    if (!bits) {
        return std::nullopt;
    }
    return std::bit_cast<float>(static_cast<std::uint32_t>(*bits) | (decimal->negative ? 0x80000000u : 0u));
}

} // namespace ray::cam::detail
