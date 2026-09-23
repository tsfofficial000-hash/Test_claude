#include "core/selftest.h"

#include <cmath>
#include <cstdint>

namespace fanforge {
namespace {

bool closeEnough(double a, double b, double tolerance) {
    if (!(a == a) || !(b == b)) return false;  // NaN on either side
    return std::fabs(a - b) <= tolerance;
}

std::string rpmText(double rpm) {
    if (!(rpm == rpm)) return "unreadable";
    return std::to_string(static_cast<long long>(std::llround(rpm))) + " RPM";
}

// The speed to ask for while testing: whatever the fan is doing right now, so
// that a passing test cannot be heard. A stopped or unreadable fan is tested at
// its own minimum instead of at zero, which it could not hold.
double probeSpeed(const FanInfo& fan) {
    const double current = fan.actualRpm;
    if (!(current == current)) return fan.minRpm;
    if (current < fan.minRpm) return fan.minRpm;
    if (current > fan.maxRpm) return fan.maxRpm;
    return current;
}

}  // namespace

bool SelfTestReport::ok() const {
    if (!applicable || steps.empty()) return false;
    for (const SelfTestStep& step : steps) {
        if (!step.ok) return false;
    }
    return true;
}

bool SelfTestReport::writePathConfirmed() const {
    for (const SelfTestStep& step : steps) {
        if (step.ok && step.label.find("write path") != std::string::npos) return true;
    }
    return false;
}

std::string SelfTestReport::summary() const {
    if (!applicable) return "not applicable: " + skippedReason;

    int passed = 0;
    for (const SelfTestStep& step : steps) {
        if (step.ok) ++passed;
    }
    std::string out = std::to_string(passed) + "/" + std::to_string(steps.size()) + " checks passed";
    if (!ok()) {
        for (const SelfTestStep& step : steps) {
            if (!step.ok) {
                out += " - first failure: " + step.label + " (" + step.detail + ")";
                break;
            }
        }
    }
    return out;
}

SelfTestReport runWritePathSelfTest(SmcDevice& smc, FanBank& bank) {
    SelfTestReport report;

    if (!smc.isOpen()) {
        report.skippedReason = "the SMC is not open";
        return report;
    }
    if (bank.empty()) {
        report.skippedReason = "this machine reports no fans";
        return report;
    }
    if (bank.manualMode() == ManualMode::None) {
        report.skippedReason =
            "this machine exposes no manual fan-control path, so there is no write path to test";
        return report;
    }
    report.applicable = true;

    // Start from the truth on the hardware rather than from what was last
    // commanded: the test's whole purpose is to find out what the hardware does.
    bank.refresh(smc);

    const double tolerance = bank.verificationTolerance() > 0 ? bank.verificationTolerance() : 1.0;

    for (size_t i = 0; i < bank.count(); ++i) {
        const std::string tag = "fan " + std::to_string(i);

        // Copy, because refresh() overwrites the bank's own entry.
        const FanInfo before = bank.at(i);
        const bool wasManual = before.manual;
        const double wasTarget = before.targetRpm;

        if (!before.controllable()) {
            report.steps.push_back(SelfTestStep{
                tag + " write path", false,
                "no usable speed range was reported, so this fan is never driven"});
            continue;
        }

        const double probe = bank.clamp(i, probeSpeed(before));

        const bool wrote = bank.setTarget(smc, i, probe);
        bank.refresh(smc);

        const double readback = bank.at(i).targetRpm;
        const bool targetOk = closeEnough(readback, probe, tolerance);
        const bool manualOk = bank.at(i).manual;
        report.steps.push_back(SelfTestStep{
            tag + " write path", wrote && targetOk && manualOk,
            "asked for " + rpmText(probe) + ", read back " + rpmText(readback) + ", manual " +
                (manualOk ? std::string("engaged") : std::string("not engaged"))});

        // Put the fan back the way it was found, whatever happened above.
        bool restored = false;
        std::string restoreDetail;
        if (wasManual && wasTarget == wasTarget && wasTarget > 0.0) {
            const double original = bank.clamp(i, wasTarget);
            restored = bank.setTarget(smc, i, original) && bank.at(i).manual;
            restoreDetail = "restored the original target of " + rpmText(original);
        } else {
            restored = bank.returnToSystem(smc, i);
            restoreDetail = "returned to firmware control";
        }
        if (restored) {
            bank.refresh(smc);
            const bool manualNow = bank.at(i).manual;
            const bool targetNow = closeEnough(bank.at(i).targetRpm,
                                               wasManual ? bank.clamp(i, wasTarget) : bank.at(i).targetRpm,
                                               tolerance);
            restored = wasManual ? (manualNow && targetNow) : !manualNow;
        }
        report.steps.push_back(SelfTestStep{tag + " restore", restored, restoreDetail});
    }

    return report;
}

}  // namespace fanforge
