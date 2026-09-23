#pragma once
//
// A non-destructive check of the write path.
//
// Whether a machine's SMC actually accepts fan writes cannot be known from the
// outside: at the register level a rejected write and an accepted one look
// identical unless the value is read back. So this engages manual control at
// the speed each fan is *already* running at - which is why it is inaudible -
// confirms the readback, and then puts the fan back exactly as it was found.
//
// It is careful about which manual-control style the machine uses. Older Intel
// Macs take one bit per fan in the shared "FS! " mask; T2 and later machines
// take a per-fan F<n>Md key. Reading only the mask would report a false failure
// on a machine that controls perfectly well the other way.
//
#include <string>
#include <vector>

#include "core/fan.h"
#include "smc/device.h"

namespace fanforge {

struct SelfTestStep {
    std::string label;
    bool ok = false;
    std::string detail;
};

struct SelfTestReport {
    // False when the machine offers nothing to test. That is a fact about the
    // hardware, not a failure, so it is reported separately from `ok()`.
    bool applicable = false;
    std::string skippedReason;
    std::vector<SelfTestStep> steps;

    // True only when the test ran and every step passed.
    bool ok() const;

    // True when at least one fan's write path was confirmed end to end.
    bool writePathConfirmed() const;

    // One line suitable for a log or the CLI.
    std::string summary() const;
};

// Engages, verifies and releases manual control on every controllable fan, at
// the speed it is already running. Safe to run on real hardware.
SelfTestReport runWritePathSelfTest(SmcDevice& smc, FanBank& bank);

}  // namespace fanforge
