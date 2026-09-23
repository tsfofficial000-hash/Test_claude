#pragma once
//
// The SMC as seen through two I/O ports.
//
// This is the low-level interface the SMC has exposed since the first Intel
// Macs and the one the macOS kernel driver uses. It needs raw port access,
// which on Windows means a helper driver (InpOut32 / WinRing0); on this side
// of IByteIo it is ordinary portable C++.
//
// This path matters because it is the only one available on Macs whose Boot
// Camp install does not publish the \\.\APPLESMC device.
//
#include <string>

#include "smc/transport.h"

namespace fanforge {

namespace smc_port {
// Port pair. 0x300-0x31f is the range the SMC decodes.
constexpr uint16_t kDataPort = 0x300;
constexpr uint16_t kCommandPort = 0x304;

// Status register bits, read from kCommandPort.
constexpr uint8_t kStatusAwaitingData = 0x01;  // a byte is ready on the data port
constexpr uint8_t kStatusInputClosed = 0x02;   // SMC will ignore input for now
constexpr uint8_t kStatusBusy = 0x04;          // a command is in progress

// Command bytes, written to kCommandPort.
constexpr uint8_t kCommandRead = 0x10;
constexpr uint8_t kCommandWrite = 0x11;
constexpr uint8_t kCommandKeyByIndex = 0x12;
constexpr uint8_t kCommandKeyType = 0x13;

// How many polls to spend waiting on a status bit before giving up on the
// transaction. The loop calls IByteIo::idle() each time, so with the Windows
// shim yielding the CPU this is a few hundred milliseconds at most.
constexpr int kDefaultPollBudget = 4096;
}  // namespace smc_port

class PortIoTransport final : public ISmcTransport {
public:
    explicit PortIoTransport(IByteIo& io, int pollBudget = smc_port::kDefaultPollBudget);

    const char* name() const override { return "port-io"; }
    bool isOpen() const override { return io_ != nullptr; }
    std::string lastError() const override { return error_; }

    bool readKey(Key key, uint8_t size, Value& out) override;
    bool writeKey(Key key, const uint8_t* data, uint8_t size) override;
    bool keyInfo(Key key, KeyInfo& out) override;
    bool keyByIndex(uint32_t index, Key& out) override;

    // Optional cross-process exclusion. Not owned; must outlive this transport.
    void setTransactionLock(ITransactionLock* lock) { lock_ = lock; }
    ITransactionLock* transactionLock() const { return lock_; }

    // Exposed so the test suite can assert on the exact transaction shape.
    int pollBudget() const { return pollBudget_; }
    void setPollBudget(int budget) { pollBudget_ = budget; }

private:
    bool waitFor(uint8_t mask, uint8_t value);
    bool waitIdle();
    bool sanitize();
    bool sendCommand(uint8_t command);
    bool sendByte(uint8_t value, uint16_t port);
    bool readBlock(uint8_t command, const uint8_t arg[4], uint8_t length, uint8_t* out);
    bool writeBlock(uint8_t command, const uint8_t arg[4], const uint8_t* data, uint8_t length);
    void fail(const char* what);

    IByteIo* io_;
    int pollBudget_;
    ITransactionLock* lock_ = nullptr;
    std::string error_;
};

}  // namespace fanforge
