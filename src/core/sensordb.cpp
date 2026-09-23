#include "core/sensordb.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <string>

namespace fanforge {
namespace {

struct Entry {
    const char* key;
    const char* name;
    SensorCategory category;
};

// Keys verified to appear on Intel Macs, gathered from the sensor set the
// platform's own SMC tools expose.
const Entry kCatalogue[] = {
    // CPU
    {"TC0P", "CPU proximity", SensorCategory::Cpu},
    {"TC0D", "CPU die", SensorCategory::Cpu},
    {"TC0E", "CPU die (remote)", SensorCategory::Cpu},
    {"TC0F", "CPU die (filtered)", SensorCategory::Cpu},
    {"TC0H", "CPU heatsink", SensorCategory::Cpu},
    {"TC0C", "CPU core", SensorCategory::Cpu},
    {"TCXC", "CPU package (PECI)", SensorCategory::Cpu},
    // GPU
    {"TG0P", "GPU proximity", SensorCategory::Gpu},
    {"TG0D", "GPU die", SensorCategory::Gpu},
    {"TG0H", "GPU heatsink", SensorCategory::Gpu},
    {"TG0C", "GPU core", SensorCategory::Gpu},
    {"TG1D", "GPU die 2", SensorCategory::Gpu},
    {"TG1P", "GPU proximity 2", SensorCategory::Gpu},
    // Power and logic board
    {"TP0P", "Power supply proximity", SensorCategory::Power},
    {"TPCD", "Platform controller die", SensorCategory::Power},
    {"TN0P", "Northbridge proximity", SensorCategory::Power},
    {"TN0D", "Northbridge die", SensorCategory::Power},
    {"TN0H", "Northbridge heatsink", SensorCategory::Power},
    {"TN1D", "Northbridge die 2", SensorCategory::Power},
    {"TS0C", "SMC die", SensorCategory::Power},
    // Memory
    {"TM0P", "Memory proximity", SensorCategory::Memory},
    {"TM0D", "Memory die", SensorCategory::Memory},
    {"TM1P", "Memory proximity 2", SensorCategory::Memory},
    {"TM2P", "Memory proximity 3", SensorCategory::Memory},
    // Battery
    {"TB0T", "Battery", SensorCategory::Battery},
    {"TB1T", "Battery 2", SensorCategory::Battery},
    {"TB2T", "Battery 3", SensorCategory::Battery},
    {"TB3T", "Battery 4", SensorCategory::Battery},
    // Enclosure, palm rest, display
    {"Th0H", "Heatsink", SensorCategory::Enclosure},
    {"Th1H", "Heatsink 2", SensorCategory::Enclosure},
    {"Ts0P", "Palm rest", SensorCategory::Palm},
    {"Ts0S", "Palm rest (skin)", SensorCategory::Palm},
    {"Ts1P", "Palm rest (right)", SensorCategory::Palm},
    {"Ts1S", "Palm rest (right, skin)", SensorCategory::Palm},
    {"Ts2P", "Palm rest (rear)", SensorCategory::Palm},
    {"TL0P", "Display proximity", SensorCategory::Display},
    {"TL1P", "Display proximity 2", SensorCategory::Display},
    {"TL0V", "Display backlight", SensorCategory::Display},
    // Ambient and wireless
    {"TA0P", "Ambient", SensorCategory::Ambient},
    {"TA1P", "Ambient 2", SensorCategory::Ambient},
    {"TW0P", "Wireless proximity", SensorCategory::Wireless},
    {"TW1P", "Wireless proximity 2", SensorCategory::Wireless},
    {"TV0P", "Thunderbolt proximity", SensorCategory::Other},
    {"TV1P", "Thunderbolt proximity 2", SensorCategory::Other},
};

std::string trim(const std::string& s) {
    size_t begin = 0;
    size_t end = s.size();
    while (begin < end && (s[begin] == ' ')) ++begin;
    while (end > begin && (s[end - 1] == ' ')) --end;
    return s.substr(begin, end - begin);
}

bool isDecimalDigit(char c) { return c >= '0' && c <= '9'; }

}  // namespace

const char* categoryName(SensorCategory category) {
    switch (category) {
        case SensorCategory::Cpu: return "CPU";
        case SensorCategory::Gpu: return "GPU";
        case SensorCategory::Memory: return "Memory";
        case SensorCategory::Storage: return "Storage";
        case SensorCategory::Battery: return "Battery";
        case SensorCategory::Enclosure: return "Enclosure";
        case SensorCategory::Ambient: return "Ambient";
        case SensorCategory::Palm: return "Palm rest";
        case SensorCategory::Wireless: return "Wireless";
        case SensorCategory::Power: return "Power";
        case SensorCategory::Display: return "Display";
        case SensorCategory::Other: break;
    }
    return "Other";
}

bool plausibleTemperature(double celsius) {
    if (!(celsius == celsius)) return false;  // NaN
    if (celsius <= kMinimumPlausibleCelsius) return false;
    if (celsius >= kMaximumPlausibleCelsius) return false;
    return true;
}

std::string sensorName(const std::string& key) {
    const std::string k = trim(key);

    for (const Entry& e : kCatalogue) {
        if (k == e.key) return e.name;
    }

    // Per-core CPU sensors are a numbered family: TC1C, TC2C ... TC9C.
    if (k.size() == 4 && k[0] == 'T' && k[1] == 'C' && isDecimalDigit(k[2]) && k[3] == 'C') {
        return std::string("CPU core ") + k[2];
    }
    // GPU siblings.
    if (k.size() == 4 && k.rfind("TG", 0) == 0 && isDecimalDigit(k[2])) {
        if (k[3] == 'D') return std::string("GPU die ") + k[2];
        if (k[3] == 'P') return std::string("GPU proximity ") + k[2];
        if (k[3] == 'H') return std::string("GPU heatsink ") + k[2];
    }
    // Battery packs.
    if (k.size() == 4 && k.rfind("TB", 0) == 0 && isDecimalDigit(k[2]) && k[3] == 'T') {
        return std::string("Battery ") + k[2];
    }
    // Palm rest / skin sensors.
    if (k.size() == 4 && k.rfind("Ts", 0) == 0 && isDecimalDigit(k[2])) {
        return std::string("Palm rest ") + k[2] + (k[3] == 'S' ? " (skin)" : "");
    }
    // Memory banks.
    if (k.size() == 4 && k.rfind("TM", 0) == 0 && isDecimalDigit(k[2])) {
        return std::string("Memory ") + k[2];
    }

    // Unknown keys still get a usable label: the key itself plus what family it
    // looks like it belongs to.
    const std::string family = categoryName(sensorCategory(k));
    if (family != std::string("Other")) return k + " (" + family + ")";
    return k;
}

SensorCategory sensorCategory(const std::string& key) {
    const std::string k = trim(key);
    for (const Entry& e : kCatalogue) {
        if (k == e.key) return e.category;
    }
    if (k.size() >= 2 && k.rfind("TC", 0) == 0) return SensorCategory::Cpu;
    if (k.size() >= 2 && k.rfind("TG", 0) == 0) return SensorCategory::Gpu;
    if (k.size() >= 2 && k.rfind("TB", 0) == 0) return SensorCategory::Battery;
    if (k.size() >= 2 && k.rfind("TM", 0) == 0) return SensorCategory::Memory;
    if (k.size() >= 2 && k.rfind("Ts", 0) == 0) return SensorCategory::Palm;
    if (k.size() >= 2 && k.rfind("Th", 0) == 0) return SensorCategory::Enclosure;
    if (k.size() >= 2 && k.rfind("TW", 0) == 0) return SensorCategory::Wireless;
    if (k.size() >= 2 && k.rfind("TL", 0) == 0) return SensorCategory::Display;
    if (k.size() >= 2 && k.rfind("TP", 0) == 0) return SensorCategory::Power;
    if (k.size() >= 2 && k.rfind("TN", 0) == 0) return SensorCategory::Power;
    if (k.size() >= 2 && k.rfind("TS", 0) == 0) return SensorCategory::Storage;
    return SensorCategory::Other;
}

void sortByTemperature(std::vector<SensorReading>& readings) {
    std::sort(readings.begin(), readings.end(),
              [](const SensorReading& a, const SensorReading& b) {
                  if (a.celsius != b.celsius) return a.celsius > b.celsius;
                  return a.key < b.key;
              });
}

}  // namespace fanforge
