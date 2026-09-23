#include "core/config.h"

#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>

#ifdef _WIN32
#include <direct.h>
#include <windows.h>
#else
#include <sys/stat.h>
#include <sys/types.h>
#endif

namespace fanforge {
namespace {

std::string trim(const std::string& s) {
    size_t begin = 0;
    size_t end = s.size();
    while (begin < end && (s[begin] == ' ' || s[begin] == '\t' || s[begin] == '\r')) ++begin;
    while (end > begin && (s[end - 1] == ' ' || s[end - 1] == '\t' || s[end - 1] == '\r')) --end;
    return s.substr(begin, end - begin);
}

std::string formatNumber(double value) {
    char buffer[32];
    std::snprintf(buffer, sizeof buffer, "%.6g", value);
    return buffer;
}

bool parseBool(const std::string& text, bool fallback) {
    if (text == "1" || text == "true" || text == "yes" || text == "on") return true;
    if (text == "0" || text == "false" || text == "no" || text == "off") return false;
    return fallback;
}

bool parseDouble(const std::string& text, double& out) {
    if (text.empty()) return false;
    char* end = nullptr;
    const double value = std::strtod(text.c_str(), &end);
    if (end == text.c_str() || (end && *end != '\0')) return false;
    if (!(value == value)) return false;  // NaN
    out = value;
    return true;
}

bool parseInt(const std::string& text, int& out) {
    double value = 0;
    if (!parseDouble(text, value)) return false;
    out = static_cast<int>(value + (value < 0 ? -0.5 : 0.5));
    return true;
}

// Curves serialise as "45:2160,58:2900". Spaces are tolerated on input so the
// file stays hand-editable.
std::string formatCurve(const FanCurve& curve) {
    std::string out;
    for (size_t i = 0; i < curve.points.size(); ++i) {
        if (i) out += ',';
        out += formatNumber(curve.points[i].temperature);
        out += ':';
        out += formatNumber(curve.points[i].rpm);
    }
    return out;
}

FanCurve parseCurve(const std::string& text) {
    FanCurve curve;
    std::string current;
    std::istringstream stream(text);
    while (std::getline(stream, current, ',')) {
        const std::string point = trim(current);
        if (point.empty()) continue;
        const size_t colon = point.find(':');
        if (colon == std::string::npos) continue;
        double temperature = 0;
        double rpm = 0;
        if (!parseDouble(trim(point.substr(0, colon)), temperature)) continue;
        if (!parseDouble(trim(point.substr(colon + 1)), rpm)) continue;
        curve.points.push_back(CurvePoint{temperature, rpm});
    }
    curve.normalise();
    return curve;
}

bool makeDirectory(const std::string& path) {
    if (path.empty()) return true;
#ifdef _WIN32
    // Create each component in turn; CreateDirectory fails on the second
    // component if the first does not exist.
    std::string built;
    for (size_t i = 0; i < path.size(); ++i) {
        built += path[i];
        const bool last = (i + 1 == path.size());
        if (path[i] == '\\' || path[i] == '/' || last) {
            if (built == "\\\\" || built.size() <= 3) continue;
            if (!CreateDirectoryA(built.c_str(), nullptr) &&
                GetLastError() != ERROR_ALREADY_EXISTS) {
                if (!last) continue;
                return false;
            }
        }
    }
    return true;
#else
    std::string built;
    for (size_t i = 0; i < path.size(); ++i) {
        built += path[i];
        if (path[i] == '/' || i + 1 == path.size()) {
            if (built == "/" || built.empty()) continue;
            if (::mkdir(built.c_str(), 0755) != 0 && errno != EEXIST) {
                if (i + 1 != path.size()) continue;
                return false;
            }
        }
    }
    return true;
#endif
}

std::string parentDirectory(const std::string& path) {
    const size_t slash = path.find_last_of("\\/");
    if (slash == std::string::npos) return {};
    return path.substr(0, slash);
}

// "fan.2.mode" -> index 2, field "mode".
bool parseFanField(const std::string& key, int& index, std::string& field) {
    if (key.rfind("fan.", 0) != 0) return false;
    const size_t dot = key.find('.', 4);
    if (dot == std::string::npos) return false;
    if (!parseInt(key.substr(4, dot - 4), index)) return false;
    if (index < 0) return false;
    field = key.substr(dot + 1);
    return !field.empty();
}

}  // namespace

FanPolicy Config::policyFor(size_t index) const {
    if (index < fans.size()) return fans[index];
    return FanPolicy{};
}

void Config::setPolicy(size_t index, const FanPolicy& policy) {
    if (fans.size() <= index) fans.resize(index + 1);
    fans[index] = policy;
}

std::string Config::toText() const {
    std::ostringstream out;
    out << "# FanForge configuration.\n";
    out << "# Lines are key=value. Curve points are temperature:rpm pairs.\n";
    out << "version=1\n";
    out << "poll_interval_ms=" << pollIntervalMs << "\n";
    out << "restore_on_exit=" << (restoreOnExit ? 1 : 0) << "\n";
    out << "start_minimized=" << (startMinimized ? 1 : 0) << "\n";
    out << "emergency_celsius=" << formatNumber(emergencyCelsius) << "\n";
    out << "fahrenheit=" << (fahrenheit ? 1 : 0) << "\n";
    out << "record_csv=" << (recordCsv ? 1 : 0) << "\n";
    out << "max_rise_rpm_per_s=" << formatNumber(maximumRisePerSecond) << "\n";
    out << "max_fall_rpm_per_s=" << formatNumber(maximumFallPerSecond) << "\n";
    out << "deadband_rpm=" << formatNumber(deadbandRpm) << "\n";
    out << "verification_tolerance_rpm=" << formatNumber(verificationToleranceRpm) << "\n";

    for (size_t i = 0; i < fans.size(); ++i) {
        out << "fan." << i << ".mode=" << modeName(fans[i].mode) << "\n";
        out << "fan." << i << ".sensor=" << fans[i].curveSensorKey << "\n";
        out << "fan." << i << ".manual_rpm=" << formatNumber(fans[i].manualRpm) << "\n";
        out << "fan." << i << ".curve=" << formatCurve(fans[i].curve) << "\n";
    }
    return out.str();
}

Config Config::fromText(const std::string& text) {
    Config config;
    config.fans.clear();

    std::istringstream stream(text);
    std::string line;
    while (std::getline(stream, line)) {
        const std::string stripped = trim(line);
        if (stripped.empty() || stripped[0] == '#') continue;

        const size_t equals = stripped.find('=');
        if (equals == std::string::npos) continue;
        const std::string key = trim(stripped.substr(0, equals));
        const std::string value = trim(stripped.substr(equals + 1));

        if (key == "poll_interval_ms") {
            int ms = config.pollIntervalMs;
            if (parseInt(value, ms) && ms > 0) {
                // A poll faster than twice a second would hammer the SMC for no
                // benefit; the controller is not a real-time loop.
                config.pollIntervalMs = ms < 200 ? 200 : ms;
            }
        } else if (key == "restore_on_exit") {
            config.restoreOnExit = parseBool(value, config.restoreOnExit);
        } else if (key == "start_minimized") {
            config.startMinimized = parseBool(value, config.startMinimized);
        } else if (key == "emergency_celsius") {
            // A negative value is accepted and means "disabled"; rejecting it
            // would silently turn the feature back on for someone who was
            // deliberately switching it off.
            double v = 0;
            if (parseDouble(value, v)) config.emergencyCelsius = v;
        } else if (key == "fahrenheit") {
            config.fahrenheit = parseBool(value, config.fahrenheit);
        } else if (key == "record_csv") {
            config.recordCsv = parseBool(value, config.recordCsv);
        } else if (key == "max_rise_rpm_per_s") {
            double v = 0;
            if (parseDouble(value, v) && v > 0) config.maximumRisePerSecond = v;
        } else if (key == "max_fall_rpm_per_s") {
            double v = 0;
            if (parseDouble(value, v) && v > 0) config.maximumFallPerSecond = v;
        } else if (key == "deadband_rpm") {
            double v = 0;
            if (parseDouble(value, v) && v >= 0) config.deadbandRpm = v;
        } else if (key == "verification_tolerance_rpm") {
            double v = 0;
            if (parseDouble(value, v) && v >= 0) config.verificationToleranceRpm = v;
        } else {
            int index = 0;
            std::string field;
            if (!parseFanField(key, index, field)) continue;
            const size_t slot = static_cast<size_t>(index);

            FanPolicy policy = config.policyFor(slot);
            if (field == "mode") {
                FanControlMode mode;
                if (parseMode(value, mode)) policy.mode = mode;
            } else if (field == "sensor") {
                policy.curveSensorKey = value;
            } else if (field == "manual_rpm") {
                double v = 0;
                if (parseDouble(value, v)) policy.manualRpm = v;
            } else if (field == "curve") {
                policy.curve = parseCurve(value);
            } else {
                continue;
            }
            config.setPolicy(slot, policy);
        }
    }
    return config;
}

Config Config::read(const std::string& path) {
    std::ifstream file(path.c_str(), std::ios::binary);
    if (!file) return Config{};
    std::ostringstream buffer;
    buffer << file.rdbuf();
    return Config::fromText(buffer.str());
}

bool Config::write(const std::string& path) const {
    const std::string directory = parentDirectory(path);
    if (!directory.empty()) makeDirectory(directory);

    std::ofstream file(path.c_str(), std::ios::binary | std::ios::trunc);
    if (!file) return false;
    file << toText();
    return file.good();
}

bool Config::operator==(const Config& other) const {
    if (pollIntervalMs != other.pollIntervalMs) return false;
    if (restoreOnExit != other.restoreOnExit) return false;
    if (startMinimized != other.startMinimized) return false;
    if (emergencyCelsius != other.emergencyCelsius) return false;
    if (fahrenheit != other.fahrenheit) return false;
    if (recordCsv != other.recordCsv) return false;
    if (maximumRisePerSecond != other.maximumRisePerSecond) return false;
    if (maximumFallPerSecond != other.maximumFallPerSecond) return false;
    if (deadbandRpm != other.deadbandRpm) return false;
    if (verificationToleranceRpm != other.verificationToleranceRpm) return false;
    if (fans.size() != other.fans.size()) return false;
    for (size_t i = 0; i < fans.size(); ++i) {
        if (fans[i].mode != other.fans[i].mode) return false;
        if (fans[i].curveSensorKey != other.fans[i].curveSensorKey) return false;
        if (fans[i].manualRpm != other.fans[i].manualRpm) return false;
        if (fans[i].curve.points.size() != other.fans[i].curve.points.size()) return false;
        for (size_t p = 0; p < fans[i].curve.points.size(); ++p) {
            if (fans[i].curve.points[p].temperature != other.fans[i].curve.points[p].temperature) {
                return false;
            }
            if (fans[i].curve.points[p].rpm != other.fans[i].curve.points[p].rpm) return false;
        }
    }
    return true;
}

bool ensureParentDirectory(const std::string& path) {
    const std::string directory = parentDirectory(path);
    if (directory.empty()) return true;
    return makeDirectory(directory);
}

std::string defaultConfigPath() {
#ifdef _WIN32
    const char* appData = std::getenv("APPDATA");
    if (appData && *appData) return std::string(appData) + "\\FanForge\\config.ini";
    const char* userProfile = std::getenv("USERPROFILE");
    if (userProfile && *userProfile) {
        return std::string(userProfile) + "\\FanForge\\config.ini";
    }
    return "FanForge\\config.ini";
#else
    const char* home = std::getenv("HOME");
    if (home && *home) return std::string(home) + "/.config/fanforge/config.ini";
    return "./fanforge-config.ini";
#endif
}

std::string defaultLogPath() {
    const std::string config = defaultConfigPath();
    const size_t slash = config.find_last_of("\\/");
    if (slash == std::string::npos) return "fanforge-log.csv";
    return config.substr(0, slash + 1) + "fanforge-log.csv";
}

}  // namespace fanforge
