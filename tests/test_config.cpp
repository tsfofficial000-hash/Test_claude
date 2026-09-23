#include <cstdio>
#include <string>

#include "core/config.h"
#include "test_framework.h"

using namespace fanforge;

TEST(default_config_survives_a_round_trip) {
    const Config original;
    const Config restored = Config::fromText(original.toText());
    CHECK(restored == original);
    CHECK(restored.pollIntervalMs == 1000);
    CHECK(restored.restoreOnExit);
    CHECK(restored.fans.empty());
}

TEST(a_populated_config_survives_a_round_trip) {
    Config original;
    original.pollIntervalMs = 750;
    original.startMinimized = true;
    original.restoreOnExit = false;
    original.maximumRisePerSecond = 1200.0;
    original.maximumFallPerSecond = 250.0;
    original.deadbandRpm = 25.0;

    FanPolicy curve;
    curve.mode = FanControlMode::Curve;
    curve.curveSensorKey = "TC0D";
    curve.curve = FanCurve::sensible(2160.0, 5927.0);
    original.setPolicy(0, curve);

    FanPolicy manual;
    manual.mode = FanControlMode::Manual;
    manual.manualRpm = 4200.0;
    original.setPolicy(1, manual);

    const Config restored = Config::fromText(original.toText());
    CHECK(restored == original);
    CHECK(restored.fans.size() == 2);
    CHECK(restored.fans[0].mode == FanControlMode::Curve);
    CHECK(restored.fans[0].curveSensorKey == "TC0D");
    CHECK(restored.fans[0].curve.points.size() == 5);
    CHECK(restored.fans[1].mode == FanControlMode::Manual);
    CHECK_NEAR(restored.fans[1].manualRpm, 4200.0, 0.001);
}

TEST(a_hand_written_config_is_read_as_written) {
    const std::string text =
        "# a comment\n"
        "\n"
        "poll_interval_ms=500\n"
        "restore_on_exit=0\n"
        "start_minimized=yes\n"
        "max_rise_rpm_per_s=1000\n"
        "max_fall_rpm_per_s=200\n"
        "deadband_rpm=30\n"
        "fan.0.mode=curve\n"
        "fan.0.sensor=TG0D\n"
        "fan.0.manual_rpm=3500\n"
        "fan.0.curve=45:2160, 62:3000, 80:5000, 92:6200\n"
        "fan.1.mode=auto\n";

    const Config config = Config::fromText(text);
    CHECK(config.pollIntervalMs == 500);
    CHECK(!config.restoreOnExit);
    CHECK(config.startMinimized);
    CHECK_NEAR(config.maximumRisePerSecond, 1000.0, 0.001);
    CHECK_NEAR(config.maximumFallPerSecond, 200.0, 0.001);
    CHECK_NEAR(config.deadbandRpm, 30.0, 0.001);

    CHECK(config.fans.size() == 2);
    CHECK(config.fans[0].mode == FanControlMode::Curve);
    CHECK(config.fans[0].curveSensorKey == "TG0D");
    CHECK(config.fans[0].curve.points.size() == 4);
    // Spaces around a curve point are tolerated.
    CHECK_NEAR(config.fans[0].curve.points[1].temperature, 62.0, 0.001);
    CHECK_NEAR(config.fans[0].curve.points[1].rpm, 3000.0, 0.001);
    // "auto" is accepted as a synonym for system control.
    CHECK(config.fans[1].mode == FanControlMode::System);
}

TEST(unknown_lines_and_keys_are_ignored) {
    const std::string text =
        "something_we_did_not_invent=12\n"
        "garbage without an equals sign\n"
        "single_instance=false\n"  // a setting that no longer exists
        "fan.0.mode=system\n"
        "fan.0.nonsense=99\n"
        "poll_interval_ms=800\n";
    const Config config = Config::fromText(text);
    CHECK(config.pollIntervalMs == 800);
    CHECK(config.fans.size() == 1);
    CHECK(config.fans[0].mode == FanControlMode::System);
}

TEST(a_fan_field_that_means_nothing_does_not_create_a_policy) {
    // Only a recognised field sizes the fan list. A stray line naming fan 4 for
    // an unknown reason must not conjure up four fans.
    const Config config = Config::fromText("fan.4.nonsense=99\n");
    CHECK(config.fans.empty());
    CHECK(config.policyFor(4).mode == FanControlMode::System);
}

TEST(malformed_values_fall_back_to_the_defaults) {
    const std::string text =
        "poll_interval_ms=fast\n"
        "max_rise_rpm_per_s=-5\n"
        "deadband_rpm=not-a-number\n"
        "fan.0.mode=turbo\n"
        "fan.0.manual_rpm=nope\n"
        "fan.0.curve=45\n";  // no colon

    const Config config = Config::fromText(text);
    const Config defaults;
    CHECK(config.pollIntervalMs == defaults.pollIntervalMs);
    CHECK_NEAR(config.maximumRisePerSecond, defaults.maximumRisePerSecond, 0.001);
    CHECK_NEAR(config.deadbandRpm, defaults.deadbandRpm, 0.001);
    CHECK(config.fans[0].mode == FanControlMode::System);
    CHECK_NEAR(config.fans[0].manualRpm, 0.0, 0.001);
    CHECK(config.fans[0].curve.points.empty());
}

TEST(the_poll_interval_is_kept_in_a_sane_range) {
    // Polling the SMC many times a second buys nothing and hammers the
    // controller; the floor is applied on read.
    CHECK(Config::fromText("poll_interval_ms=1").pollIntervalMs == 200);
    CHECK(Config::fromText("poll_interval_ms=199").pollIntervalMs == 200);
    CHECK(Config::fromText("poll_interval_ms=200").pollIntervalMs == 200);
    CHECK(Config::fromText("poll_interval_ms=2000").pollIntervalMs == 2000);
    CHECK(Config::fromText("poll_interval_ms=0").pollIntervalMs == 1000);
}

TEST(curve_points_are_parsed_and_sorted) {
    const Config config = Config::fromText("fan.0.curve=80:5000,45:2160,62:3000");
    CHECK(config.fans[0].curve.points.size() == 3);
    CHECK_NEAR(config.fans[0].curve.points[0].temperature, 45.0, 0.001);
    CHECK_NEAR(config.fans[0].curve.points[1].temperature, 62.0, 0.001);
    CHECK_NEAR(config.fans[0].curve.points[2].temperature, 80.0, 0.001);
    CHECK(config.fans[0].curve.valid());
}

TEST(policy_lookup_defaults_to_system_control) {
    Config config;
    CHECK(config.policyFor(0).mode == FanControlMode::System);

    FanPolicy manual;
    manual.mode = FanControlMode::Manual;
    config.setPolicy(3, manual);

    // Setting fan 3 fills the gap, and the fans before it stay on system.
    CHECK(config.fans.size() == 4);
    CHECK(config.policyFor(0).mode == FanControlMode::System);
    CHECK(config.policyFor(2).mode == FanControlMode::System);
    CHECK(config.policyFor(3).mode == FanControlMode::Manual);
    // Past the end is also system control, not a crash.
    CHECK(config.policyFor(99).mode == FanControlMode::System);
}

TEST(config_writes_and_reads_a_file) {
    const std::string path = "fanforge-test-config.ini";
    std::remove(path.c_str());

    // A missing file yields defaults rather than an error.
    const Config missing = Config::read(path);
    CHECK(missing == Config{});

    Config original;
    original.pollIntervalMs = 1500;
    FanPolicy policy;
    policy.mode = FanControlMode::Curve;
    policy.curve = FanCurve::sensible(2000.0, 5000.0);
    original.setPolicy(1, policy);

    CHECK(original.write(path));

    const Config loaded = Config::read(path);
    CHECK(loaded == original);
    std::remove(path.c_str());
}

TEST(the_default_config_path_is_absolute_and_well_formed) {
    const std::string path = defaultConfigPath();
    CHECK(!path.empty());
    CHECK(path.find("config.ini") != std::string::npos);
#ifdef _WIN32
    CHECK(path.find('\\') != std::string::npos);
#else
    CHECK(path.find('/') != std::string::npos);
#endif
}

TEST(mode_names_and_parsing_agree) {
    FanControlMode mode;
    CHECK(parseMode("system", mode) && mode == FanControlMode::System);
    CHECK(parseMode("manual", mode) && mode == FanControlMode::Manual);
    CHECK(parseMode("curve", mode) && mode == FanControlMode::Curve);
    CHECK(!parseMode("nonsense", mode));

    for (FanControlMode m :
         {FanControlMode::System, FanControlMode::Manual, FanControlMode::Curve}) {
        FanControlMode parsed;
        CHECK(parseMode(modeName(m), parsed));
        CHECK(parsed == m);
    }
}
