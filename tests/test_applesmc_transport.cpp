#include <cstring>

#include "smc/port_io_transport.h"
#include "smc/smc_sim.h"
#include "smc/transport_applesmc.h"
#include "test_framework.h"

using namespace fanforge;

namespace {

struct Fixture {
    SmcModel model = SmcModel::macBookProTwoFans();
    SimulatedKernelDevice device{model};
    ApplesmcTransport transport{device};
};

}  // namespace

TEST(applesmc_reports_its_name_and_protocol) {
    Fixture f;
    CHECK(std::string(f.transport.name()) == "applesmc");
    CHECK(f.transport.isOpen());
    CHECK(f.transport.protocol() == 1);  // mmio
}

TEST(applesmc_reads_a_temperature) {
    Fixture f;
    Value v;
    CHECK(f.transport.readKey(Key("TC0P"), 2, v));
    CHECK(v.size == 2);
    CHECK_NEAR(decodeValue(Key("sp78"), v.data, v.size).value(), 45.5, 0.001);
}

TEST(applesmc_reads_the_key_count_and_index) {
    Fixture f;
    Value count;
    CHECK(f.transport.readKey(Key("#KEY"), 4, count));
    const uint32_t declared = count.toU32();
    CHECK(declared == f.model.size());

    Key first;
    CHECK(f.transport.keyByIndex(0, first));
    bool found = false;
    for (uint32_t i = 0; i < declared; ++i) {
        Key k;
        CHECK(f.transport.keyByIndex(i, k));
        if (k == Key("F0Tg")) found = true;
    }
    CHECK(found);
}

TEST(applesmc_reports_metadata) {
    Fixture f;
    KeyInfo info;
    CHECK(f.transport.keyInfo(makeFanKey(0, "Ac"), info));
    CHECK(info.dataSize == 2);
    CHECK(Key(info.dataType) == Key("fpe2"));
    CHECK(info.readable());
    CHECK(!info.writable());

    CHECK(f.transport.keyInfo(Key("FS! "), info));
    CHECK(info.dataSize == 2);
    CHECK(info.writable());
}

TEST(applesmc_writes_a_target_and_reads_it_back) {
    Fixture f;
    uint8_t payload[2];
    CHECK(encodeValue(Key("fpe2"), 3000.0, payload, sizeof payload) == 2);
    CHECK(f.transport.writeKey(makeFanKey(1, "Tg"), payload, 2));

    Value v;
    CHECK(f.transport.readKey(makeFanKey(1, "Tg"), 2, v));
    CHECK_NEAR(decodeValue(Key("fpe2"), v.data, v.size).value(), 3000.0, 0.001);
}

TEST(applesmc_reads_fan_ids_as_strings) {
    Fixture f;
    KeyInfo info;
    CHECK(f.transport.keyInfo(Key("F0ID"), info));
    CHECK(Key(info.dataType) == Key("ch8*"));

    // A transport moves bytes and does not invent a type; the caller takes the
    // declared type from keyInfo, the way SmcDevice does.
    Value v;
    v.type = Key(info.dataType);
    CHECK(f.transport.readKey(Key("F0ID"), info.dataSize, v));
    CHECK(v.size == 16);
    CHECK(v.toText() == "Left side");  // padding trimmed
}

TEST(applesmc_reports_a_rejected_write_unlike_the_port_path) {
    // The device path does acknowledge, so a write the controller refuses is
    // reported as a failure rather than silently succeeding.
    Fixture f;
    const uint8_t payload[] = {0x37, 0x80};
    CHECK(!f.transport.writeKey(makeFanKey(0, "Mx"), payload, 2));
    CHECK(!f.transport.lastError().empty());
    CHECK_NEAR(f.model.valueOf("F0Mx").value(), 5927.0, 0.001);
}

TEST(applesmc_unknown_key_fails) {
    Fixture f;
    Value v;
    CHECK(!f.transport.readKey(Key("ZZZZ"), 2, v));
    KeyInfo info;
    CHECK(!f.transport.keyInfo(Key("ZZZZ"), info));
    Key k;
    CHECK(!f.transport.keyByIndex(999999, k));
}

TEST(applesmc_rejects_bad_requests_before_touching_the_device) {
    Fixture f;
    Value v;
    CHECK(!f.transport.readKey(Key("FNum"), 0, v));
    CHECK(!f.transport.readKey(Key("FNum"), 33, v));
    CHECK(!f.transport.writeKey(Key("FS! "), nullptr, 2));
    CHECK(!f.transport.writeKey(Key("FS! "), reinterpret_cast<const uint8_t*>("x"), 0));

    const int readsBefore = f.model.readCount;
    CHECK(!f.transport.readKey(Key("FNum"), 100, v));
    CHECK(f.model.readCount == readsBefore);  // never reached the device
}

TEST(applesmc_reports_a_device_that_is_not_open) {
    Fixture f;
    f.device.setOpen(false);
    CHECK(!f.transport.isOpen());
    Value v;
    CHECK(!f.transport.readKey(Key("FNum"), 1, v));
    CHECK(!f.transport.lastError().empty());
}

TEST(applesmc_surfaces_a_failed_ioctl) {
    Fixture f;
    f.model.failNextReads = 1;
    Value v;
    CHECK(!f.transport.readKey(Key("FNum"), 1, v));
    CHECK(f.transport.readKey(Key("FNum"), 1, v));
}

TEST(both_transports_agree_on_the_same_machine) {
    // Two access paths onto identical machine state must produce byte-identical
    // values, or the fallback path is not a real fallback.
    SmcModel a = SmcModel::macBookProTwoFans();
    SmcModel b = SmcModel::macBookProTwoFans();

    SimulatedSmcPortIo io{a};
    PortIoTransport port(io, 64);
    SimulatedKernelDevice dev{b};
    ApplesmcTransport applesmc(dev);

    // Every key the SMC exposes, each read at its declared size.
    Value count;
    CHECK(port.readKey(Key("#KEY"), 4, count));
    const uint32_t declared = count.toU32();
    CHECK(declared > 20);

    uint32_t compared = 0;
    for (uint32_t i = 0; i < declared; ++i) {
        Key k;
        CHECK(port.keyByIndex(i, k));

        KeyInfo info;
        CHECK(port.keyInfo(k, info));
        if (info.dataSize == 0 || info.dataSize > kMaxValueBytes) continue;

        Value viaPort, viaApplesmc;
        CHECK(port.readKey(k, info.dataSize, viaPort));
        CHECK(applesmc.readKey(k, info.dataSize, viaApplesmc));
        CHECK(viaPort.size == viaApplesmc.size);
        CHECK(std::memcmp(viaPort.data, viaApplesmc.data, viaPort.size) == 0);
        ++compared;
    }
    CHECK(compared == declared);
}
