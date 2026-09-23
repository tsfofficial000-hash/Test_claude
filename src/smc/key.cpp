#include "smc/key.h"

#include <cmath>
#include <cstdio>
#include <cstring>

namespace fanforge {
namespace {

// Fixed-point types are named <s|f>p<int-bits><frac-bits> where both digits
// are hexadecimal, and are always 16 bits wide:
//   sp78 = sign + 7 int + 8 frac  -> signed   /256   (temperatures)
//   fpe2 = 14 int   + 2 frac      -> unsigned /4     (fan speeds)
// The fraction shift is simply the numeric value of the fourth character.
int hexDigit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// True for the "sp??"-style fixed-point family.
bool isFixedPoint(Key type, std::string& text) {
    text = type.str();
    return text.size() == 4 && (text[0] == 's' || text[0] == 'f') && text[1] == 'p';
}

}  // namespace

Key::Key(const char* text) {
    char b[4] = {' ', ' ', ' ', ' '};
    if (text) {
        for (int i = 0; i < 4 && text[i]; ++i) b[i] = text[i];
    }
    // Little-endian in memory: byte 0 of the key becomes the low byte.
    raw = static_cast<uint32_t>(static_cast<uint8_t>(b[0])) |
          (static_cast<uint32_t>(static_cast<uint8_t>(b[1])) << 8) |
          (static_cast<uint32_t>(static_cast<uint8_t>(b[2])) << 16) |
          (static_cast<uint32_t>(static_cast<uint8_t>(b[3])) << 24);
}

std::string Key::str() const {
    char b[5];
    std::memcpy(b, &raw, 4);
    b[4] = '\0';
    for (int i = 0; i < 4; ++i) {
        const unsigned char c = static_cast<unsigned char>(b[i]);
        if (c < 32 || c > 126) b[i] = '.';
    }
    std::string s(b);
    while (!s.empty() && s.back() == ' ') s.pop_back();
    return s;
}

Key makeFanKey(int index, const char* suffix) {
    char b[5];
    std::snprintf(b, sizeof b, "F%d", index);
    // Pad the index to one character, which is what the SMC does; fan indices
    // never exceed the single-digit range on real hardware.
    char name[4] = {' ', ' ', ' ', ' '};
    name[0] = b[0];
    if (b[1]) name[1] = b[1];
    if (suffix) {
        name[2] = suffix[0];
        name[3] = suffix[1] ? suffix[1] : ' ';
    }
    return Key(name);  // Key(const char*) reads at most four chars
}

uint64_t bigEndianRead(const uint8_t* p, int n) {
    uint64_t v = 0;
    for (int i = 0; i < n; ++i) v = (v << 8) | p[i];
    return v;
}

void bigEndianWrite(uint8_t* p, int n, uint64_t v) {
    for (int i = n - 1; i >= 0; --i) {
        p[i] = static_cast<uint8_t>(v & 0xFF);
        v >>= 8;
    }
}

int64_t bigEndianReadSigned(const uint8_t* p, int n) {
    int64_t v = static_cast<int64_t>(bigEndianRead(p, n));
    if (n <= 0 || n > 8) return v;
    const int64_t sign = static_cast<int64_t>(1) << (n * 8 - 1);
    if (v & sign) v -= (sign << 1);
    return v;
}

std::optional<double> decodeValue(Key type, const uint8_t* data, uint32_t size) {
    if (!data || size == 0) return std::nullopt;
    const std::string t = type.str();

    if (t == "ui8" || t == "ui16" || t == "ui32" || t == "ui64") {
        return static_cast<double>(bigEndianRead(data, static_cast<int>(size < 8 ? size : 8)));
    }
    if (t == "si8" || t == "si16" || t == "si32" || t == "si64") {
        return static_cast<double>(bigEndianReadSigned(data, static_cast<int>(size < 8 ? size : 8)));
    }
    if (t == "flag") return data[0] ? 1.0 : 0.0;
    if (t == "flt" && size >= 4) {
        // The one little-endian type in the SMC.
        float f = 0.0f;
        std::memcpy(&f, data, 4);
        return static_cast<double>(f);
    }

    std::string text;
    if (isFixedPoint(type, text) && size >= 2) {
        const int frac = hexDigit(text[3]);
        if (frac < 0 || frac > 16) return std::nullopt;
        const double scale = static_cast<double>(1u << frac);
        if (text[0] == 's') return static_cast<double>(bigEndianReadSigned(data, 2)) / scale;
        return static_cast<double>(bigEndianRead(data, 2)) / scale;
    }
    return std::nullopt;
}

uint32_t encodeValue(Key type, double value, uint8_t* out, uint32_t capacity) {
    if (!out || capacity == 0) return 0;
    const std::string t = type.str();

    auto putUnsigned = [&](int n, double v) -> uint32_t {
        if (static_cast<uint32_t>(n) > capacity) return 0;
        if (!(v > 0.0)) v = 0.0;  // also catches NaN
        const double maxv = std::pow(256.0, n) - 1.0;
        if (v > maxv) v = maxv;
        bigEndianWrite(out, n, static_cast<uint64_t>(std::llround(v)));
        return static_cast<uint32_t>(n);
    };
    auto putSigned = [&](int n, double v) -> uint32_t {
        if (static_cast<uint32_t>(n) > capacity) return 0;
        if (!(v == v)) v = 0.0;  // NaN
        const double limit = std::pow(2.0, n * 8 - 1);
        if (v < -limit) v = -limit;
        if (v > limit - 1.0) v = limit - 1.0;
        bigEndianWrite(out, n, static_cast<uint64_t>(static_cast<int64_t>(std::llround(v))));
        return static_cast<uint32_t>(n);
    };

    if (t == "ui8") return putUnsigned(1, value);
    if (t == "ui16") return putUnsigned(2, value);
    if (t == "ui32") return putUnsigned(4, value);
    if (t == "si8") return putSigned(1, value);
    if (t == "si16") return putSigned(2, value);
    if (t == "si32") return putSigned(4, value);
    if (t == "flag") {
        if (capacity < 1) return 0;
        out[0] = (value != 0.0) ? 1 : 0;
        return 1;
    }
    if (t == "flt") {
        if (capacity < 4) return 0;
        const float f = static_cast<float>(value);
        std::memcpy(out, &f, 4);  // little-endian, matching the decode path
        return 4;
    }

    std::string text;
    if (isFixedPoint(type, text)) {
        const int frac = hexDigit(text[3]);
        if (frac < 0 || frac > 16) return 0;
        const double scaled = value * static_cast<double>(1u << frac);
        return (text[0] == 's') ? putSigned(2, scaled) : putUnsigned(2, scaled);
    }
    return 0;
}

std::optional<double> Value::toDouble() const { return decodeValue(type, data, size); }

uint32_t Value::toU32() const {
    if (size == 0) return 0;
    const uint32_t n = size < 4 ? size : 4;
    return static_cast<uint32_t>(bigEndianRead(data, static_cast<int>(n)));
}

std::string Value::toText() const {
    if (auto d = toDouble()) {
        char b[64];
        std::snprintf(b, sizeof b, "%.4g", *d);
        return b;
    }
    const std::string t = type.str();
    if (t.rfind("ch8", 0) == 0) {
        std::string s;
        for (uint32_t i = 0; i < size; ++i) {
            const uint8_t c = data[i];
            if (c == 0) break;
            s += (c >= 32 && c < 127) ? static_cast<char>(c) : '.';
        }
        // The SMC space-pads fixed-width strings (fan labels especially), so
        // trailing whitespace is padding, not content.
        while (!s.empty() && s.back() == ' ') s.pop_back();
        return s;
    }
    std::string hex;
    char b[8];
    for (uint32_t i = 0; i < size; ++i) {
        std::snprintf(b, sizeof b, "%02X", data[i]);
        hex += b;
    }
    return hex;
}

}  // namespace fanforge
