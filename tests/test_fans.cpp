#include <cstring>
#include <utility>

#include "core/fan.h"
#include "smc/port_io_transport.h"
#include "smc/smc_sim.h"
#include "test_framework.h"

using namespace fanforge;

namespace {

// A complete rig over an arbitrary simulated machine.
struct AnyRig {
    SmcModel model;
    SimulatedSmcPortIo io;
    PortIoTransport transport;
    SmcDevice smc;
    FanBank bank;

    explicit AnyRig(SmcModel m)
        : model(std::move(m)), io(model), transport(io, 64), smc(transport) {}
};

using Rig = AnyRig;

// The default two-fan MacBook Pro.
AnyRig twoFans() { return AnyRig(SmcModel::macBookProTwoFans()); }

// Fans but no speed limits exposed: control must be refused.
SmcModel modelWithoutLimits() {
    SmcModel m;
    m.setFanCount(1);
    m.addString("F0ID", "Only fan", 16);
    m.addFpe2("F0Ac", 2000.0);
    m.addFpe2("F0Tg", 2000.0);
    uint8_t mask[2];
    bigEndianWrite(mask, 2, 0);
    m.add(Key("FS! "), Key("ui16"), kAttrReadable | kAttrWritable, mask, 2);
    m.addU32("#KEY", 0);
    m.refreshKeyCount();
    return m;
}

// A fan with a full range but no way to take manual control.
SmcModel modelWithoutManualControl() {
    SmcModel m;
    m.setFanCount(1);
    m.addFan(0, "Passive", 1200, 5000, 1500, 1500);
    m.addU32("#KEY", 0);
    m.refreshKeyCount();
    return m;
}

// A newer machine that reports per-fan mode keys instead of a force mask.
SmcModel modelWithPerFanMode() {
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

// The fanless 12" MacBook.
SmcModel modelWithoutFans() {
    SmcModel m;
    m.addSp78("TC0P", 45.0);
    m.addU32("#KEY", 0);
    m.refreshKeyCount();
    return m;
}

}  // namespace

TEST(discovery_reads_everything_from_the_hardware) {
    Rig r = twoFans();
    CHECK(r.bank.discover(r.smc));
    CHECK(r.bank.count() == 2);
    CHECK(r.bank.manualMode() == ManualMode::ForceMask);

    const FanInfo& left = r.bank.at(0);
    CHECK(left.label == "Left side");
    CHECK(left.labelFromHardware);
    CHECK(left.hasLimits);
    CHECK(left.controllable());
    CHECK_NEAR(left.minRpm, 2160.0, 0.001);
    CHECK_NEAR(left.maxRpm, 5927.0, 0.001);
    CHECK_NEAR(left.actualRpm, 3552.0, 0.001);
    CHECK_NEAR(left.targetRpm, 3553.0, 0.001);
    CHECK_NEAR(left.safeRpm, 5927.0, 0.001);
    CHECK(!left.manual);

    const FanInfo& right = r.bank.at(1);
    CHECK(right.label == "Right side");
    CHECK_NEAR(right.minRpm, 2000.0, 0.001);
    CHECK_NEAR(right.maxRpm, 5489.0, 0.001);
}

TEST(discovery_handles_a_fanless_machine) {
    Rig r{modelWithoutFans()};
    // No fan count is not an error: temperatures still work.
    CHECK(r.bank.discover(r.smc));
    CHECK(r.bank.count() == 0);
    CHECK(r.bank.manualMode() == ManualMode::None);
    CHECK(!r.bank.notes().empty());
}

TEST(discovery_refuses_a_fan_with_no_usable_range) {
    Rig r{modelWithoutLimits()};
    CHECK(r.bank.discover(r.smc));
    CHECK(r.bank.count() == 1);
    CHECK(!r.bank.at(0).controllable());
    CHECK(!r.bank.at(0).hasLimits);
    CHECK(r.bank.notes().find("refused") != std::string::npos);
}

TEST(discovery_ignores_a_limit_pair_that_is_the_wrong_way_round) {
    SmcModel m = SmcModel::macBookProTwoFans();
    m.addFpe2("F0Mn", 6000.0);
    m.addFpe2("F0Mx", 5000.0);  // maximum below minimum
    Rig r{std::move(m)};
    CHECK(r.bank.discover(r.smc));
    CHECK(!r.bank.at(0).controllable());
    // The other fan is unaffected.
    CHECK(r.bank.at(1).controllable());
}

TEST(discovery_ignores_a_limit_that_is_not_a_real_fan_speed) {
    // Newer (T2-era) Macs declare fan keys as 32-bit floats, which can express
    // any value at all - so the sanity ceiling has real work to do on that path.
    SmcModel m = SmcModel::macBookProTwoFans();
    const float absurd = 50000.0f;
    uint8_t bytes[4];
    std::memcpy(bytes, &absurd, 4);
    m.add(Key("F0Mx"), Key("flt"), kAttrReadable, bytes, 4);
    Rig r{std::move(m)};
    CHECK(r.bank.discover(r.smc));
    CHECK(!r.bank.at(0).controllable());
    CHECK_NEAR(r.bank.at(0).maxRpm, 0.0, 0.001);  // cleared, never used
    CHECK(r.bank.at(1).controllable());
}

TEST(discovery_detects_the_per_fan_mode_style) {
    Rig r{modelWithPerFanMode()};
    CHECK(r.bank.discover(r.smc));
    CHECK(r.bank.count() == 2);
    CHECK(r.bank.manualMode() == ManualMode::PerFanMode);
    CHECK(!r.bank.at(0).manual);
}

TEST(clamping_keeps_requests_inside_the_fans_own_range) {
    Rig r = twoFans();
    CHECK(r.bank.discover(r.smc));

    CHECK_NEAR(r.bank.clamp(0, 3000.0), 3000.0, 0.001);
    CHECK_NEAR(r.bank.clamp(0, 100.0), 2160.0, 0.001);
    CHECK_NEAR(r.bank.clamp(0, 99999.0), 5927.0, 0.001);
    CHECK_NEAR(r.bank.clamp(1, 100.0), 2000.0, 0.001);

    // A non-finite request floors rather than peaks.
    const double nan = 0.0 / 0.0;
    CHECK_NEAR(r.bank.clamp(0, nan), 2160.0, 0.001);
}

TEST(setting_a_target_engages_manual_control) {
    Rig r = twoFans();
    CHECK(r.bank.discover(r.smc));
    CHECK(r.model.u32Of("FS! ") == 0u);
    CHECK(!r.bank.at(0).manual);

    CHECK(r.bank.setTarget(r.smc, 0, 3000.0));

    CHECK(r.model.u32Of("FS! ") == 1u);   // this fan's force bit is set
    CHECK((r.model.u32Of("FS! ") & 2u) == 0u);  // and only this fan's
    CHECK(r.bank.at(0).manual);
    CHECK(r.bank.forceMask() == 1u);
    CHECK_NEAR(r.model.valueOf("F0Tg").value(), 3000.0, 0.25);
    CHECK_NEAR(r.bank.at(0).targetRpm, 3000.0, 0.25);
}

TEST(setting_a_target_only_moves_that_fan) {
    Rig r = twoFans();
    CHECK(r.bank.discover(r.smc));
    CHECK(r.bank.setTarget(r.smc, 1, 4200.0));
    CHECK(r.model.u32Of("FS! ") == 2u);
    CHECK_NEAR(r.model.valueOf("F0Tg").value(), 3553.0, 0.25);
    CHECK_NEAR(r.model.valueOf("F1Tg").value(), 4200.0, 0.25);
}

TEST(setting_a_target_clamps_to_the_range) {
    Rig r = twoFans();
    CHECK(r.bank.discover(r.smc));

    CHECK(r.bank.setTarget(r.smc, 0, 50.0));
    CHECK_NEAR(r.model.valueOf("F0Tg").value(), 2160.0, 0.25);

    CHECK(r.bank.setTarget(r.smc, 0, 1e6));
    CHECK_NEAR(r.model.valueOf("F0Tg").value(), 5927.0, 0.25);
}

TEST(setting_a_target_refuses_an_uncontrollable_fan) {
    Rig r{modelWithoutLimits()};
    CHECK(r.bank.discover(r.smc));

    CHECK(!r.bank.setTarget(r.smc, 0, 3000.0));
    // Nothing was forced: the fan was never taken over.
    CHECK(r.model.u32Of("FS! ") == 0u);
    CHECK(!r.bank.at(0).manual);
}

TEST(setting_a_target_refuses_a_machine_with_no_manual_path) {
    Rig r{modelWithoutManualControl()};
    CHECK(r.bank.discover(r.smc));
    CHECK(r.bank.count() == 1);
    CHECK(r.bank.manualMode() == ManualMode::None);

    // Writing a target here would be a lie: the firmware would ignore it.
    CHECK(!r.bank.setTarget(r.smc, 0, 3000.0));
    CHECK_NEAR(r.model.valueOf("F0Tg").value(), 1500.0, 0.25);
}

TEST(per_fan_mode_uses_the_mode_key) {
    Rig r{modelWithPerFanMode()};
    CHECK(r.bank.discover(r.smc));

    CHECK(r.bank.setTarget(r.smc, 0, 3000.0));
    CHECK_NEAR(r.model.valueOf("F0Md").value(), 1.0, 0.001);
    CHECK_NEAR(r.model.valueOf("F0Tg").value(), 3000.0, 0.25);
    CHECK_NEAR(r.model.valueOf("F1Md").value(), 0.0, 0.001);  // untouched

    CHECK(r.bank.returnToSystem(r.smc, 0));
    CHECK_NEAR(r.model.valueOf("F0Md").value(), 0.0, 0.001);
}

TEST(returning_one_fan_leaves_the_other_alone) {
    Rig r = twoFans();
    CHECK(r.bank.discover(r.smc));
    CHECK(r.bank.setTarget(r.smc, 0, 3000.0));
    CHECK(r.bank.setTarget(r.smc, 1, 3000.0));
    CHECK(r.model.u32Of("FS! ") == 3u);

    CHECK(r.bank.returnToSystem(r.smc, 0));
    CHECK(r.model.u32Of("FS! ") == 2u);
    CHECK(!r.bank.at(0).manual);
    CHECK(r.bank.at(1).manual);
}

TEST(returning_every_fan_clears_the_force_mask) {
    Rig r = twoFans();
    CHECK(r.bank.discover(r.smc));
    CHECK(r.bank.setTarget(r.smc, 0, 3000.0));
    CHECK(r.bank.setTarget(r.smc, 1, 3000.0));
    CHECK(r.model.u32Of("FS! ") == 3u);

    CHECK(r.bank.returnAllToSystem(r.smc));
    CHECK(r.model.u32Of("FS! ") == 0u);
    CHECK(!r.bank.at(0).manual);
    CHECK(!r.bank.at(1).manual);
}

TEST(returning_a_fan_that_is_already_on_system_control_succeeds) {
    Rig r = twoFans();
    CHECK(r.bank.discover(r.smc));
    CHECK(r.bank.returnToSystem(r.smc, 0));
    CHECK(r.model.u32Of("FS! ") == 0u);
}

TEST(refresh_picks_up_a_change_made_behind_our_back) {
    Rig r = twoFans();
    CHECK(r.bank.discover(r.smc));
    CHECK(!r.bank.at(0).manual);

    // Something else takes the fan over and changes the speed. F0Ac is not
    // writable through the normal path - it is an input, not a control - so the
    // machine state itself is changed, the way the firmware would.
    uint8_t mask[2] = {0x00, 0x01};
    CHECK(r.model.write(Key("FS! "), mask, 2));
    r.model.addFpe2("F0Ac", 5000.0);

    CHECK(r.bank.refresh(r.smc));
    CHECK(r.bank.at(0).manual);
    CHECK_NEAR(r.bank.at(0).actualRpm, 5000.0, 0.25);
    CHECK(r.bank.forceMask() == 1u);
}

TEST(a_write_that_cannot_be_confirmed_releases_the_fan) {
    // The controller accepts the transaction but does not apply it. Leaving the
    // fan forced in a state we cannot explain is the failure mode to avoid.
    Rig r = twoFans();
    CHECK(r.bank.discover(r.smc));
    r.model.failNextWrites = 1;

    CHECK(!r.bank.setTarget(r.smc, 0, 3000.0));
    CHECK(!r.bank.at(0).manual);
    CHECK(r.model.u32Of("FS! ") == 0u);
}

TEST(repeated_changes_keep_the_force_bit_set) {
    Rig r = twoFans();
    CHECK(r.bank.discover(r.smc));
    CHECK(r.bank.setTarget(r.smc, 0, 3000.0));
    CHECK(r.bank.setTarget(r.smc, 0, 3200.0));
    CHECK(r.bank.setTarget(r.smc, 0, 3400.0));
    CHECK(r.model.u32Of("FS! ") == 1u);
    CHECK_NEAR(r.model.valueOf("F0Tg").value(), 3400.0, 0.25);
}
