#include <cstring>

#include "smc/key.h"
#include "test_framework.h"

using namespace fanforge;

TEST(key_wire_order_matches_the_hardware) {
    // 'F0Ac' must land as 0x63413046 - the little-endian reinterpretation the
    // APPLESMC driver expects.
    CHECK(Key("F0Ac").raw == 0x63413046u);
    // Short names are space padded, exactly as the SMC pads its own keys.
    CHECK(Key("FS!").raw == Key("FS! ").raw);
    CHECK(Key("ui8").raw == Key("ui8 ").raw);
    CHECK(Key("ui8").str() == "ui8");  // ...and trimmed again for display.
    CHECK(Key("F0Ac").raw != Key("F1Ac").raw);
}

TEST(key_text_round_trip) {
    CHECK(Key("F0Ac").str() == "F0Ac");
    CHECK(Key("FNum").str() == "FNum");
    // Trailing padding is stripped for display only.
    CHECK(Key("FS! ").str() == "FS!");
    // A null or empty name is four spaces.
    CHECK(Key("").str().empty());
    CHECK(Key(nullptr).str().empty());
}

TEST(key_chars_are_the_raw_bytes) {
    uint32_t raw = Key("TC0P").raw;
    char b[4];
    std::memcpy(b, &raw, 4);
    CHECK(b[0] == 'T');
    CHECK(b[1] == 'C');
    CHECK(b[2] == '0');
    CHECK(b[3] == 'P');
}

TEST(make_fan_key_builds_the_family) {
    CHECK(makeFanKey(0, "Ac") == Key("F0Ac"));
    CHECK(makeFanKey(1, "Tg") == Key("F1Tg"));
    CHECK(makeFanKey(0, "Mn") == Key("F0Mn"));
    CHECK(makeFanKey(1, "ID") == Key("F1ID"));
}

TEST(fpe2_decodes_fan_speeds) {
    // 3552 RPM * 4 = 14208 = 0x3780
    const uint8_t bytes[] = {0x37, 0x80};
    auto v = decodeValue(Key("fpe2"), bytes, 2);
    CHECK(v.has_value());
    CHECK_NEAR(*v, 3552.0, 0.001);
}

TEST(sp78_decodes_temperatures) {
    // 52.25 C * 256 = 13376 = 0x3440
    const uint8_t bytes[] = {0x34, 0x40};
    auto v = decodeValue(Key("sp78"), bytes, 2);
    CHECK(v.has_value());
    CHECK_NEAR(*v, 52.25, 0.001);

    // Negative temperatures use the signed interpretation.
    const uint8_t cold[] = {0xFF, 0x00};  // -256 / 256 = -1.0
    auto c = decodeValue(Key("sp78"), cold, 2);
    CHECK(c.has_value());
    CHECK_NEAR(*c, -1.0, 0.001);
}

TEST(dead_sensor_sentinels_are_visible_to_the_caller) {
    // -127 C is the sentinel a dead sensor reports.
    const uint8_t bytes[] = {0x81, 0x00};  // -32512 / 256 = -127.0
    auto v = decodeValue(Key("sp78"), bytes, 2);
    CHECK(v.has_value());
    CHECK_NEAR(*v, -127.0, 0.001);
}

TEST(integers_are_big_endian) {
    const uint8_t u16[] = {0x13, 0x88};  // 5000 mV
    auto v16 = decodeValue(Key("ui16"), u16, 2);
    CHECK(v16.has_value());
    CHECK_NEAR(*v16, 5000.0, 0.001);

    const uint8_t u32[] = {0x00, 0x00, 0x03, 0x8F};  // 911 keys
    auto v32 = decodeValue(Key("ui32"), u32, 4);
    CHECK(v32.has_value());
    CHECK_NEAR(*v32, 911.0, 0.001);

    const uint8_t s8[] = {0xFE};  // -2
    auto vs = decodeValue(Key("si8"), s8, 1);
    CHECK(vs.has_value());
    CHECK_NEAR(*vs, -2.0, 0.001);
}

TEST(flt_is_little_endian_unlike_every_other_type) {
    // 12.5f == 0x41480000; native little-endian storage is 00 00 48 41.
    const uint8_t le[] = {0x00, 0x00, 0x48, 0x41};
    auto v = decodeValue(Key("flt"), le, 4);
    CHECK(v.has_value());
    CHECK_NEAR(*v, 12.5, 0.0001);

    // The same 12.5 laid out big-endian decodes to a denormal near zero. This
    // is exactly the trap the endianness choice exists to avoid: get it wrong
    // and every power reading on the machine reads as zero.
    const uint8_t be[] = {0x41, 0x48, 0x00, 0x00};
    auto wrong = decodeValue(Key("flt"), be, 4);
    CHECK(wrong.has_value());
    CHECK(*wrong >= 0.0);
    CHECK(*wrong < 1e-30);
}

TEST(flag_and_string_types) {
    const uint8_t one[] = {1};
    const uint8_t zero[] = {0};
    CHECK(decodeValue(Key("flag"), one, 1).value() == 1.0);
    CHECK(decodeValue(Key("flag"), zero, 1).value() == 0.0);

    // Non-numeric types decode to nothing rather than a wrong number.
    const uint8_t text[] = {'1', '.', '3', '0'};
    CHECK(!decodeValue(Key("ch8*"), text, 4).has_value());
    CHECK(!decodeValue(Key("hex_"), text, 4).has_value());
}

TEST(string_values_render_as_text) {
    Value v;
    v.type = Key("ch8*");
    v.size = 4;
    std::memcpy(v.data, "1.30", 4);
    CHECK(v.toText() == "1.30");

    Value hex;
    hex.type = Key("hex_");
    hex.size = 2;
    hex.data[0] = 0xAB;
    hex.data[1] = 0x0C;
    CHECK(hex.toText() == "AB0C");
}

TEST(encode_matches_decode) {
    uint8_t out[8] = {};
    uint32_t n = encodeValue(Key("fpe2"), 3552.0, out, sizeof out);
    CHECK(n == 2);
    CHECK(out[0] == 0x37);
    CHECK(out[1] == 0x80);
    CHECK_NEAR(decodeValue(Key("fpe2"), out, 2).value(), 3552.0, 0.001);

    n = encodeValue(Key("sp78"), 52.25, out, sizeof out);
    CHECK(n == 2);
    CHECK_NEAR(decodeValue(Key("sp78"), out, 2).value(), 52.25, 0.001);

    n = encodeValue(Key("flt"), 12.5, out, sizeof out);
    CHECK(n == 4);
    CHECK(out[0] == 0x00);
    CHECK(out[3] == 0x41);
    CHECK_NEAR(decodeValue(Key("flt"), out, 4).value(), 12.5, 0.0001);

    n = encodeValue(Key("ui16"), 5000.0, out, sizeof out);
    CHECK(n == 2);
    CHECK(out[0] == 0x13);
    CHECK(out[1] == 0x88);
}

TEST(encode_clamps_instead_of_wrapping) {
    uint8_t out[8] = {};

    // A fan speed beyond the 16-bit fixed-point ceiling saturates rather than
    // wrapping to a nonsense low value.
    CHECK(encodeValue(Key("fpe2"), 1e9, out, sizeof out) == 2);
    CHECK(out[0] == 0xFF);
    CHECK(out[1] == 0xFF);

    CHECK(encodeValue(Key("fpe2"), -500.0, out, sizeof out) == 2);
    CHECK(out[0] == 0x00);
    CHECK(out[1] == 0x00);

    // Same for signed and unsigned integers.
    CHECK(encodeValue(Key("ui8"), 999.0, out, sizeof out) == 1);
    CHECK(out[0] == 0xFF);

    CHECK(encodeValue(Key("sp78"), 1e9, out, sizeof out) == 2);
    CHECK(out[0] == 0x7F);
    CHECK(out[1] == 0xFF);
}

TEST(encode_refuses_what_it_cannot_represent) {
    uint8_t out[8] = {};
    // A 4-byte float cannot fit in a 1-byte capacity.
    CHECK(encodeValue(Key("flt"), 1.0, out, 2) == 0);
    // Non-numeric types are not encodable.
    CHECK(encodeValue(Key("ch8*"), 1.0, out, sizeof out) == 0);
    CHECK(encodeValue(Key("hex_"), 1.0, out, sizeof out) == 0);
    // A malformed fixed-point name is rejected, not guessed at.
    CHECK(encodeValue(Key("spzz"), 1.0, out, sizeof out) == 0);
}
