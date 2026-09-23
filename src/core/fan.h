#pragma once
//
// Every fan, discovered from the hardware rather than assumed.
//
// The fan count comes from FNum, each fan's label from F<n>ID, and its speed
// limits from F<n>Mn / F<n>Mx. If the limits cannot be read, the fan is refused
// control rather than commanded with a guessed range.
//
#include <cstdint>
#include <string>
#include <vector>

#include "smc/device.h"

namespace fanforge {

// The SMC's own practical ceiling; Linux's applesmc driver caps at ten.
constexpr size_t kMaxFans = 10;

// How a machine exposes manual fan control. Older Intel Macs use one bit per
// fan in the shared "FS! " mask; newer ones (T2 and later) use a per-fan
// F<n>Md mode key. Which one is present is read from the hardware.
enum class ManualMode {
    None,        // no manual path: monitoring only
    ForceMask,   // "FS!" bitmask
    PerFanMode,  // F<n>Md per fan
};

const char* manualModeName(ManualMode mode);

struct FanInfo {
    int index = 0;
    std::string label;
    double minRpm = 0.0;
    double maxRpm = 0.0;
    double safeRpm = 0.0;
    double actualRpm = 0.0;
    double targetRpm = 0.0;
    bool hasLimits = false;
    bool manual = false;
    bool labelFromHardware = false;

    // A fan with no usable range must never be driven.
    bool controllable() const { return hasLimits && maxRpm > minRpm; }
    double range() const { return maxRpm - minRpm; }
};

class FanBank {
public:
    // Reads the fan count, labels, limits and manual-control style. Never
    // throws and never partially populates a fan it could not read limits for:
    // the fan still appears, marked uncontrollable.
    bool discover(SmcDevice& smc);

    size_t count() const { return fans_.size(); }
    bool empty() const { return fans_.empty(); }
    const FanInfo& at(size_t index) const { return fans_[index]; }
    const std::vector<FanInfo>& fans() const { return fans_; }

    ManualMode manualMode() const { return mode_; }
    uint16_t forceMask() const { return forceMask_; }
    const std::string& notes() const { return notes_; }

    // Clamps a request to the fan's own reported range. Returns the value
    // unchanged when the fan has no usable range, so callers must check
    // controllable() first.
    double clamp(size_t index, double rpm) const;

    // Re-reads actual speed, target and manual state for every fan.
    bool refresh(SmcDevice& smc);

    // Engages or releases manual control for one fan. Verified by readback.
    bool setManual(SmcDevice& smc, size_t index, bool manual);

    // Engages manual control and sets the target, clamped to the fan's range
    // and verified by readback. Returns false and leaves the fan on system
    // control if the write cannot be confirmed.
    bool setTarget(SmcDevice& smc, size_t index, double rpm);

    bool returnToSystem(SmcDevice& smc, size_t index);

    // Hands every fan back to the firmware. Best effort per fan; returns true
    // only when all of them were restored.
    bool returnAllToSystem(SmcDevice& smc);

    // How close a readback has to be to count as confirmed, in RPM. Defaults to
    // one RPM, which is comfortably above the quarter-RPM that fpe2 can
    // represent.
    void setVerificationTolerance(double rpm) { verificationTolerance_ = rpm > 0 ? rpm : 0.25; }
    double verificationTolerance() const { return verificationTolerance_; }

private:
    std::vector<FanInfo> fans_;
    ManualMode mode_ = ManualMode::None;
    uint16_t forceMask_ = 0;
    double verificationTolerance_ = 1.0;
    std::string notes_;
};

}  // namespace fanforge
