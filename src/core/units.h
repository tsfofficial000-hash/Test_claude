#pragma once
//
// Temperature units. Kept as text helpers with no degree symbol on purpose:
// the CLI prints these straight to a Windows console, and the GUI has its own
// wide-string path where it can add the symbol itself.
//
#include <string>

namespace fanforge {

double celsiusToFahrenheit(double celsius);
double fahrenheitToCelsius(double fahrenheit);

// "C" or "F", for a heading or a suffix.
const char* temperatureUnit(bool fahrenheit);

// One decimal place, no unit: "62.5".
std::string formatTemperature(double celsius, bool fahrenheit);

// One decimal place with its unit: "62.5 C".
std::string formatTemperatureWithUnit(double celsius, bool fahrenheit);

// Whole degrees, no unit, for compact places such as a tray tooltip: "63".
std::string formatTemperatureRounded(double celsius, bool fahrenheit);

}  // namespace fanforge
