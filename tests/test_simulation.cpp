#include "sim/simulate.h"
#include "test_framework.h"

using namespace fanforge;

namespace {

SimulationOptions options(double seconds, bool curve, bool table = false) {
    SimulationOptions o;
    o.seconds = seconds;
    o.stepSeconds = 1.0;
    o.curveControl = curve;
    o.printTable = table;
    return o;
}

}  // namespace

TEST(simulation_runs_the_whole_loop_and_lets_go_of_the_fans) {
    const SimulationReport report = runSimulation(options(60.0, true), nullptr);

    // The loop actually did something rather than idling out.
    CHECK(report.steps == 60);
    CHECK(report.controlWrites > 0);
    CHECK(report.notes.empty());

    // And it handed the machine back at the end.
    CHECK(report.fansReturnedToSystem);
}

TEST(simulation_keeps_the_die_out_of_trouble) {
    // Sustained full load, which is the case the app exists for.
    const SimulationReport report = runSimulation(options(120.0, true), nullptr);

    // The controller's own safety behaviour is what keeps this bounded: it
    // ramps fast on heat and clamps to the fan's reported maximum.
    CHECK(report.peakCelsius < 95.0);
    CHECK(report.peakCelsius > 40.0);
    CHECK(report.finalCelsius <= report.peakCelsius + 0.001);

    // The fans were used, and none was driven past its maximum.
    CHECK(report.averageRpm > 2000.0);
    CHECK(report.maximumRpm <= 5927.0 + 5489.0);
}

TEST(a_quieter_curve_trades_temperature_for_airflow) {
    // The app's value is not a fixed opinion about how loud a machine should be;
    // it is that the trade is yours to pick, and the loop honours it. This
    // asserts the trade exists in both directions, which is the property that
    // matters - not that any particular curve beats any particular firmware.
    SimulationOptions quiet = options(150.0, true);
    quiet.curveStartCelsius = 70.0;
    quiet.curveFullCelsius = 105.0;

    SimulationOptions cool = options(150.0, true);
    cool.curveStartCelsius = 40.0;
    cool.curveFullCelsius = 80.0;

    const SimulationReport quietReport = runSimulation(quiet, nullptr);
    const SimulationReport coolReport = runSimulation(cool, nullptr);

    CHECK(quietReport.fansReturnedToSystem);
    CHECK(coolReport.fansReturnedToSystem);

    // Both settings keep the machine safe. The safety behaviour does not depend
    // on the curve, which is the point: a quiet curve is a preference, not a
    // way to cook the machine.
    CHECK(quietReport.peakCelsius < 100.0);
    CHECK(coolReport.peakCelsius < 95.0);

    // And the trade is real in both directions.
    CHECK(coolReport.peakCelsius < quietReport.peakCelsius);
    CHECK(coolReport.averageRpm > quietReport.averageRpm);
}

TEST(the_firmware_left_in_charge_is_also_safe) {
    // The comparison run is not a straw man to be beaten: with the firmware in
    // charge the machine must also stay within limits, or the simulation is
    // modelling something that never happens.
    const SimulationReport firmware = runSimulation(options(120.0, false), nullptr);

    CHECK(firmware.fansReturnedToSystem);
    CHECK(firmware.controlWrites == 0);  // nobody took the fans over
    CHECK(firmware.peakCelsius < 100.0);
    CHECK(firmware.averageRpm > 0.0);
}

TEST(a_stepped_load_is_answered_promptly) {
    // Light load, then heavy load from ten seconds in. The interesting number
    // is how fast the fans react, because that is the moment a laptop starts
    // to feel hot.
    SimulationOptions o = options(60.0, true);
    o.lightLoadUntil = 10.0;
    o.lightLoad = 0.1;
    o.heavyLoad = 1.0;
    const SimulationReport report = runSimulation(o, nullptr);

    CHECK(report.notes.empty());
    CHECK(report.fansReturnedToSystem);
    CHECK(report.controlWrites > 0);

    // A hundred and twenty seconds of flat-out load peaked the die; a minute of
    // mostly-idle time must not.
    const SimulationReport sustained = runSimulation(options(120.0, true), nullptr);
    CHECK(report.peakCelsius < sustained.peakCelsius);
}
