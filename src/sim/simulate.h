#pragma once
//
// The whole thing, running against a machine that responds.
//
// The simulation is a closed loop: the controller writes to a simulated SMC,
// the simulated fans spin up towards whatever they were told (or stay where the
// firmware left them), and the resulting airflow changes the die temperature
// that feeds back into the controller. Nothing is stubbed out; the same
// Controller, FanBank and port protocol that run on real hardware run here.
//
// Its point is to answer the question a unit test cannot: does the loop
// actually keep the machine cool, and does it let go of the fans afterwards?
//
#include <iosfwd>
#include <string>
#include <vector>

namespace fanforge {

struct SimulationOptions {
    double seconds = 90.0;
    double stepSeconds = 1.0;

    // Seconds of light load before the heavy load starts. The interesting
    // behaviour is what happens at the moment heat arrives.
    double lightLoadUntil = 10.0;
    double lightLoad = 0.15;
    double heavyLoad = 1.0;

    // true: FanForge drives the fans from a curve.
    // false: the firmware's own law is left in charge, for comparison.
    bool curveControl = true;

    // The temperature window the default curve is stretched over: the fan stays
    // at its minimum until `curveStartCelsius` and reaches full speed at
    // `curveFullCelsius`. Widening the window is quieter and warmer; narrowing
    // it is louder and cooler. That trade is the whole point of the app, so the
    // simulation exposes it rather than hard-coding one opinion.
    double curveStartCelsius = 50.0;
    double curveFullCelsius = 92.0;

    bool printTable = false;
};

struct SimulationReport {
    double peakCelsius = 0.0;
    double finalCelsius = 0.0;
    double averageRpm = 0.0;
    double maximumRpm = 0.0;
    int controlWrites = 0;
    int steps = 0;
    bool fansReturnedToSystem = true;
    std::vector<std::string> notes;
};

// Runs the loop and returns what happened. `log` may be null.
SimulationReport runSimulation(const SimulationOptions& options, std::ostream* log);

}  // namespace fanforge
