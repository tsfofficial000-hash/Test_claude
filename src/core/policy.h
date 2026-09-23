#pragma once
//
// What the user asked each fan to do. Kept separate from the controller so
// configuration can be read and written without dragging in the hardware.
//
#include <string>

#include "core/curve.h"

namespace fanforge {

enum class FanControlMode {
    System,  // hand the fan back to the firmware's own thermal loop
    Manual,  // a fixed speed the user picked
    Curve,   // a temperature -> speed curve
};

const char* modeName(FanControlMode mode);
bool parseMode(const std::string& text, FanControlMode& out);

struct FanPolicy {
    FanControlMode mode = FanControlMode::System;

    // Which sensor the curve follows. Empty means "whichever component is
    // hottest", which is the safe default for a machine whose sensor names
    // differ between models.
    std::string curveSensorKey;

    double manualRpm = 0.0;
    FanCurve curve;
};

}  // namespace fanforge
