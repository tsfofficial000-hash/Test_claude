#include <cstring>

#include "smc/device.h"
#include "smc/port_io_transport.h"
#include "smc/smc_sim.h"
#include "smc/transport_applesmc.h"
#include "test_framework.h"

using namespace fanforge;

namespace {

struct Rig {
    SmcModel model = SmcModel::macBookProTwoFans();
    SimulatedSmcPortIo io{model};
    PortIoTransport transport{io, 64};
    SmcDevice smc{transport};
};

}  // namespace

TEST(device_fills_in_the_declared_type_when_reading) {
    Rig r;
    Value v;
    CHECK(r.smc.read(Key("TC0P"), v));
    CHECK(Key(v.type) == Key("sp78"));
    CHECK(v.size == 2);
    CHECK_NEAR(v.toDouble().value(), 45.5, 0.001);

    Value id;
    CHECK(r.smc.read(Key("F0ID"), id));
    CHECK(Key(id.type) == Key("ch8*"));
    CHECK(id.toText() == "Left side");
}

TEST(device_reads_numbers) {
    Rig r;
    double value = 0;
    CHECK(r.smc.readNumber(Key("F0Ac"), value));
    CHECK_NEAR(value, 3552.0, 0.001);

    CHECK(r.smc.readNumber(Key("TC0D"), value));
    CHECK_NEAR(value, 52.25, 0.001);

    // A type the decoder cannot turn into a number reports failure, not zero.
    CHECK(!r.smc.readNumber(Key("F0ID"), value));
}

TEST(device_counts_and_enumerates_keys) {
    Rig r;
    const uint32_t declared = r.smc.keyCount();
    CHECK(declared == r.model.size());
    CHECK(declared > 20);

    const std::vector<Key> keys = r.smc.allKeys();
    CHECK(keys.size() == declared);
    bool found = false;
    for (const Key& k : keys) {
        if (k == Key("FNum")) found = true;
    }
    CHECK(found);
}

TEST(device_reports_writability_from_the_hardware) {
    Rig r;
    CHECK(r.smc.isWritable(makeFanKey(0, "Tg")));
    CHECK(r.smc.isWritable(Key("FS! ")));
    CHECK(!r.smc.isWritable(makeFanKey(0, "Mx")));
    CHECK(!r.smc.isWritable(Key("TC0P")));
    CHECK(!r.smc.isWritable(Key("ZZZZ")));
}

TEST(device_writes_a_number_using_the_declared_type) {
    Rig r;
    CHECK(r.smc.writeNumber(makeFanKey(0, "Tg"), 4321.0));

    double readback = 0;
    CHECK(r.smc.readNumber(makeFanKey(0, "Tg"), readback));
    // fpe2 stores quarter-RPM steps, so an integer request lands exactly.
    CHECK_NEAR(readback, 4321.0, 0.25);
}

TEST(device_verifies_a_write_by_reading_it_back) {
    Rig r;
    CHECK(r.smc.writeNumberVerified(makeFanKey(0, "Tg"), 3600.0, 1.0));

    // The model really did change.
    CHECK_NEAR(r.model.valueOf("F0Tg").value(), 3600.0, 0.25);
}

TEST(device_detects_a_write_that_silently_does_not_take) {
    Rig r;
    // The controller accepts the transaction but ignores the payload. The port
    // path reports the write as successful, so only the readback reveals the
    // truth. This is the reason every fan write is verified.
    r.model.failNextWrites = 1;
    CHECK(!r.smc.writeNumberVerified(makeFanKey(0, "Tg"), 4444.0, 1.0));
    CHECK_NEAR(r.model.valueOf("F0Tg").value(), 3553.0, 0.25);  // unchanged

    // The same call succeeds once the controller behaves.
    CHECK(r.smc.writeNumberVerified(makeFanKey(0, "Tg"), 4444.0, 1.0));
    CHECK_NEAR(r.model.valueOf("F0Tg").value(), 4444.0, 0.25);
}

TEST(device_refuses_to_write_a_read_only_key) {
    Rig r;
    CHECK(!r.smc.writeNumber(Key("TC0P"), 30.0));
    CHECK(!r.smc.writeNumber(makeFanKey(0, "Mx"), 1000.0));
}

TEST(device_refuses_to_write_an_unknown_key) {
    Rig r;
    CHECK(!r.smc.writeNumber(Key("ZZZZ"), 1.0));
}

TEST(device_refuses_to_write_a_type_it_cannot_encode) {
    Rig r;
    // "REV " is a string type; there is no encoding of a number into it.
    CHECK(!r.smc.writeNumber(Key("REV "), 1.0));
}

TEST(device_labels_fans_from_hardware_and_falls_back) {
    Rig r;
    CHECK(r.smc.fanLabel(0) == "Left side");
    CHECK(r.smc.fanLabel(1) == "Right side");
    // A fan index the machine does not have still gets a usable label.
    CHECK(r.smc.fanLabel(7) == "Fan 7");
}

TEST(device_uses_cached_metadata_and_can_clear_it) {
    Rig r;
    KeyInfo first;
    CHECK(r.smc.keyInfo(Key("TC0P"), first));
    r.smc.clearCache();
    KeyInfo second;
    CHECK(r.smc.keyInfo(Key("TC0P"), second));
    CHECK(first.dataSize == second.dataSize);
    CHECK(first.dataType == second.dataType);
}

TEST(device_is_inert_when_the_transport_is_not_open) {
    Rig r;
    SimulatedKernelDevice device(r.model);
    device.setOpen(false);
    ApplesmcTransport closed(device);
    SmcDevice smc(closed);

    CHECK(!smc.isOpen());
    Value v;
    CHECK(!smc.read(Key("TC0P"), v));

    double number = 0;
    CHECK(!smc.readNumber(Key("TC0P"), number));
    CHECK(!smc.hasKey(Key("TC0P")));
    CHECK(smc.keyCount() == 0);
    CHECK(smc.allKeys().empty());
    CHECK(!smc.writeNumber(makeFanKey(0, "Tg"), 3000.0));
}
