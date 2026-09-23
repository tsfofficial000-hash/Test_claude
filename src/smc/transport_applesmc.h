#pragma once
//
// The Apple SMC device that Boot Camp's "AppleSMC" service publishes as
// \\.\APPLESMC. This is the preferred path: it needs no driver of our own, and
// the driver serialises access for us.
//
// The device hands out a single handle at a time, so only one SMC program can
// run at once - whichever transport is used.
//
#include <cstdint>
#include <string>

#include "smc/kernel_device.h"
#include "smc/transport.h"

namespace fanforge {

class ApplesmcTransport final : public ISmcTransport {
public:
    explicit ApplesmcTransport(IKernelDevice& device) : device_(&device) {}

    const char* name() const override { return "applesmc"; }
    bool isOpen() const override { return device_ != nullptr && device_->isOpen(); }
    std::string lastError() const override;

    bool readKey(Key key, uint8_t size, Value& out) override;
    bool writeKey(Key key, const uint8_t* data, uint8_t size) override;
    bool keyInfo(Key key, KeyInfo& out) override;
    bool keyByIndex(uint32_t index, Key& out) override;

    // 1 = mmio, otherwise pmio; reported by the driver.
    uint8_t protocol() const;

    // Optional cross-process exclusion, shared with the port path so that a
    // tool using the other transport cannot interleave with this one.
    void setTransactionLock(ITransactionLock* lock) { lock_ = lock; }
    ITransactionLock* transactionLock() const { return lock_; }

private:
    IKernelDevice* device_;
    ITransactionLock* lock_ = nullptr;
};

}  // namespace fanforge
