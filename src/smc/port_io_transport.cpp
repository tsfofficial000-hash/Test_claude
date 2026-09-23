#include "smc/port_io_transport.h"

#include <cstring>

namespace fanforge {
using namespace smc_port;

PortIoTransport::PortIoTransport(IByteIo& io, int pollBudget)
    : io_(&io), pollBudget_(pollBudget > 0 ? pollBudget : kDefaultPollBudget) {}

void PortIoTransport::fail(const char* what) {
    if (error_.empty()) error_ = what;
}

// Polls the status register until (status & mask) == value. Bounded, so a
// wedged or absent SMC turns into a failed transaction rather than a hang.
bool PortIoTransport::waitFor(uint8_t mask, uint8_t value) {
    for (int i = 0; i < pollBudget_; ++i) {
        if ((io_->inb(kCommandPort) & mask) == value) return true;
        io_->idle();
    }
    return false;
}

// The SMC is ready to accept a byte once it has dropped the input-closed bit.
bool PortIoTransport::waitIdle() { return waitFor(kStatusInputClosed, 0); }

// If the controller is stuck mid-command, the Apple driver's documented
// recovery is to issue a read command, which resets the state machine.
bool PortIoTransport::sanitize() {
    if (waitFor(kStatusBusy, 0)) return true;
    uint8_t arg[4] = {'\0', '\0', '\0', '\0'};
    // Best effort: send a bare read command and wait again.
    if (waitIdle()) io_->outb(kCommandPort, kCommandRead);
    if (waitFor(kStatusBusy, 0)) return true;
    (void)arg;
    fail("SMC busy and did not recover");
    return false;
}

bool PortIoTransport::sendCommand(uint8_t command) {
    if (!waitIdle()) {
        fail("SMC ignored input while sending a command");
        return false;
    }
    io_->outb(kCommandPort, command);
    return true;
}

bool PortIoTransport::sendByte(uint8_t value, uint16_t port) {
    if (!waitIdle()) {
        fail("SMC ignored input while sending a byte");
        return false;
    }
    io_->outb(port, value);
    return true;
}

// Command, four argument bytes, length byte, then `length` response bytes.
bool PortIoTransport::readBlock(uint8_t command, const uint8_t arg[4], uint8_t length,
                                uint8_t* out) {
    if (!sanitize()) return false;
    if (!sendCommand(command)) return false;
    for (int i = 0; i < 4; ++i) {
        if (!sendByte(arg[i], kDataPort)) return false;
    }
    if (!sendByte(length, kDataPort)) return false;

    for (uint8_t i = 0; i < length; ++i) {
        if (!waitFor(kStatusAwaitingData, kStatusAwaitingData)) {
            fail("SMC did not return the expected data");
            return false;
        }
        out[i] = io_->inb(kDataPort);
    }

    // Newer SMCs return the whole key regardless of the requested length, so
    // drain whatever is left before releasing the controller.
    for (int i = 0; i < 16; ++i) {
        const uint8_t status = io_->inb(kCommandPort);
        if (!(status & kStatusAwaitingData)) break;
        (void)io_->inb(kDataPort);
    }
    if (!waitFor(kStatusBusy, 0)) {
        fail("SMC stayed busy after a read");
        return false;
    }
    return true;
}

bool PortIoTransport::writeBlock(uint8_t command, const uint8_t arg[4], const uint8_t* data,
                                 uint8_t length) {
    if (!sanitize()) return false;
    if (!sendCommand(command)) return false;
    for (int i = 0; i < 4; ++i) {
        if (!sendByte(arg[i], kDataPort)) return false;
    }
    if (!sendByte(length, kDataPort)) return false;
    for (uint8_t i = 0; i < length; ++i) {
        if (!sendByte(data[i], kDataPort)) return false;
    }
    if (!waitFor(kStatusBusy, 0)) {
        fail("SMC stayed busy after a write");
        return false;
    }
    return true;
}

bool PortIoTransport::readKey(Key key, uint8_t size, Value& out) {
    if (!io_) return false;
    TransactionGuard guard(lock_);
    if (!guard.held()) {
        fail("another program is using the SMC");
        return false;
    }
    if (size == 0 || size > kMaxValueBytes) {
        fail("read of an implausible size");
        return false;
    }
    uint8_t arg[4];
    std::memcpy(arg, &key.raw, 4);
    uint8_t buffer[kMaxValueBytes] = {};
    if (!readBlock(kCommandRead, arg, size, buffer)) return false;
    std::memcpy(out.data, buffer, size);
    out.size = size;
    return true;
}

bool PortIoTransport::writeKey(Key key, const uint8_t* data, uint8_t size) {
    if (!io_ || !data) return false;
    TransactionGuard guard(lock_);
    if (!guard.held()) {
        fail("another program is using the SMC");
        return false;
    }
    if (size == 0 || size > kMaxValueBytes) {
        fail("write of an implausible size");
        return false;
    }
    uint8_t arg[4];
    std::memcpy(arg, &key.raw, 4);
    return writeBlock(kCommandWrite, arg, data, size);
}

bool PortIoTransport::keyInfo(Key key, KeyInfo& out) {
    if (!io_) return false;
    TransactionGuard guard(lock_);
    if (!guard.held()) {
        fail("another program is using the SMC");
        return false;
    }
    uint8_t arg[4];
    std::memcpy(arg, &key.raw, 4);
    uint8_t info[6] = {};
    if (!readBlock(kCommandKeyType, arg, sizeof info, info)) return false;
    out.dataSize = info[0];
    std::memcpy(&out.dataType, info + 1, 4);
    out.dataAttributes = info[5];
    return true;
}

bool PortIoTransport::keyByIndex(uint32_t index, Key& out) {
    if (!io_) return false;
    TransactionGuard guard(lock_);
    if (!guard.held()) {
        fail("another program is using the SMC");
        return false;
    }
    // The index travels as a big-endian u32 in the argument slot.
    uint8_t arg[4];
    bigEndianWrite(arg, 4, index);
    uint8_t keyBytes[4] = {};
    if (!readBlock(kCommandKeyByIndex, arg, sizeof keyBytes, keyBytes)) return false;
    std::memcpy(&out.raw, keyBytes, 4);
    return true;
}

}  // namespace fanforge
