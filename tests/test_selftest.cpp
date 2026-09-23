//
// The write-path selftest.
//
// The properties that matter are: it confirms a machine that works, it does NOT
// report a false failure on a machine that controls fans the other way, it
// notices a write the SMC silently drops, and it never leaves a fan in a state
// the user did not ask for.
//
#include <utility>

#include "core/selftest.h"
#include "smc/port_io_transport.h"
#include "smc/smc_sim.h"
#include "test_framework.h"

using namespace fanforge;

namespace {

struct Rig {
    SmcModel model;
    SimulatedSmcPortIo io;
    PortIoTransport transport;
    SmcDevice smc;
    FanBank bank;

    explicit Rig(SmcModel m)
        : model(std::move(m)), io(model), transport(io, 64), smc(transport) {
        bank.discover(smc);
    }
};

Rig twoFans() { return Rig(SmcModel::macBookProTwoFans()); }

// A newer machine that exposes per-fan mode keys instead of the shared mask.
SmcModel perFanModeMachine() {
    SmcModel m;
    m.setFanCount(2);
    m.addFan(0, "Left side ", 2160, 5927, 3552, 3553);
    m.addFan(1, "Right side", 2000, 5489, 3284, 3290);
    const uint8_t zero = 0;
    m.add(Key("F0Md"), Key("ui8"), kAttrReadable | kAttrWritable, &zero, 1);
    m.add(Key("F1Md"), Key("ui8"), kAttrReadable | kAttrWritable, &zero, 1);
    m.addU32("#KEY", 0);
    m.refreshKeyCount();
    return m;
}

// A fan with a full speed range and no way to take manual control.
SmcModel machineWithoutManualControl() {
    SmcModel m;
    m.setFanCount(1);
    m.addFan(0, "Passive", 1200, 5000, 1500, 1500);
    m.addU32("#KEY", 0);
    m.refreshKeyCount();
    return m;
}

SmcModel fanlessMachine() {
    SmcModel m;
    m.addSp78("TC0P", 45.0);
    m.addU32("#KEY", 0);
    m.refreshKeyCount();
    return m;
}

}  // namespace

TEST(selftest_confirms_a_healthy_machine) {
    Rig rig = twoFans();
    CHECK(rig.bank.count() == 2);

    const SelfTestReport report = runWritePathSelfTest(rig.smc, rig.bank);

    CHECK(report.applicable);
    CHECK(report.writePathConfirmed());
    CHECK(report.ok());
    CHECK(!report.summary().empty());

    // One "write path" step and one "restore" step per fan.
    CHECK(report.steps.size() == 4);
}

TEST(selftest_leaves_every_fan_on_system_control) {
    Rig rig = twoFans();
    runWritePathSelfTest(rig.smc, rig.bank);

    // The test engaged manual control and must have handed it all back.
    CHECK(rig.bank.forceMask() == 0);
    CHECK(!rig.bank.at(0).manual);
    CHECK(!rig.bank.at(1).manual);

    // And the hardware agrees, not just our cached view.
    rig.bank.refresh(rig.smc);
    CHECK(rig.model.u32Of("FS! ") == 0);
    CHECK(!rig.bank.at(0).manual);
    CHECK(!rig.bank.at(1).manual);
}

TEST(selftest_passes_on_a_machine_that_uses_per_fan_mode_keys) {
    // Regression: reading only the "FS! " mask reported a false failure here,
    // because this machine has no mask at all.
    Rig rig(perFanModeMachine());
    CHECK(rig.bank.manualMode() == ManualMode::PerFanMode);

    const SelfTestReport report = runWritePathSelfTest(rig.smc, rig.bank);

    CHECK(report.applicable);
    CHECK(report.writePathConfirmed());
    CHECK(report.ok());

    // Restored: both fans back under firmware control.
    rig.bank.refresh(rig.smc);
    CHECK(!rig.bank.at(0).manual);
    CHECK(!rig.bank.at(1).manual);
}

TEST(selftest_notices_a_write_the_smc_silently_drops) {
    Rig rig = twoFans();
    rig.model.failNextWrites = 1000;

    const SelfTestReport report = runWritePathSelfTest(rig.smc, rig.bank);

    CHECK(report.applicable);
    CHECK(!report.ok());
    CHECK(!report.writePathConfirmed());
    CHECK(report.summary().find("first failure") != std::string::npos);
}

TEST(selftest_is_not_applicable_without_a_manual_path) {
    Rig rig(machineWithoutManualControl());
    CHECK(rig.bank.manualMode() == ManualMode::None);

    const SelfTestReport report = runWritePathSelfTest(rig.smc, rig.bank);

    CHECK(!report.applicable);
    CHECK(!report.skippedReason.empty());
    CHECK(!report.ok());          // not applicable is not a pass
    CHECK(report.steps.empty());
}

TEST(selftest_is_not_applicable_on_a_fanless_machine) {
    Rig rig(fanlessMachine());
    CHECK(rig.bank.empty());

    const SelfTestReport report = runWritePathSelfTest(rig.smc, rig.bank);

    CHECK(!report.applicable);
    CHECK(!report.skippedReason.empty());
}

TEST(selftest_puts_a_fan_that_was_already_manual_back_as_it_found_it) {
    Rig rig = twoFans();

    // The user had fan 0 pinned at a specific speed before the test ran.
    CHECK(rig.bank.setTarget(rig.smc, 0, 3000.0));
    CHECK(rig.bank.at(0).manual);

    const SelfTestReport report = runWritePathSelfTest(rig.smc, rig.bank);
    CHECK(report.ok());

    rig.bank.refresh(rig.smc);
    CHECK(rig.bank.at(0).manual);                        // still manual
    CHECK_NEAR(rig.bank.at(0).targetRpm, 3000.0, 1.0);   // at the same speed
    CHECK(!rig.bank.at(1).manual);                       // the other one is free
}

TEST(selftest_asks_for_the_speed_the_fan_is_already_running) {
    Rig rig = twoFans();
    const double current = rig.bank.at(0).actualRpm;
    CHECK(current > 0);

    const SelfTestReport report = runWritePathSelfTest(rig.smc, rig.bank);

    // The first step's detail quotes the speed it asked for; it must be the
    // speed the fan was running, or the test would be audible.
    CHECK(!report.steps.empty());
    const std::string expected = std::to_string(static_cast<long long>(current));
    CHECK(report.steps[0].detail.find(expected) != std::string::npos);
}
