#pragma once
//
// The control loop.
//
// A tick reads every temperature, evaluates each fan's policy, and actuates.
// It is a plain function of elapsed time rather than a thread, so the exact
// same code path is what the tests drive and what the GUI drives - only the
// caller differs.
//
#include <string>
#include <vector>

#include "core/config.h"
#include "core/fan.h"
#include "core/sensordb.h"
#include "smc/device.h"

namespace fanforge {

class Controller {
public:
    Controller(SmcDevice& smc, FanBank& bank) : smc_(smc), bank_(bank) {}

    // Discovers the temperature sensors, sizes the policies to the hardware,
    // seeds the rate limiters from the speeds the fans are already running at,
    // and hands any previously forced fan back to the firmware.
    bool initialise(const Config& config);

    const Config& config() const { return config_; }
    void setConfig(const Config& config);

    struct TickResult {
        bool hasTemperatures = false;
        double hottestCelsius = 0.0;
        // Set when the machine was hot enough that every fan was taken to
        // maximum regardless of its policy.
        bool emergency = false;
        int writes = 0;
        std::vector<std::string> notes;
        // Requested speed per fan; 0 means the fan is on system control.
        std::vector<double> commandedRpm;
    };

    // One control cycle. `seconds` is the elapsed time since the last tick and
    // drives the rate limiters.
    TickResult tick(double seconds);

    // Every plausible temperature the SMC reports, hottest first.
    std::vector<SensorReading> readTemperatures();
    const std::vector<std::string>& sensorKeys() const { return sensorKeys_; }

    // Re-walks the key space. Cheap enough to call when the user opens a
    // sensor picker; not something to do every tick.
    void refreshSensorList();

    // Hands every fan back to the firmware.
    bool restoreAllToSystem();

    // After a resume, or after anything else that may have reset the hardware,
    // forget what was last commanded so every policy is re-asserted.
    void invalidateCommandCache();

    // The sentinel used when no curve sensor is chosen.
    static const char* hottestComponentKey();

private:
    void enumerateSensors();
    void sizeToHardware();
    void ensureSystem(size_t index, const std::string& reason, TickResult& result);

    SmcDevice& smc_;
    FanBank& bank_;
    Config config_;
    std::vector<CurveOutput> outputs_;
    std::vector<double> lastCommanded_;
    std::vector<std::string> sensorKeys_;
    bool sensorsEnumerated_ = false;
};

}  // namespace fanforge
