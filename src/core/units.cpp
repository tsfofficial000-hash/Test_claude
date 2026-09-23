#include "core/units.h"

#include <cmath>
#include <cstdio>

namespace fanforge {
namespace {

constexpr double kScale = 9.0 / 5.0;
constexpr double kOffset = 32.0;

}  // namespace

double celsiusToFahrenheit(double celsius) { return celsius * kScale + kOffset; }

double fahrenheitToCelsius(double fahrenheit) { return (fahrenheit - kOffset) / kScale; }

const char* temperatureUnit(bool fahrenheit) { return fahrenheit ? "F" : "C"; }

std::string formatTemperature(double celsius, bool fahrenheit) {
    if (!(celsius == celsius)) return "--";  // NaN has no temperature
    const double value = fahrenheit ? celsiusToFahrenheit(celsius) : celsius;
    char buffer[32];
    std::snprintf(buffer, sizeof buffer, "%.1f", value);
    return buffer;
}

std::string formatTemperatureWithUnit(double celsius, bool fahrenheit) {
    if (!(celsius == celsius)) return "--";
    return formatTemperature(celsius, fahrenheit) + " " + temperatureUnit(fahrenheit);
}

std::string formatTemperatureRounded(double celsius, bool fahrenheit) {
    if (!(celsius == celsius)) return "--";
    const double value = fahrenheit ? celsiusToFahrenheit(celsius) : celsius;
    char buffer[32];
    std::snprintf(buffer, sizeof buffer, "%.0f", value);
    return buffer;
}

}  // namespace fanforge
