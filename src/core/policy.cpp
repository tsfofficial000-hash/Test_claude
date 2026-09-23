#include "core/policy.h"

namespace fanforge {

const char* modeName(FanControlMode mode) {
    switch (mode) {
        case FanControlMode::System: return "system";
        case FanControlMode::Manual: return "manual";
        case FanControlMode::Curve: return "curve";
    }
    return "system";
}

bool parseMode(const std::string& text, FanControlMode& out) {
    if (text == "system" || text == "auto") {
        out = FanControlMode::System;
        return true;
    }
    if (text == "manual" || text == "fixed") {
        out = FanControlMode::Manual;
        return true;
    }
    if (text == "curve" || text == "auto-curve") {
        out = FanControlMode::Curve;
        return true;
    }
    return false;
}

}  // namespace fanforge
