#pragma once
//
// The seam between the SMC protocol logic and the operating system.
//
// Everything above this interface is portable C++ and is covered by the test
// suite on any platform. Everything below it is a thin shim: a device handle,
// or two I/O port reads. That is deliberate - it is the only part that cannot
// be unit tested, so it is kept as small as it can possibly be.
//
#include <cstdint>
#include <string>

#include "smc/key.h"

namespace fanforge {

class ISmcTransport {
public:
    virtual ~ISmcTransport() = default;

    // Short identifier for diagnostics, e.g. "applesmc" or "port-io".
    virtual const char* name() const = 0;

    virtual bool isOpen() const = 0;
    virtual bool open() { return isOpen(); }
    virtual void close() {}

    // Human-readable reason for the last failure.
    virtual std::string lastError() const { return {}; }

    // Reads `size` bytes of `key` into `out->data`. `out->type` is not set here
    // (the caller already knows it from keyInfo); SmcDevice fills it in.
    virtual bool readKey(Key key, uint8_t size, Value& out) = 0;

    // Writes exactly `size` bytes to `key`.
    //
    // Returns true when the transport carried the write to the controller, NOT
    // when the controller accepted it. The port path has no acknowledgement at
    // all, so a rejected write still reports success there. Callers that need
    // certainty must read the key back - SmcDevice::writeNumberVerified does.
    virtual bool writeKey(Key key, const uint8_t* data, uint8_t size) = 0;

    // Size, type and access flags the SMC declares for `key`.
    virtual bool keyInfo(Key key, KeyInfo& out) = 0;

    // Maps a 0-based index onto a key. Returns false past the end.
    virtual bool keyByIndex(uint32_t index, Key& out) = 0;
};

// Byte-level port I/O. Implemented on Windows over InpOut32/WinRing0, and in
// tests by a simulated SMC chip.
class IByteIo {
public:
    virtual ~IByteIo() = default;
    virtual uint8_t inb(uint16_t port) = 0;
    virtual void outb(uint16_t port, uint8_t value) = 0;

    // Called between polls while waiting on the status register. The real
    // Windows shim yields the CPU here; tests leave it empty.
    virtual void idle() {}
};

// Serialises a whole SMC transaction.
//
// Two programs poking the SMC at the same time interleave their register
// traffic and read each other's answers, which surfaces as implausible
// readings rather than as an error. The Windows implementation takes the named
// mutex "Global\AppleSmcAccess", which is the name the other Boot Camp fan
// tools publish for exactly this purpose, so they cooperate instead of
// colliding. Where the name cannot be created the lock degrades to nothing,
// which is what those tools do too.
//
// The unit of exclusion is the transaction, not the byte: a lock taken per byte
// would still let another program slip in between a command and its arguments.
class ITransactionLock {
public:
    virtual ~ITransactionLock() = default;

    // Blocks for a bounded time. False means "someone else is mid-transaction"
    // - the caller fails its own transaction rather than interleaving with it.
    virtual bool acquire() = 0;
    virtual void release() = 0;
};

// Holds the lock for exactly one transaction, and releases it on every path
// out, including the early returns a failed read takes.
class TransactionGuard {
public:
    explicit TransactionGuard(ITransactionLock* lock) : lock_(lock) {
        if (lock_) held_ = lock_->acquire();
    }
    ~TransactionGuard() {
        if (held_) lock_->release();
    }
    TransactionGuard(const TransactionGuard&) = delete;
    TransactionGuard& operator=(const TransactionGuard&) = delete;

    // True when the transaction may proceed (no lock configured counts as yes).
    bool held() const { return lock_ == nullptr || held_; }

private:
    ITransactionLock* lock_;
    bool held_ = false;
};

}  // namespace fanforge
