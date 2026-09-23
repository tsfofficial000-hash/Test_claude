#include "core/curve.h"
#include "test_framework.h"

using namespace fanforge;

TEST(curve_interpolates_between_points) {
    FanCurve curve{{40.0, 2000.0}, {60.0, 4000.0}, {80.0, 6000.0}};

    CHECK_NEAR(curve.evaluate(40.0), 2000.0, 0.001);
    CHECK_NEAR(curve.evaluate(60.0), 4000.0, 0.001);
    CHECK_NEAR(curve.evaluate(80.0), 6000.0, 0.001);
    // Halfway between points is halfway between speeds.
    CHECK_NEAR(curve.evaluate(50.0), 3000.0, 0.001);
    CHECK_NEAR(curve.evaluate(70.0), 5000.0, 0.001);
    // A quarter of the way.
    CHECK_NEAR(curve.evaluate(45.0), 2500.0, 0.001);
}

TEST(curve_holds_at_the_ends_and_never_extrapolates) {
    FanCurve curve{{40.0, 2000.0}, {60.0, 4000.0}};

    // Below the first point and above the last, the curve is a clamp. This is
    // what stops an extrapolated curve from commanding a speed the fan does not
    // have.
    CHECK_NEAR(curve.evaluate(-50.0), 2000.0, 0.001);
    CHECK_NEAR(curve.evaluate(0.0), 2000.0, 0.001);
    CHECK_NEAR(curve.evaluate(59.9), 3990.0, 0.001);
    CHECK_NEAR(curve.evaluate(60.0), 4000.0, 0.001);
    CHECK_NEAR(curve.evaluate(200.0), 4000.0, 0.001);
    CHECK_NEAR(curve.evaluate(1e9), 4000.0, 0.001);
}

TEST(curve_ignores_a_non_finite_temperature) {
    FanCurve curve{{40.0, 2000.0}, {60.0, 4000.0}};
    const double nan = 0.0 / 0.0;
    // Returns the coolest end rather than propagating a NaN into a fan speed.
    CHECK_NEAR(curve.evaluate(nan), 2000.0, 0.001);
}

TEST(curve_of_nothing_evaluates_to_nothing) {
    FanCurve empty;
    CHECK_NEAR(empty.evaluate(50.0), 0.0, 0.001);
    CHECK(!empty.valid());
}

TEST(curve_normalise_sorts_and_collapses_duplicates) {
    FanCurve curve{{70.0, 5000.0}, {50.0, 3000.0}, {70.0, 5500.0}, {60.0, 4000.0}};
    curve.normalise();
    CHECK(curve.points.size() == 3);
    CHECK_NEAR(curve.points[0].temperature, 50.0, 0.001);
    CHECK_NEAR(curve.points[1].temperature, 60.0, 0.001);
    CHECK_NEAR(curve.points[2].temperature, 70.0, 0.001);
    // The later point wins at a duplicate temperature.
    CHECK_NEAR(curve.points[2].rpm, 5500.0, 0.001);
}

TEST(curve_validation_rejects_the_shapes_that_break_control) {
    FanCurve single{{50.0, 3000.0}};
    CHECK(!single.valid());
    CHECK(!single.invalidReason().empty());

    // Speed must not fall as temperature rises, or the fan would slow down when
    // the machine gets hotter.
    FanCurve inverted{{50.0, 4000.0}, {70.0, 3000.0}};
    CHECK(!inverted.valid());

    FanCurve flat{{50.0, 4000.0}, {70.0, 4000.0}};
    CHECK(flat.valid());  // a constant speed is dull but safe

    FanCurve good{{50.0, 3000.0}, {70.0, 4000.0}};
    CHECK(good.valid());
    CHECK(good.invalidReason().empty());
}

TEST(curve_normalise_drops_non_finite_points) {
    const double nan = 0.0 / 0.0;
    FanCurve curve{{50.0, 3000.0}, {nan, 4000.0}, {70.0, 5000.0}};
    curve.normalise();
    CHECK(curve.points.size() == 2);
    CHECK(curve.valid());
}

TEST(sensible_curve_spans_the_fans_own_range) {
    const FanCurve curve = FanCurve::sensible(2160.0, 5927.0);
    CHECK(curve.valid());
    CHECK_NEAR(curve.evaluate(0.0), 2160.0, 0.001);
    CHECK_NEAR(curve.evaluate(200.0), 5927.0, 0.001);
    // Monotonic all the way up.
    double previous = -1.0;
    for (double t = 0.0; t <= 110.0; t += 1.0) {
        const double rpm = curve.evaluate(t);
        CHECK(rpm >= previous);
        previous = rpm;
    }
}

TEST(sensible_curve_refuses_an_impossible_range) {
    // A fan whose maximum is not above its minimum cannot be given a curve; the
    // caller is expected to check valid() and fall back.
    CHECK(!FanCurve::sensible(5000.0, 1000.0).valid());
    CHECK(!FanCurve::sensible(3000.0, 3000.0).valid());
}

TEST(limiter_seeds_itself_without_ramping_from_zero) {
    CurveOutput limiter(900.0, 120.0, 40.0);
    // The first value is taken directly: a fan must not sweep up from 0 RPM
    // when the app starts.
    CHECK_NEAR(limiter.update(3000.0, 1.0), 3000.0, 0.001);
    CHECK(limiter.primed());
}

TEST(limiter_rises_fast_and_falls_slowly) {
    CurveOutput limiter(900.0, 120.0, 40.0);
    limiter.reset(2000.0);

    // Rising: 900 RPM per second.
    CHECK_NEAR(limiter.update(6000.0, 1.0), 2900.0, 0.001);
    CHECK_NEAR(limiter.update(6000.0, 1.0), 3800.0, 0.001);

    // Falling: 120 RPM per second. Heat is the emergency, quiet is not.
    limiter.reset(5000.0);
    CHECK_NEAR(limiter.update(2000.0, 1.0), 4880.0, 0.001);
    CHECK_NEAR(limiter.update(2000.0, 1.0), 4760.0, 0.001);
}

TEST(limiter_respects_the_elapsed_time) {
    CurveOutput limiter(900.0, 120.0, 40.0);
    limiter.reset(2000.0);

    // A tenth of a second buys a tenth of the movement.
    CHECK_NEAR(limiter.update(6000.0, 0.1), 2090.0, 0.001);
    // Zero time is no movement at all.
    CHECK_NEAR(limiter.update(6000.0, 0.0), 2090.0, 0.001);
    // A two-second gap buys two seconds of movement.
    CHECK_NEAR(limiter.update(6000.0, 2.0), 3890.0, 0.001);
}

TEST(limiter_deadband_stops_the_fan_hunting) {
    CurveOutput limiter(900.0, 120.0, 40.0);
    limiter.reset(3000.0);

    // A correction smaller than the deadband is ignored entirely.
    CHECK_NEAR(limiter.update(3030.0, 1.0), 3000.0, 0.001);
    CHECK_NEAR(limiter.update(2970.0, 1.0), 3000.0, 0.001);
    // Beyond it, movement resumes.
    CHECK_NEAR(limiter.update(3100.0, 1.0), 3100.0, 0.001);  // within one rise step
}

TEST(limiter_reaches_and_holds_its_target) {
    CurveOutput limiter(900.0, 120.0, 40.0);
    limiter.reset(2000.0);

    double value = 2000.0;
    for (int i = 0; i < 100; ++i) value = limiter.update(4200.0, 0.5);
    CHECK_NEAR(value, 4200.0, 0.001);

    // Once settled it stops moving.
    CHECK_NEAR(limiter.update(4200.0, 0.5), 4200.0, 0.001);
}

TEST(limiter_ignores_a_non_finite_target) {
    CurveOutput limiter(900.0, 120.0, 40.0);
    limiter.reset(3000.0);
    const double nan = 0.0 / 0.0;
    CHECK_NEAR(limiter.update(nan, 1.0), 3000.0, 0.001);
}

TEST(limiter_limits_can_be_changed_without_losing_state) {
    CurveOutput limiter(900.0, 120.0, 40.0);
    limiter.reset(2000.0);
    limiter.update(6000.0, 1.0);  // 2900

    limiter.setLimits(100.0, 100.0);
    // The current output is preserved; only future movement changes.
    CHECK_NEAR(limiter.value(), 2900.0, 0.001);
    CHECK_NEAR(limiter.update(6000.0, 1.0), 3000.0, 0.001);
}
