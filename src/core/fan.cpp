#include "core/fan.h"

#include <cmath>

namespace fanforge {
namespace {

// The shared manual-force bitmask. Built through the normal key encoding so
// there is one definition of what "FS! " means, not two.
Key forceMaskKey() { return Key("FS! "); }

// A fan label is the text the SMC stores for F<n>ID. Some machines leave it
// blank or filled with the fill byte, so it is only used when it reads like
// text a person would recognise.
bool labelLooksUsable(const std::string& label) {
    if (label.empty() || label.size() > 32) return false;
    int alphanumeric = 0;
    for (char c : label) {
        if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) {
            ++alphanumeric;
        }
    }
    return alphanumeric >= 3;
}

}  // namespace

const char* manualModeName(ManualMode mode) {
    switch (mode) {
        case ManualMode::ForceMask: return "FS! mask";
        case ManualMode::PerFanMode: return "per-fan mode key";
        case ManualMode::None: break;
    }
    return "unavailable";
}

bool FanBank::discover(SmcDevice& smc) {
    fans_.clear();
    mode_ = ManualMode::None;
    forceMask_ = 0;
    notes_.clear();

    if (!smc.isOpen()) {
        notes_ = "the SMC is not open";
        return false;
    }

    double reportedCount = 0;
    if (!smc.readNumber(Key("FNum"), reportedCount)) {
        // Fanless Intel Macs (the 12" MacBook) report no fan count at all.
        // Temperatures still work, so this is not an error.
        notes_ = "FNum is absent: this machine reports no fans";
        return true;
    }

    int count = static_cast<int>(reportedCount + 0.5);
    if (count < 0) count = 0;
    if (static_cast<size_t>(count) > kMaxFans) {
        notes_ = "the SMC reports more fans than the protocol supports; using the first ten";
        count = static_cast<int>(kMaxFans);
    }

    for (int i = 0; i < count; ++i) {
        FanInfo fan;
        fan.index = i;

        std::string rawLabel;
        if (smc.readString(makeFanKey(i, "ID"), rawLabel) && labelLooksUsable(rawLabel)) {
            fan.label = rawLabel;
            fan.labelFromHardware = true;
        } else {
            fan.label = "Fan " + std::to_string(i);
        }

        const bool readMin = smc.readNumber(makeFanKey(i, "Mn"), fan.minRpm);
        const bool readMax = smc.readNumber(makeFanKey(i, "Mx"), fan.maxRpm);
        fan.hasLimits = readMin && readMax && fan.maxRpm > fan.minRpm;

        if (fan.hasLimits) {
            // The "safe" speed is not present on every model; the maximum is a
            // sound stand-in. Reject a safe speed outside the fan's own range.
            if (!smc.readNumber(makeFanKey(i, "Sf"), fan.safeRpm) || fan.safeRpm < fan.minRpm ||
                fan.safeRpm > fan.maxRpm) {
                fan.safeRpm = fan.maxRpm;
            }
            // A limit pair that is not believable is refused rather than used.
            if (!(fan.minRpm >= 0.0) || fan.minRpm > 20000.0 || fan.maxRpm > 20000.0) {
                fan.hasLimits = false;
            }
        }
        if (!fan.hasLimits) {
            fan.minRpm = 0.0;
            fan.maxRpm = 0.0;
            fan.safeRpm = 0.0;
            notes_ += fan.label + " reports no usable speed range; control is refused. ";
        }

        smc.readNumber(makeFanKey(i, "Ac"), fan.actualRpm);
        smc.readNumber(makeFanKey(i, "Tg"), fan.targetRpm);
        fans_.push_back(fan);
    }

    // Which manual-control style this machine has is read from the hardware,
    // never assumed: older Intel Macs use the shared force mask, newer ones a
    // per-fan mode key.
    if (count > 0) {
        if (smc.isWritable(forceMaskKey())) {
            mode_ = ManualMode::ForceMask;
            uint32_t mask = 0;
            if (smc.readU32(forceMaskKey(), mask)) forceMask_ = static_cast<uint16_t>(mask);
        } else if (smc.isWritable(makeFanKey(0, "Md"))) {
            mode_ = ManualMode::PerFanMode;
        } else {
            notes_ += "no manual fan-control path is exposed; monitoring only. ";
        }
    }

    // Apply the manual state now that the style is known.
    if (mode_ == ManualMode::ForceMask) {
        for (FanInfo& fan : fans_) fan.manual = ((forceMask_ >> fan.index) & 1u) != 0;
    } else if (mode_ == ManualMode::PerFanMode) {
        for (FanInfo& fan : fans_) {
            double value = 0;
            if (smc.readNumber(makeFanKey(fan.index, "Md"), value)) fan.manual = value != 0.0;
        }
    }

    return true;
}

double FanBank::clamp(size_t index, double rpm) const {
    if (index >= fans_.size()) return rpm;
    const FanInfo& fan = fans_[index];
    if (!fan.controllable()) return rpm;
    if (!(rpm == rpm)) return fan.minRpm;  // NaN
    if (rpm < fan.minRpm) return fan.minRpm;
    if (rpm > fan.maxRpm) return fan.maxRpm;
    return rpm;
}

bool FanBank::refresh(SmcDevice& smc) {
    if (fans_.empty()) return true;
    if (!smc.isOpen()) return false;

    for (FanInfo& fan : fans_) {
        smc.readNumber(makeFanKey(fan.index, "Ac"), fan.actualRpm);
        smc.readNumber(makeFanKey(fan.index, "Tg"), fan.targetRpm);
    }

    if (mode_ == ManualMode::ForceMask) {
        uint32_t mask = 0;
        if (smc.readU32(forceMaskKey(), mask)) forceMask_ = static_cast<uint16_t>(mask);
        for (FanInfo& fan : fans_) fan.manual = ((forceMask_ >> fan.index) & 1u) != 0;
    } else if (mode_ == ManualMode::PerFanMode) {
        for (FanInfo& fan : fans_) {
            double value = 0;
            if (smc.readNumber(makeFanKey(fan.index, "Md"), value)) fan.manual = value != 0.0;
        }
    }
    return true;
}

bool FanBank::setManual(SmcDevice& smc, size_t index, bool manual) {
    if (index >= fans_.size()) return false;

    if (mode_ == ManualMode::ForceMask) {
        if (index >= 16) return false;  // the mask is a ui16
        uint16_t mask = forceMask_;
        const uint16_t bit = static_cast<uint16_t>(1u << index);
        if (manual) {
            mask = static_cast<uint16_t>(mask | bit);
        } else {
            mask = static_cast<uint16_t>(mask & ~bit);
        }
        // Integers must round-trip exactly, so the tolerance is zero.
        if (!smc.writeNumberVerified(forceMaskKey(), mask, 0.0)) return false;
        forceMask_ = mask;
        fans_[index].manual = manual;
        return true;
    }

    if (mode_ == ManualMode::PerFanMode) {
        if (!smc.writeNumberVerified(makeFanKey(static_cast<int>(index), "Md"),
                                     manual ? 1.0 : 0.0, 0.0)) {
            return false;
        }
        fans_[index].manual = manual;
        return true;
    }

    return false;
}

bool FanBank::setTarget(SmcDevice& smc, size_t index, double rpm) {
    if (index >= fans_.size()) return false;
    FanInfo& fan = fans_[index];

    // Never command a fan whose range we could not read.
    if (!fan.controllable()) return false;

    const double requested = clamp(index, rpm);

    // Engage manual control first, so the target cannot be silently overridden
    // by the firmware's own loop. A machine that exposes no manual path at all
    // cannot be controlled, and saying so is better than writing a target the
    // firmware will ignore.
    if (!fan.manual) {
        if (mode_ == ManualMode::None) return false;
        if (!setManual(smc, index, true)) return false;
    }

    // fpe2 quantises to a quarter RPM, so the default tolerance of one RPM is
    // exact enough to tell "applied" from "ignored".
    if (!smc.writeNumberVerified(makeFanKey(static_cast<int>(index), "Tg"), requested,
                                verificationTolerance_)) {
        // The write could not be confirmed. Do not leave the fan pinned in a
        // state we cannot account for.
        setManual(smc, index, false);
        return false;
    }

    fan.targetRpm = requested;
    return true;
}

bool FanBank::returnToSystem(SmcDevice& smc, size_t index) {
    if (index >= fans_.size()) return false;
    if (mode_ == ManualMode::None) return true;
    if (!fans_[index].manual) return true;
    return setManual(smc, index, false);
}

bool FanBank::returnAllToSystem(SmcDevice& smc) {
    bool all = true;
    for (size_t i = 0; i < fans_.size(); ++i) {
        if (!returnToSystem(smc, i)) all = false;
    }
    return all;
}

}  // namespace fanforge
