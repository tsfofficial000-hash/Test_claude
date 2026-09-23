#include <cstring>
#include <utility>

#include "core/controller.h"
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
    Controller controller;

    explicit Rig(SmcModel m)
        : model(std::move(m)), io(model), transport(io, 64), smc(transport), controller(smc, bank) {}

    bool start(const Config& config) {
        if (!bank.discover(smc)) return false;
        return controller.initialise(config);
    }

    // Runs the loop `times` times and returns the last result.
    Controller::TickResult run(int times, double seconds = 1.0) {
        Controller::TickResult result;
        for (int i = 0; i < times; ++i) result = controller.tick(seconds);
        return result;
    }
};

Rig twoFans() { return Rig(SmcModel::macBookProTwoFans()); }

Config curveConfig(const char* sensorKey = "") {
    Config config;
    FanPolicy policy;
    policy.mode = FanControlMode::Curve;
    policy.curve = FanCurve::sensible(2160.0, 5927.0);
    if (sensorKey && *sensorKey) policy.curveSensorKey = sensorKey;
    config.setPolicy(0, policy);
    return config;
}

SmcModel fanlessMachine() {
    SmcModel m;
    m.addSp78("TC0P", 45.0);
    m.addSp78("TG0P", 40.0);
    m.addU32("#KEY", 0);
    m.refreshKeyCount();
    return m;
}

// A fan that reports a speed and a target but no limits at all.
SmcModel machineWithoutFanLimits() {
    SmcModel m;
    m.setFanCount(1);
    m.addString("F0ID", "Only fan", 16);
    m.addFpe2("F0Ac", 2000.0);
    m.addFpe2("F0Tg", 2000.0);
    m.addSp78("TC0D", 60.0);
    uint8_t mask[2];
    bigEndianWrite(mask, 2, 0);
    m.add(Key("FS! "), Key("ui16"), kAttrReadable | kAttrWritable, mask, 2);
    m.addU32("#KEY", 0);
    m.refreshKeyCount();
    return m;
}

}  // namespace

TEST(defaults_leave_every_fan_on_system_control) {
    Rig r = twoFans();
    CHECK(r.start(Config{}));

    const Controller::TickResult result = r.controller.tick(1.0);
    CHECK(result.hasTemperatures);
    CHECK_NEAR(result.hottestCelsius, 52.25, 0.001);  // TC0D is the hottest here
    CHECK(result.commandedRpm.size() == 2);
    CHECK(result.commandedRpm[0] == 0.0);
    CHECK(result.commandedRpm[1] == 0.0);
    CHECK(result.writes == 0);
    CHECK(r.model.u32Of("FS! ") == 0u);
}

TEST(initialise_hands_back_a_fan_a_previous_run_left_forced) {
    Rig r = twoFans();
    uint8_t mask[2] = {0x00, 0x01};
    CHECK(r.model.write(Key("FS! "), mask, 2));

    CHECK(r.start(Config{}));
    CHECK(r.model.u32Of("FS! ") == 0u);
    CHECK(!r.bank.at(0).manual);
}

TEST(curve_mode_takes_control_and_moves_the_fan) {
    Rig r = twoFans();
    CHECK(r.start(curveConfig()));

    r.model.addSp78("TC0D", 85.0);
    const Controller::TickResult result = r.controller.tick(1.0);

    CHECK(result.writes == 1);
    CHECK(r.model.u32Of("FS! ") == 1u);  // only fan 0 was taken over

    // The curve asks for ~5268 RPM at 85 C. The fan was at 3552 and the limiter
    // allows 900 RPM in the first second, so it lands at 4452.
    CHECK_NEAR(result.commandedRpm[0], 4452.0, 3.0);
    CHECK_NEAR(r.model.valueOf("F0Tg").value(), result.commandedRpm[0], 0.25);

    // Fan 1 has no policy, so it is untouched.
    CHECK(result.commandedRpm[1] == 0.0);
    CHECK_NEAR(r.model.valueOf("F1Tg").value(), 3290.0, 0.25);
}

TEST(curve_source_can_be_a_specific_sensor) {
    Rig r = twoFans();
    CHECK(r.start(curveConfig("TC0P")));

    // The machine is hot, but the chosen source is not.
    r.model.addSp78("TC0D", 85.0);
    r.model.addSp78("TC0P", 45.0);

    const Controller::TickResult result = r.controller.tick(1.0);
    // 45 C maps to the fan's minimum, and the fall is rate limited to 120 RPM.
    CHECK_NEAR(result.commandedRpm[0], 3432.0, 3.0);
    CHECK(result.commandedRpm[0] < 3552.0);
}

TEST(fan_catches_up_over_successive_ticks_then_stops_writing) {
    Rig r = twoFans();
    CHECK(r.start(curveConfig()));
    r.model.addSp78("TC0D", 85.0);

    const Controller::TickResult first = r.controller.tick(1.0);
    CHECK_NEAR(first.commandedRpm[0], 4452.0, 3.0);
    CHECK(first.writes == 1);

    // The second tick closes the remaining gap because it fits inside one
    // second's allowance.
    const Controller::TickResult second = r.controller.tick(1.0);
    CHECK_NEAR(second.commandedRpm[0], 5267.8, 3.0);
    CHECK(second.writes == 1);

    // Settled: the same temperature asks for the same speed, so the hardware is
    // left alone.
    const Controller::TickResult third = r.controller.tick(1.0);
    CHECK(third.writes == 0);
    CHECK_NEAR(third.commandedRpm[0], second.commandedRpm[0], 0.001);
}

TEST(a_sensor_that_stops_reporting_returns_the_fan_to_system) {
    Rig r = twoFans();
    Config config = curveConfig("TZZZ");  // no such sensor
    CHECK(r.start(config));

    const Controller::TickResult result = r.controller.tick(1.0);
    CHECK(result.commandedRpm[0] == 0.0);
    CHECK(!result.notes.empty());
    CHECK(result.notes[0].find("TZZZ") != std::string::npos);
    CHECK(r.model.u32Of("FS! ") == 0u);
}

TEST(an_unusable_curve_returns_the_fan_to_system) {
    Rig r = twoFans();
    Config config;
    FanPolicy policy;
    policy.mode = FanControlMode::Curve;
    policy.curve = FanCurve{};  // no points at all
    config.setPolicy(0, policy);
    CHECK(r.start(config));

    const Controller::TickResult result = r.controller.tick(1.0);
    CHECK(result.commandedRpm[0] == 0.0);
    CHECK(!result.notes.empty());
    CHECK(r.model.u32Of("FS! ") == 0u);
}

TEST(manual_mode_sets_the_requested_speed_and_clamps_it) {
    Rig r = twoFans();
    Config config;
    FanPolicy policy;
    policy.mode = FanControlMode::Manual;
    policy.manualRpm = 2000.0;  // below this fan's 2160 minimum
    config.setPolicy(0, policy);
    CHECK(r.start(config));

    const Controller::TickResult clamped = r.controller.tick(1.0);
    CHECK_NEAR(clamped.commandedRpm[0], 2160.0, 0.001);
    CHECK_NEAR(r.model.valueOf("F0Tg").value(), 2160.0, 0.25);

    // Raising the request moves the fan; a manual speed is applied at once.
    policy.manualRpm = 4000.0;
    config.setPolicy(0, policy);
    r.controller.setConfig(config);

    const Controller::TickResult raised = r.controller.tick(1.0);
    CHECK_NEAR(raised.commandedRpm[0], 4000.0, 0.25);
    CHECK(raised.writes == 1);
}

TEST(a_fan_without_usable_limits_is_never_driven) {
    Rig r{machineWithoutFanLimits()};
    CHECK(r.start(curveConfig()));
    CHECK(!r.bank.at(0).controllable());

    const Controller::TickResult result = r.controller.tick(1.0);
    CHECK(result.commandedRpm[0] == 0.0);
    CHECK(!result.notes.empty());
    CHECK(r.model.u32Of("FS! ") == 0u);
}

TEST(a_fanless_machine_still_reads_temperatures) {
    Rig r{fanlessMachine()};
    CHECK(r.start(Config{}));
    CHECK(r.bank.count() == 0);

    const Controller::TickResult result = r.controller.tick(1.0);
    CHECK(result.hasTemperatures);
    CHECK(result.commandedRpm.empty());
    CHECK(result.writes == 0);
    CHECK_NEAR(result.hottestCelsius, 45.0, 0.001);
}

TEST(restore_all_returns_every_fan_to_the_firmware) {
    Rig r = twoFans();
    Config config = curveConfig();
    FanPolicy second;
    second.mode = FanControlMode::Manual;
    second.manualRpm = 3000.0;
    config.setPolicy(1, second);
    CHECK(r.start(config));

    r.controller.tick(1.0);
    CHECK(r.model.u32Of("FS! ") == 3u);

    CHECK(r.controller.restoreAllToSystem());
    CHECK(r.model.u32Of("FS! ") == 0u);
    CHECK(!r.bank.at(0).manual);
    CHECK(!r.bank.at(1).manual);
}

TEST(manual_control_dropped_by_the_firmware_is_re_asserted) {
    // The SMC drops manual fan control across a suspend. The loop has to notice
    // and take the fan back.
    Rig r = twoFans();
    CHECK(r.start(curveConfig()));
    r.model.addSp78("TC0D", 85.0);
    r.controller.tick(1.0);
    CHECK(r.model.u32Of("FS! ") == 1u);

    uint8_t cleared[2] = {0x00, 0x00};
    CHECK(r.model.write(Key("FS! "), cleared, 2));

    const Controller::TickResult result = r.controller.tick(1.0);
    CHECK(result.writes == 1);
    CHECK(r.model.u32Of("FS! ") == 1u);
}

TEST(invalidating_the_command_cache_re_asserts_the_policy) {
    Rig r = twoFans();
    CHECK(r.start(curveConfig()));
    r.model.addSp78("TC0D", 85.0);
    r.run(5);
    CHECK(r.controller.tick(1.0).writes == 0);  // settled

    r.controller.invalidateCommandCache();
    CHECK(r.controller.tick(1.0).writes == 1);
}

TEST(heat_drives_the_fan_to_the_top_of_its_range) {
    Rig r = twoFans();
    CHECK(r.start(curveConfig()));

    r.model.addSp78("TC0D", 96.0);
    const Controller::TickResult warm = r.controller.tick(1.0);
    CHECK(warm.commandedRpm[0] > 4000.0);

    // Sustained heat reaches the fan's maximum and is clamped there, never
    // beyond.
    const Controller::TickResult hot = r.run(20);
    CHECK_NEAR(hot.commandedRpm[0], 5927.0, 1.0);
    CHECK_NEAR(r.model.valueOf("F0Tg").value(), 5927.0, 0.25);
    CHECK(r.model.valueOf("F0Tg").value() <= 5927.0);
}

TEST(cooling_brings_the_fan_down_but_more_slowly_than_it_went_up) {
    Rig r = twoFans();
    CHECK(r.start(curveConfig()));
    r.model.addSp78("TC0D", 96.0);
    r.run(20);
    const double hot = r.controller.tick(1.0).commandedRpm[0];
    CHECK_NEAR(hot, 5927.0, 1.0);

    // The machine cools right down. Every hot sensor has to drop, because the
    // curve follows the hottest component by default.
    for (const char* key : {"TC0P", "TC0D", "TC0E", "TC0F", "TC0H", "TCXC", "TG0P", "TG0D", "TG0H"}) {
        r.model.addSp78(key, 35.0);
    }
    const Controller::TickResult cooling = r.controller.tick(1.0);
    // One second of a 120 RPM per second fall: heat comes down on the fan's
    // schedule, not the temperature's.
    CHECK_NEAR(cooling.commandedRpm[0], 5807.0, 1.0);

    // It keeps coming down until it reaches the curve's value for 35 C, which
    // is below the first point, so the fan's own minimum. It rests within the
    // deadband of it, which is the deliberate price of not hunting: the fan
    // stops correcting once it is close enough.
    const Controller::TickResult settled = r.run(40);
    CHECK_NEAR(settled.commandedRpm[0], 2160.0, 45.0);
    CHECK(settled.commandedRpm[0] < hot + 1.0);
    CHECK(settled.commandedRpm[0] > 2100.0);
}

TEST(temperatures_are_enumerated_named_and_filtered) {
    Rig r = twoFans();
    CHECK(r.start(Config{}));

    const std::vector<SensorReading> readings = r.controller.readTemperatures();
    CHECK(readings.size() >= 10);
    CHECK(readings.front().key == "TC0D");
    CHECK(readings.front().name == "CPU die");
    CHECK_NEAR(readings.front().celsius, 52.25, 0.001);

    // Hottest first, all the way down.
    for (size_t i = 1; i < readings.size(); ++i) {
        CHECK(readings[i - 1].celsius >= readings[i].celsius);
    }

    // The dead sensor is left out entirely.
    for (const SensorReading& reading : readings) {
        CHECK(reading.key != "TS2P");
        CHECK(plausibleTemperature(reading.celsius));
    }

    // Every sensor the controller lists is one it can name.
    CHECK(r.controller.sensorKeys().size() == readings.size());
}

TEST(the_controller_survives_a_transport_that_goes_silent) {
    Rig r = twoFans();
    CHECK(r.start(curveConfig()));

    // The SMC stops answering mid-flight.
    r.model.stopAcceptingInput = true;
    const Controller::TickResult result = r.controller.tick(1.0);
    // It reports no temperatures rather than inventing any, and drives nothing.
    CHECK(!result.hasTemperatures);
    CHECK(result.commandedRpm[0] == 0.0);
}

// A machine running hotter than the emergency limit, from the first tick.
Rig overheatingMachine(double celsius) {
    SmcModel m = SmcModel::macBookProTwoFans();
    m.addSp78("TC0P", celsius);
    return Rig(std::move(m));
}

TEST(emergency_cooling_takes_every_fan_to_maximum) {
    Rig r = overheatingMachine(99.0);   // the default limit is 95 C
    CHECK(r.start(curveConfig("TC0P")));

    const Controller::TickResult result = r.controller.tick(1.0);

    CHECK(result.emergency);
    CHECK(result.hasTemperatures);
    // Fan 1 was left on "system" policy and is overridden anyway: at this
    // temperature the firmware's own loop is not keeping up, which is the
    // entire point of the feature.
    CHECK_NEAR(result.commandedRpm[0], r.bank.at(0).maxRpm, 1.0);
    CHECK_NEAR(result.commandedRpm[1], r.bank.at(1).maxRpm, 1.0);
}

TEST(emergency_cooling_stays_out_of_the_way_below_the_limit) {
    Rig r = overheatingMachine(60.0);
    CHECK(r.start(curveConfig("TC0P")));

    const Controller::TickResult result = r.controller.tick(1.0);

    CHECK(!result.emergency);
    CHECK(result.commandedRpm[0] < r.bank.at(0).maxRpm);
    // Fan 1 is on system control and is left there.
    CHECK(result.commandedRpm[1] == 0.0);
}

TEST(emergency_cooling_can_be_switched_off) {
    Rig r = overheatingMachine(99.0);
    Config config = curveConfig("TC0P");
    config.emergencyCelsius = 0.0;      // disabled
    CHECK(r.start(config));

    const Controller::TickResult result = r.controller.tick(1.0);

    CHECK(!result.emergency);
    CHECK(result.commandedRpm[1] == 0.0);
}

// One fan with a known range and one that reports a speed but no limits at all.
// The unmeasurable fan must be refused even during an emergency: driving it at
// a guessed speed is exactly the failure the range check exists to prevent.
SmcModel machineWithOneUnmeasurableFan() {
    SmcModel m;
    m.setFanCount(2);
    m.addFan(0, "Known", 2160, 5927, 3552, 3553);
    m.addString("F1ID", "Unknown", 16);
    m.addFpe2("F1Ac", 3000.0);
    m.addFpe2("F1Tg", 3000.0);
    uint8_t mask[2];
    bigEndianWrite(mask, 2, 0);
    m.add(Key("FS! "), Key("ui16"), kAttrReadable | kAttrWritable, mask, 2);
    m.addSp78("TC0P", 99.0);
    m.addU32("#KEY", 0);
    m.refreshKeyCount();
    return m;
}

TEST(emergency_cooling_does_not_drive_a_fan_whose_range_is_unknown) {
    Rig r(machineWithOneUnmeasurableFan());
    CHECK(r.start(curveConfig("TC0P")));
    CHECK(r.bank.at(0).controllable());
    CHECK(!r.bank.at(1).controllable());

    const Controller::TickResult result = r.controller.tick(1.0);

    CHECK(result.emergency);
    CHECK_NEAR(result.commandedRpm[0], r.bank.at(0).maxRpm, 1.0);
    CHECK(result.commandedRpm[1] == 0.0);
}

TEST(the_curve_resumes_after_the_emergency_passes) {
    Rig r = overheatingMachine(99.0);
    CHECK(r.start(curveConfig("TC0P")));
    CHECK(r.controller.tick(1.0).emergency);

    // The heat passes.
    r.model.addSp78("TC0P", 40.0);
    const Controller::TickResult cooled = r.controller.tick(1.0);

    CHECK(!cooled.emergency);
    // Back under the curve's control. It does not fall to the curve's value in
    // one step - the rate limiter is what stops a fan surging - but it is
    // demonstrably no longer pinned at maximum.
    CHECK(cooled.commandedRpm[0] < r.bank.at(0).maxRpm);
    CHECK(cooled.commandedRpm[0] > 0.0);
}
