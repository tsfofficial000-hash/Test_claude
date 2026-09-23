#pragma once
//
// Human names, categories and plausibility for the SMC's temperature keys.
//
// Nothing here decides what a machine has: the catalogue only names keys the
// hardware reports. An unknown key still shows up, with a generated name.
//
#include <string>
#include <vector>

namespace fanforge {

enum class SensorCategory {
    Cpu,
    Gpu,
    Memory,
    Storage,
    Battery,
    Enclosure,
    Ambient,
    Palm,
    Wireless,
    Power,
    Display,
    Other,
};

const char* categoryName(SensorCategory category);

// The SMC reports dead sensors as sentinels, not as errors. Anything outside
// this band is treated as "not a real reading" rather than fed to a curve.
constexpr double kMinimumPlausibleCelsius = -20.0;
constexpr double kMaximumPlausibleCelsius = 130.0;

bool plausibleTemperature(double celsius);

// Display name for a temperature key: "TC0P" -> "CPU proximity". Unknown keys
// get a name derived from the key plus its category, never an empty string.
std::string sensorName(const std::string& key);
SensorCategory sensorCategory(const std::string& key);

struct SensorReading {
    std::string key;
    std::string name;
    SensorCategory category = SensorCategory::Other;
    double celsius = 0.0;
};

// Sorts hottest first, which is the order the UI and the "hottest component"
// curve source both want.
void sortByTemperature(std::vector<SensorReading>& readings);

}  // namespace fanforge
