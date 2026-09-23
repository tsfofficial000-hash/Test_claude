#pragma once
//
// Settings. A tiny line-based format, parsed entirely in portable C++ so the
// round trip is testable without touching a disk.
//
#include <string>
#include <vector>

#include "core/policy.h"

namespace fanforge {

struct Config {
    // Timing
    int pollIntervalMs = 1000;

    // Behaviour. `restoreOnExit` off means the fans are deliberately left where
    // they are when the app quits - useful for a long render, dangerous if you
    // forget, and documented as such.
    bool restoreOnExit = true;
    bool startMinimized = false;

    // Safety. At or above this temperature the controller stops following
    // policy and takes every fan to its maximum, because at that point the
    // machine is hotter than any policy the user chose would have produced.
    // Zero or less disables it.
    double emergencyCelsius = 95.0;

    // Display only. The SMC always speaks Celsius; this decides what the user
    // is shown.
    bool fahrenheit = false;

    // Append every reading to a CSV file, for looking at a thermal problem
    // after the fact rather than while it happens.
    bool recordCsv = false;

    // How fast the curve output may move. Rises are fast because heat is the
    // thing that matters; falls are slow because that is what stops the fan
    // surging up and down audibly.
    double maximumRisePerSecond = 900.0;
    double maximumFallPerSecond = 120.0;
    double deadbandRpm = 40.0;
    double verificationToleranceRpm = 1.0;

    // One entry per fan; missing entries mean "system control".
    std::vector<FanPolicy> fans;

    // The policy for a fan, or the default system policy when absent.
    FanPolicy policyFor(size_t index) const;
    void setPolicy(size_t index, const FanPolicy& policy);

    std::string toText() const;
    static Config fromText(const std::string& text);

    // Reads `path`, falling back to defaults if it is missing or unreadable.
    // Never throws.
    static Config read(const std::string& path);

    // Writes `path`, creating the parent directory when needed.
    bool write(const std::string& path) const;

    bool operator==(const Config& other) const;
};

// %APPDATA%\FanForge\config.ini on Windows, ~/.config/fanforge/config.ini
// elsewhere.
std::string defaultConfigPath();

// The CSV log, next to the configuration file.
std::string defaultLogPath();

// Creates the directory containing `path` if it does not exist. Exposed
// because a log file needs it too, and there should be one implementation of
// "make my parent directory" rather than two.
bool ensureParentDirectory(const std::string& path);

}  // namespace fanforge
