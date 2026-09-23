#pragma once
//
// Finding a way to talk to the SMC on this particular machine.
//
// A Mac under Boot Camp may expose the SMC through one of two quite different
// routes, and which one works depends on the model and on what Boot Camp
// installed:
//
//   1. \\.\APPLESMC  - a kernel device published by the "AppleSMC" service
//                      that ships with the Boot Camp drivers. No driver of our
//                      own is needed, and the driver serialises access.
//   2. Port I/O      - the SMC's two I/O ports, which need a helper driver
//                      (InpOut32 or WinRing0) to reach from a normal process.
//
// Supporting both is the point: a machine whose Boot Camp install does not
// publish the device can still be controlled, and a machine without a helper
// driver still gets monitoring for free.
//
#include <memory>
#include <string>
#include <vector>

#include "smc/transport.h"

namespace fanforge {

struct TransportAttempt {
    std::string name;    // "applesmc" or "port-io"
    bool ok = false;
    std::string detail;  // what happened, in plain language
};

class SmcSession {
public:
    SmcSession();
    ~SmcSession();

    // Move-only: the session owns a device handle and, possibly, a loaded
    // helper driver.
    SmcSession(SmcSession&& other) noexcept;
    SmcSession& operator=(SmcSession&& other) noexcept;
    SmcSession(const SmcSession&) = delete;
    SmcSession& operator=(const SmcSession&) = delete;

    // Tries each access path in preference order, keeps the first that opens,
    // and validates it by reading the key count. A path that opens but cannot
    // answer is rejected, so "the driver is installed" and "the SMC is
    // reachable" are not confused for one another.
    bool open();
    void close();

    ISmcTransport* transport() const;
    bool isOpen() const { return transport() != nullptr; }

    const std::vector<TransportAttempt>& attempts() const;
    std::string activeName() const;

    // One line per candidate tried, whether or not it worked.
    std::string summary() const;

private:
    struct State;
    std::unique_ptr<State> state_;
};

}  // namespace fanforge
