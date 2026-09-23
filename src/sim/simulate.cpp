#include "sim/simulate.h"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <ostream>
#include <string>
#include <vector>

#include "core/controller.h"
#include "smc/port_io_transport.h"
#include "smc/smc_sim.h"

namespace fanforge {
namespace {

// --- the plant -------------------------------------------------------------
// A single-zone lumped model: one die, two fans, and the only thing that
// matters is how much heat the airflow carries away.
constexpr double kAmbient = 25.0;
constexpr double kHeatCapacity = 25.0;   // watt-seconds per kelvin
constexpr double kIdleWatts = 10.0;      // what the machine makes doing nothing
constexpr double kLoadWatts = 45.0;      // on top of that at full load
constexpr double kFanRisePerSecond = 1500.0;  // a fan cannot teleport
constexpr double kFanFallPerSecond = 400.0;

// The firmware's own fan law, standing in for the thermal loop Boot Camp
// leaves running: quiet below 55 C, maximum by 90 C. It is not a model of any
// particular Apple firmware - it is a reasonable one, so the comparison is not
// against a straw man.
double firmwareTarget(double celsius, double minimum, double maximum) {
    if (maximum <= minimum) return minimum;
    const double low = 55.0;
    const double high = 90.0;
    if (celsius <= low) return minimum;
    if (celsius >= high) return maximum;
    return minimum + (celsius - low) / (high - low) * (maximum - minimum);
}

double loadAt(double seconds, const SimulationOptions& options) {
    return (seconds < options.lightLoadUntil) ? options.lightLoad : options.heavyLoad;
}

// The standard curve shape, stretched over a different temperature window.
// Only the temperatures move; the shape of the ramp, and the speeds it asks
// for, stay the same.
FanCurve curveForWindow(double minimum, double maximum, double start, double full) {
    FanCurve curve = FanCurve::sensible(minimum, maximum);
    if (curve.points.size() < 2) return curve;

    const double baseStart = curve.points.front().temperature;
    const double baseFull = curve.points.back().temperature;
    if (!(full > start) || !(baseFull > baseStart)) return curve;

    for (CurvePoint& point : curve.points) {
        const double where = (point.temperature - baseStart) / (baseFull - baseStart);
        point.temperature = start + where * (full - start);
    }
    return curve;
}

}  // namespace

SimulationReport runSimulation(const SimulationOptions& options, std::ostream* log) {
    SimulationReport report;

    SmcModel model = SmcModel::macBookProTwoFans();
    SimulatedSmcPortIo io(model);
    PortIoTransport transport(io);
    SmcDevice smc(transport);
    FanBank bank;
    Controller controller(smc, bank);

    if (!bank.discover(smc)) {
        report.notes.push_back("the simulated machine could not be discovered");
        return report;
    }
    const size_t fanCount = bank.count();
    if (fanCount == 0) {
        report.notes.push_back("the simulated machine has no fans");
        return report;
    }

    Config config;
    config.pollIntervalMs = static_cast<int>(options.stepSeconds * 1000.0);
    if (options.curveControl) {
        for (size_t i = 0; i < fanCount; ++i) {
            FanPolicy policy;
            policy.mode = FanControlMode::Curve;
            policy.curve = curveForWindow(bank.at(i).minRpm, bank.at(i).maxRpm,
                                         options.curveStartCelsius, options.curveFullCelsius);
            config.setPolicy(i, policy);
        }
    }
    if (!controller.initialise(config)) {
        report.notes.push_back("a fan could not be returned to system control at startup");
    }

    double die = 45.0;
    double rpmSum = 0.0;
    double rpmSamples = 0.0;
    std::vector<double> actual(fanCount, 0.0);
    for (size_t i = 0; i < fanCount; ++i) actual[i] = bank.at(i).actualRpm;

    double totalMaximum = 0.0;
    for (size_t i = 0; i < fanCount; ++i) totalMaximum += bank.at(i).maxRpm;

    if (log && options.printTable) {
        *log << "  time   die    fan0  fan1   commanded\n";
        *log << "  ----------------------------------------\n";
    }

    const int steps = static_cast<int>(options.seconds / options.stepSeconds + 0.5);
    for (int step = 0; step < steps; ++step) {
        const double now = step * options.stepSeconds;
        const double seconds = options.stepSeconds;

        const Controller::TickResult tick = controller.tick(seconds);
        report.controlWrites += tick.writes;
        ++report.steps;

        double totalActual = 0.0;
        std::vector<double> want(fanCount, 0.0);
        for (size_t i = 0; i < fanCount; ++i) {
            const std::string index = std::to_string(i);
            const bool manual = ((model.u32Of("FS! ") >> i) & 1u) != 0;

            double target = 0.0;
            if (manual) {
                if (auto value = model.valueOf(("F" + index + "Tg").c_str())) target = *value;
            } else {
                target = firmwareTarget(die, bank.at(i).minRpm, bank.at(i).maxRpm);
            }
            want[i] = target;

            // A fan takes time to get where it is going, and takes longer to
            // slow down than to speed up.
            const double delta = target - actual[i];
            const double limit = (delta > 0 ? kFanRisePerSecond : kFanFallPerSecond) * seconds;
            actual[i] += std::max(-limit, std::min(limit, delta));
            actual[i] = std::max(bank.at(i).minRpm, std::min(bank.at(i).maxRpm, actual[i]));

            model.addFpe2(("F" + index + "Ac").c_str(), actual[i]);
            totalActual += actual[i];
        }

        // Heat in from the load, heat out through the airflow.
        const double fanFraction = totalMaximum > 0 ? totalActual / totalMaximum : 0.0;
        const double heatIn = kIdleWatts + kLoadWatts * loadAt(now, options);
        const double heatOut = (die - kAmbient) * (0.35 + 2.0 * fanFraction);
        die += (heatIn - heatOut) / kHeatCapacity * seconds;
        die = std::max(kAmbient, die);

        // Every temperature sensor follows the die.
        for (const char* key : {"TC0D", "TC0E", "TC0F", "TC0H", "TCXC", "TC0P", "TG0D", "TG0P"}) {
            model.addSp78(key, die);
        }

        report.peakCelsius = std::max(report.peakCelsius, die);
        rpmSum += totalActual;
        rpmSamples += 1.0;
        report.maximumRpm = std::max(report.maximumRpm, totalActual);

        if (log && options.printTable && (step % 5 == 0 || step == steps - 1)) {
            *log << std::setw(6) << std::fixed << std::setprecision(0) << now << " "
                 << std::setw(6) << std::setprecision(1) << die << " "
                 << std::setw(7) << std::setprecision(0) << actual[0] << " "
                 << std::setw(6) << (fanCount > 1 ? actual[1] : 0.0) << " "
                 << std::setw(7) << (tick.commandedRpm.empty() ? 0.0 : tick.commandedRpm[0]) << "\n";
        }
    }

    report.finalCelsius = die;
    report.averageRpm = rpmSamples > 0 ? rpmSum / rpmSamples : 0.0;

    // Letting go of the fans is part of the contract: whatever happens, the
    // firmware gets its machine back.
    if (!controller.restoreAllToSystem()) {
        report.fansReturnedToSystem = false;
        report.notes.push_back("a fan could not be returned to system control at shutdown");
    }
    if (model.u32Of("FS! ") != 0u) {
        report.fansReturnedToSystem = false;
        report.notes.push_back("the force mask was left non-zero");
    }

    return report;
}

}  // namespace fanforge
