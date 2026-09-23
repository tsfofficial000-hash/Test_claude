#include "core/controller.h"

#include "core/units.h"

#include <cmath>
#include <limits>

namespace fanforge {
namespace {

// "No fan speed has been commanded yet" - distinct from "commanded zero".
const double kNotCommanded = std::numeric_limits<double>::quiet_NaN();

bool sameSpeed(double a, double b) {
    if (a != a || b != b) return false;  // NaN never matches
    const double delta = a - b;
    return (delta < 0 ? -delta : delta) < 1.0;
}

}  // namespace

const char* Controller::hottestComponentKey() { return "*"; }

bool Controller::initialise(const Config& config) {
    setConfig(config);
    enumerateSensors();
    sizeToHardware();

    // Seed the limiters from reality: a fan starts where it already is, so the
    // curve never causes an audible jump on startup.
    for (size_t i = 0; i < bank_.count(); ++i) {
        outputs_[i].reset(bank_.at(i).actualRpm);
        lastCommanded_[i] = kNotCommanded;
    }

    // A previous run - or a crash - may have left a fan forced. Anything the
    // configuration says should be on system control goes back immediately.
    bool allRestored = true;
    for (size_t i = 0; i < bank_.count(); ++i) {
        if (config_.policyFor(i).mode == FanControlMode::System && bank_.at(i).manual) {
            if (!bank_.returnToSystem(smc_, i)) {
                allRestored = false;
                lastCommanded_[i] = kNotCommanded;
            }
        }
    }
    bank_.refresh(smc_);
    return allRestored;
}

void Controller::setConfig(const Config& config) {
    config_ = config;
    sizeToHardware();
    // The write path's confirmation threshold is a setting, so it has to reach
    // the layer that does the confirming.
    bank_.setVerificationTolerance(config_.verificationToleranceRpm);
}

void Controller::sizeToHardware() {
    const size_t count = bank_.count();
    if (outputs_.size() != count) {
        outputs_.assign(count, CurveOutput(config_.maximumRisePerSecond,
                                           config_.maximumFallPerSecond, config_.deadbandRpm));
        lastCommanded_.assign(count, kNotCommanded);
    } else if (lastCommanded_.size() != count) {
        lastCommanded_.assign(count, kNotCommanded);
    }

    for (size_t i = 0; i < count; ++i) {
        outputs_[i].setLimits(config_.maximumRisePerSecond, config_.maximumFallPerSecond);
        outputs_[i].setDeadband(config_.deadbandRpm);
        if (!outputs_[i].primed()) outputs_[i].reset(bank_.at(i).actualRpm);
    }
}

void Controller::invalidateCommandCache() {
    lastCommanded_.assign(bank_.count(), kNotCommanded);
}

void Controller::refreshSensorList() {
    sensorsEnumerated_ = false;
    enumerateSensors();
}

void Controller::enumerateSensors() {
    sensorsEnumerated_ = true;
    sensorKeys_.clear();
    if (!smc_.isOpen()) return;

    for (const Key& key : smc_.allKeys()) {
        const std::string text = key.str();
        // Every temperature key the SMC exposes lives in the 'T' block.
        if (text.empty() || text[0] != 'T') continue;

        KeyInfo info;
        if (!smc_.keyInfo(key, info)) continue;
        // Temperatures are the signed fixed-point type. Anything else that
        // happens to start with T is not a temperature.
        if (Key(info.dataType) != Key("sp78")) continue;

        double celsius = 0;
        if (!smc_.readNumber(key, celsius)) continue;
        // A key that only ever reports a dead-sensor sentinel is not a sensor
        // worth putting in a list the user picks from.
        if (!plausibleTemperature(celsius)) continue;

        sensorKeys_.push_back(text);
    }

    if (sensorKeys_.empty()) {
        // Enumeration failed or the machine hides its keys. Fall back to the
        // well-known ones so monitoring still works; unreadable keys are simply
        // skipped when read.
        for (const char* fallback : {"TC0P", "TC0D", "TC0H", "TG0P", "TG0D", "TB0T"}) {
            sensorKeys_.push_back(fallback);
        }
    }
}

std::vector<SensorReading> Controller::readTemperatures() {
    if (!sensorsEnumerated_) enumerateSensors();

    std::vector<SensorReading> readings;
    readings.reserve(sensorKeys_.size());

    for (const std::string& key : sensorKeys_) {
        double celsius = 0;
        if (!smc_.readNumber(Key(key.c_str()), celsius)) continue;
        if (!plausibleTemperature(celsius)) continue;

        SensorReading reading;
        reading.key = key;
        reading.name = sensorName(key);
        reading.category = sensorCategory(key);
        reading.celsius = celsius;
        readings.push_back(reading);
    }

    sortByTemperature(readings);
    return readings;
}

void Controller::ensureSystem(size_t index, const std::string& reason, TickResult& result) {
    if (index >= bank_.count()) return;
    if (bank_.at(index).manual && !bank_.returnToSystem(smc_, index)) {
        result.notes.push_back(bank_.at(index).label +
                               ": could not be returned to system control");
        return;
    }
    lastCommanded_[index] = kNotCommanded;
    result.notes.push_back(reason);
}

Controller::TickResult Controller::tick(double seconds) {
    TickResult result;
    const size_t count = bank_.count();
    result.commandedRpm.assign(count, 0.0);

    sizeToHardware();

    // Temperatures are read before anything else, because a machine with no
    // fans still has sensors worth reporting: fanless Macs are monitoring-only,
    // not blind.
    const std::vector<SensorReading> readings = readTemperatures();
    result.hasTemperatures = !readings.empty();
    if (result.hasTemperatures) result.hottestCelsius = readings.front().celsius;

    if (count == 0) return result;

    // Re-read the fans. This is also how the loop notices that the firmware
    // dropped manual control - after a sleep, for instance - and re-asserts it.
    bank_.refresh(smc_);

    // Emergency cooling outranks every policy, including "system": if a sensor
    // is this hot, the firmware's own loop has already been given its chance,
    // and every fan goes to maximum until the heat passes. It is a ceiling on
    // temperature, not a substitute for the policies below it.
    const bool emergency = config_.emergencyCelsius > 0.0 && result.hasTemperatures &&
                           result.hottestCelsius >= config_.emergencyCelsius;
    result.emergency = emergency;
    if (emergency) {
        result.notes.push_back("emergency cooling: " +
                               formatTemperatureWithUnit(result.hottestCelsius, config_.fahrenheit) +
                               " reached the " +
                               formatTemperatureWithUnit(config_.emergencyCelsius, config_.fahrenheit) +
                               " limit, every fan taken to maximum");
    }

    auto temperatureFor = [&readings](const std::string& key) -> double {
        for (const SensorReading& reading : readings) {
            if (reading.key == key) return reading.celsius;
        }
        return std::numeric_limits<double>::quiet_NaN();
    };

    for (size_t i = 0; i < count; ++i) {
        const FanInfo& info = bank_.at(i);
        const FanPolicy policy = config_.policyFor(i);

        if (emergency) {
            if (!info.controllable()) {
                ensureSystem(i, info.label + ": emergency cooling, but no usable speed range "
                                             "was reported",
                             result);
                continue;
            }
            // Take the rate limiter to the emergency speed too, so that when
            // the heat passes the curve resumes from where the fan actually is
            // instead of unwinding a stale pre-emergency value.
            outputs_[i].reset(info.maxRpm);
            const double target = info.maxRpm;
            if (info.manual && sameSpeed(target, lastCommanded_[i])) {
                result.commandedRpm[i] = lastCommanded_[i];
                continue;
            }
            if (!bank_.setTarget(smc_, i, target)) {
                ensureSystem(i, info.label + ": the emergency speed could not be confirmed", result);
                continue;
            }
            lastCommanded_[i] = bank_.at(i).targetRpm;
            result.commandedRpm[i] = lastCommanded_[i];
            ++result.writes;
            continue;
        }

        if (policy.mode == FanControlMode::System) {
            if (info.manual) {
                ensureSystem(i, info.label + ": returned to system control", result);
            }
            continue;
        }

        // A fan whose own speed limits could not be read is never driven.
        if (!info.controllable()) {
            ensureSystem(i, info.label + ": no usable speed range was reported, left on system control",
                         result);
            continue;
        }

        double desired = 0.0;
        if (policy.mode == FanControlMode::Manual) {
            desired = policy.manualRpm;
            // A manual speed is an explicit instruction, so it is not ramped.
            outputs_[i].reset(desired);
        } else {
            if (!policy.curve.valid()) {
                ensureSystem(i, info.label + ": curve is not usable (" + policy.curve.invalidReason() +
                                      "), returned to system control",
                             result);
                continue;
            }

            double source = std::numeric_limits<double>::quiet_NaN();
            if (policy.curveSensorKey.empty() ||
                policy.curveSensorKey == hottestComponentKey()) {
                source = result.hasTemperatures ? result.hottestCelsius
                                                : std::numeric_limits<double>::quiet_NaN();
            } else {
                source = temperatureFor(policy.curveSensorKey);
            }

            if (!(source == source)) {
                ensureSystem(i, info.label + ": the curve's sensor " +
                                      (policy.curveSensorKey.empty() ? std::string("(hottest component)")
                                                                     : policy.curveSensorKey) +
                                      " is not reporting, returned to system control",
                             result);
                continue;
            }

            // Rate-limit the raw curve output rather than the clamped target, so
            // the limiter's state tracks temperature and not the fan's range.
            desired = outputs_[i].update(policy.curve.evaluate(source), seconds);
        }

        const double target = bank_.clamp(i, desired);
        const bool modeNeedsReasserting = !info.manual;

        if (!modeNeedsReasserting && sameSpeed(target, lastCommanded_[i])) {
            result.commandedRpm[i] = lastCommanded_[i];
            continue;  // nothing changed; do not touch the hardware
        }

        if (!bank_.setTarget(smc_, i, target)) {
            ensureSystem(i, info.label + ": the requested speed could not be confirmed, returned to "
                                         "system control",
                         result);
            continue;
        }

        lastCommanded_[i] = bank_.at(i).targetRpm;
        result.commandedRpm[i] = lastCommanded_[i];
        ++result.writes;
    }

    return result;
}

bool Controller::restoreAllToSystem() {
    const bool all = bank_.returnAllToSystem(smc_);
    invalidateCommandCache();
    return all;
}

}  // namespace fanforge
