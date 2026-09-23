//
// The same session on a platform that has no SMC. It exists so the command-line
// tool, the simulation and the test suite all build and run anywhere; the
// failure it reports is accurate rather than a stub that pretends.
//
#ifndef _WIN32

#include "platform/platform.h"

namespace fanforge {

struct SmcSession::State {
    std::vector<TransportAttempt> attempts;
};

SmcSession::SmcSession() : state_(new State()) {}
SmcSession::~SmcSession() { close(); }

SmcSession::SmcSession(SmcSession&& other) noexcept : state_(std::move(other.state_)) {}

SmcSession& SmcSession::operator=(SmcSession&& other) noexcept {
    if (this != &other) {
        close();
        state_ = std::move(other.state_);
    }
    return *this;
}

void SmcSession::close() {}

ISmcTransport* SmcSession::transport() const { return nullptr; }

// A moved-from session has no state, and is still destroyed normally. Returning
// a shared empty list keeps that from being a null dereference - the same bug
// the Windows implementation had, and the reason this shape is tested here
// where the tests actually run.
const std::vector<TransportAttempt>& SmcSession::attempts() const {
    static const std::vector<TransportAttempt> kNone;
    return state_ ? state_->attempts : kNone;
}

std::string SmcSession::activeName() const { return "none"; }

std::string SmcSession::summary() const {
    if (!state_) return "no session state\n";

    std::string out;
    for (const TransportAttempt& attempt : state_->attempts) {
        out += attempt.name + ": unavailable - " + attempt.detail + "\n";
    }
    return out;
}

bool SmcSession::open() {
    if (!state_) state_ = std::make_unique<State>();
    state_->attempts.clear();
    TransportAttempt attempt;
    attempt.name = "applesmc";
    attempt.ok = false;
    attempt.detail =
        "the SMC interface is only reachable from Windows running natively on an Intel Mac "
        "(Boot Camp). This is not such a system.";
    state_->attempts.push_back(attempt);
    return false;
}

}  // namespace fanforge

#endif  // !_WIN32
