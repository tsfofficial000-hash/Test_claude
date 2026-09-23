#pragma once
//
// SMC key and value types.
//
// The Apple System Management Controller exposes a flat namespace of
// four-character keys. Each key has a declared size and a declared *type*
// which says how to interpret its bytes. Nothing here is model specific: the
// type is always read from the hardware, never assumed.
//
// Protocol facts encoded in this file were reimplemented from the public
// documentation of the SMC interface (the Linux `applesmc` driver and the
// Darwin `AppleSMC` interface). See README.md, "Protocol references".
//
#include <cstdint>
#include <optional>
#include <string>

namespace fanforge {

// A four-character SMC key, stored in wire order as a little-endian u32.
// 'F0Ac' -> bytes 46 30 41 63 -> 0x63413046.
struct Key {
    uint32_t raw = 0;

    Key() = default;
    explicit Key(uint32_t r) : raw(r) {}
    // Accepts up to four characters, space padding shorter names the way the
    // SMC itself pads them ("FS!" is really "FS! ").
    explicit Key(const char* text);

    std::string str() const;  // printable form, trailing pad spaces removed
    bool empty() const { return raw == 0; }

    bool operator==(const Key& o) const { return raw == o.raw; }
    bool operator!=(const Key& o) const { return raw != o.raw; }
    bool operator<(const Key& o) const { return raw < o.raw; }
};

// Builds the key for the `index`-th fan with the given two-character suffix,
// e.g. makeFanKey(1, "Ac") -> "F1Ac".
Key makeFanKey(int index, const char* suffix);

// Well-known keys.
namespace keys {
// These are the four key characters reinterpreted as a little-endian u32, the
// same encoding Key(const char*) produces: 'F' 'S' '!' ' ' -> 0x20215346.
constexpr uint32_t kKeyCount = 0x59454B23;  // "#KEY" -> ui32, total key count
constexpr uint32_t kFanCount = 0x6D754E46;  // "FNum" -> ui8,  number of fans
constexpr uint32_t kFanForce = 0x20215346;  // "FS! " -> ui16, manual-force bitmask
}  // namespace keys

// The SMC's own hard limit on a key's payload.
constexpr uint8_t kMaxValueBytes = 32;

// What the SMC reports about a key: how many bytes it holds, its type, and its
// access flags.
struct KeyInfo {
    uint8_t dataSize = 0;
    uint32_t dataType = 0;
    uint8_t dataAttributes = 0;

    bool readable() const { return (dataAttributes & kAttrReadable) != 0; }
    bool writable() const { return (dataAttributes & kAttrWritable) != 0; }
    bool function() const { return (dataAttributes & kAttrFunction) != 0; }
    bool plausiblySane() const { return dataSize != 0 && dataSize <= kMaxValueBytes; }

    static constexpr uint8_t kAttrReadable = 0x80;
    static constexpr uint8_t kAttrWritable = 0x40;
    static constexpr uint8_t kAttrFunction = 0x10;
};

constexpr uint8_t kAttrReadable = KeyInfo::kAttrReadable;
constexpr uint8_t kAttrWritable = KeyInfo::kAttrWritable;
constexpr uint8_t kAttrFunction = KeyInfo::kAttrFunction;

// A key's bytes plus the type needed to interpret them.
struct Value {
    Key type;
    uint8_t data[kMaxValueBytes] = {};
    uint32_t size = 0;

    // Decodes using the declared type. Returns nullopt for non-numeric types
    // ("ch8*" strings, "{fds" structs, raw "hex_" blobs).
    std::optional<double> toDouble() const;

    // Best-effort display form: the decoded number, a printable string for
    // "ch8*", otherwise uppercase hex.
    std::string toText() const;

    // Unsigned interpretation, independent of declared type. Used for keys
    // whose payload is a flag/mask (FS!, FNum).
    uint32_t toU32() const;

    bool empty() const { return size == 0; }
};

// Type-driven interpretation. `size` is the number of valid bytes in `data`.
std::optional<double> decodeValue(Key type, const uint8_t* data, uint32_t size);

// Type-driven encoding. Returns the number of bytes written, or 0 when the
// type is not encodable. Values are clamped to the representable range rather
// than wrapping, so a bad request can never produce a wild target.
uint32_t encodeValue(Key type, double value, uint8_t* out, uint32_t capacity);

// The SMC is big-endian for every integer and fixed-point type. 'flt' is the
// documented exception: a native little-endian IEEE-754 float.
uint64_t bigEndianRead(const uint8_t* p, int n);
void bigEndianWrite(uint8_t* p, int n, uint64_t v);
int64_t bigEndianReadSigned(const uint8_t* p, int n);

}  // namespace fanforge
