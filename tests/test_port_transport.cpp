#include <cstring>
#include <string>

#include "smc/port_io_transport.h"
#include "smc/smc_sim.h"
#include "test_framework.h"

using namespace fanforge;

namespace {

// A small budget keeps the deliberate-failure cases fast. The default budget
// exists for real hardware, where the loop yields the CPU between polls.
constexpr int kFastPollBudget = 64;

struct Fixture {
    SmcModel model = SmcModel::macBookProTwoFans();
    SimulatedSmcPortIo io{model};
    PortIoTransport transport{io, kFastPollBudget};
};

}  // namespace

TEST(port_reads_a_temperature_through_the_full_handshake) {
    Fixture f;
    Value v;
    CHECK(f.transport.readKey(Key("TC0P"), 2, v));
    CHECK(v.size == 2);
    CHECK_NEAR(decodeValue(Key("sp78"), v.data, v.size).value(), 45.5, 0.001);

    // The command really did travel as a READ command byte.
    CHECK(f.io.commandsSeen >= 1);
}

TEST(port_reads_the_fan_count) {
    Fixture f;
    Value v;
    CHECK(f.transport.readKey(Key("FNum"), 1, v));
    CHECK(v.size == 1);
    CHECK(v.toU32() == 2);
}

TEST(port_reads_fan_speeds_and_limits) {
    Fixture f;
    Value v;
    CHECK(f.transport.readKey(makeFanKey(0, "Ac"), 2, v));
    CHECK_NEAR(decodeValue(Key("fpe2"), v.data, v.size).value(), 3552.0, 0.001);

    CHECK(f.transport.readKey(makeFanKey(1, "Mx"), 2, v));
    CHECK_NEAR(decodeValue(Key("fpe2"), v.data, v.size).value(), 5489.0, 0.001);
}

TEST(port_flushes_extra_bytes_when_fewer_are_requested) {
    // Fan IDs hold 16 bytes. Asking for two must still leave the controller
    // idle, because the hardware returns the whole key and the driver has to
    // drain the remainder.
    Fixture f;
    Value v;
    CHECK(f.transport.readKey(Key("F0ID"), 2, v));
    CHECK(v.size == 2);
    CHECK(v.data[0] == 'L');
    CHECK(v.data[1] == 'e');

    // Proof the controller was released: the next transaction works.
    Value after;
    CHECK(f.transport.readKey(Key("FNum"), 1, after));
    CHECK(after.toU32() == 2);
}

TEST(port_reports_key_metadata) {
    Fixture f;
    KeyInfo info;
    CHECK(f.transport.keyInfo(makeFanKey(0, "Tg"), info));
    CHECK(info.dataSize == 2);
    CHECK(Key(info.dataType) == Key("fpe2"));
    CHECK(info.writable());
    CHECK(info.readable());
    CHECK(!info.function());

    CHECK(f.transport.keyInfo(Key("#KEY"), info));
    CHECK(info.dataSize == 4);
    CHECK(Key(info.dataType) == Key("ui32"));
    CHECK(!info.writable());
}

TEST(port_enumerates_every_key_via_its_index) {
    Fixture f;
    Value count;
    CHECK(f.transport.readKey(Key("#KEY"), 4, count));
    const uint32_t declared = count.toU32();
    CHECK(declared == f.model.size());

    uint32_t walked = 0;
    bool sawFanCount = false;
    bool sawCpuTemp = false;
    for (uint32_t i = 0; i < declared; ++i) {
        Key k;
        CHECK(f.transport.keyByIndex(i, k));
        ++walked;
        if (k == Key("FNum")) sawFanCount = true;
        if (k == Key("TC0P")) sawCpuTemp = true;
    }
    CHECK(walked == declared);
    CHECK(sawFanCount);
    CHECK(sawCpuTemp);
}

TEST(port_index_past_the_end_fails_instead_of_inventing_a_key) {
    Fixture f;
    Key k;
    CHECK(!f.transport.keyByIndex(100000, k));
}

TEST(port_writes_a_fan_target_and_reads_it_back) {
    Fixture f;
    uint8_t payload[2];
    CHECK(encodeValue(Key("fpe2"), 4200.0, payload, sizeof payload) == 2);

    CHECK(f.transport.writeKey(makeFanKey(0, "Tg"), payload, 2));
    CHECK(f.model.writeCount == 1);

    Value v;
    CHECK(f.transport.readKey(makeFanKey(0, "Tg"), 2, v));
    CHECK_NEAR(decodeValue(Key("fpe2"), v.data, v.size).value(), 4200.0, 0.001);
}

TEST(port_writes_the_fan_force_mask) {
    Fixture f;
    const uint8_t mask[] = {0x00, 0x01};  // force fan 0 into manual mode
    CHECK(f.transport.writeKey(Key("FS! "), mask, 2));
    CHECK(f.model.u32Of("FS! ") == 1u);
}

TEST(port_write_to_a_read_only_key_reports_success_but_changes_nothing) {
    // The port protocol has no acknowledgement: a completed transaction is all
    // the transport can honestly report. The controller is rejected by the
    // hardware, and the caller only finds out by reading the key back. This is
    // why every fan write in the controller is verified by readback.
    Fixture f;
    const uint8_t payload[] = {0x37, 0x80};
    CHECK(f.transport.writeKey(makeFanKey(0, "Mx"), payload, 2));
    CHECK_NEAR(f.model.valueOf("F0Mx").value(), 5927.0, 0.001);  // unchanged
}

TEST(port_reading_an_unknown_key_fails_without_hanging) {
    Fixture f;
    Value v;
    CHECK(!f.transport.readKey(Key("ZZZZ"), 2, v));
    // And the transport is still usable afterwards.
    Value ok;
    CHECK(f.transport.readKey(Key("FNum"), 1, ok));
}

TEST(port_metadata_for_an_unknown_key_fails) {
    Fixture f;
    KeyInfo info;
    CHECK(!f.transport.keyInfo(Key("ZZZZ"), info));
}

TEST(port_gives_up_quickly_when_the_controller_stops_accepting_input) {
    Fixture f;
    f.model.stopAcceptingInput = true;
    Value v;
    CHECK(!f.transport.readKey(Key("FNum"), 1, v));
}

TEST(port_surfaces_a_read_failure_from_the_controller) {
    Fixture f;
    f.model.failNextReads = 1;
    Value v;
    CHECK(!f.transport.readKey(Key("FNum"), 1, v));
    // The next attempt succeeds, so a transient error is not sticky.
    CHECK(f.transport.readKey(Key("FNum"), 1, v));
}

TEST(port_rejects_an_implausible_length) {
    Fixture f;
    Value v;
    CHECK(!f.transport.readKey(Key("FNum"), 0, v));
    CHECK(!f.transport.readKey(Key("FNum"), 33, v));
    const uint8_t payload[1] = {0};
    CHECK(!f.transport.writeKey(Key("FS! "), payload, 0));
    CHECK(!f.transport.writeKey(Key("FS! "), nullptr, 2));
}

TEST(port_poll_budget_is_bounded_work) {
    // A wedged controller costs a bounded number of polls, never an infinite
    // loop. This is what makes an absent SMC a clean failure.
    Fixture f;
    f.model.stopAcceptingInput = true;
    const int before = f.io.idlePolls;
    Value v;
    CHECK(!f.transport.readKey(Key("FNum"), 1, v));
    CHECK(f.io.idlePolls - before <= kFastPollBudget * 8);
}
