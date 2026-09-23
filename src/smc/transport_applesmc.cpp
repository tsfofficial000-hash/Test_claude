#include "smc/transport_applesmc.h"

#include <cstring>

namespace fanforge {
namespace {

#pragma pack(push, 1)
struct ReadRequest {
    uint32_t key;
    uint8_t size;
};
struct WriteRequest {
    uint32_t key;
    uint8_t size;
    uint8_t data[kMaxValueBytes];
};
#pragma pack(pop)

static_assert(sizeof(ReadRequest) == 5, "APPLESMC read request is 5 bytes");
static_assert(sizeof(WriteRequest) == 5 + kMaxValueBytes, "write request is key + size + payload");

}  // namespace

std::string ApplesmcTransport::lastError() const {
    if (!device_) return "no SMC device";
    // Say plainly which failure this is; an empty string here would leave the
    // UI with nothing to show the user.
    if (!device_->isOpen()) return "the SMC device is not open";
    const std::string detail = device_->lastError();
    return detail.empty() ? std::string("the SMC device rejected the request") : detail;
}

uint8_t ApplesmcTransport::protocol() const {
    if (!device_ || !device_->isOpen()) return 0;
    uint8_t value = 0;
    uint32_t returned = 0;
    if (!device_->ioctl(applesmc::kIoctlGetProtocol, nullptr, 0, &value, 1, &returned)) return 0;
    if (returned != 1) return 0;
    return value;
}

bool ApplesmcTransport::readKey(Key key, uint8_t size, Value& out) {
    if (!device_ || !device_->isOpen()) return false;
    TransactionGuard guard(lock_);
    if (!guard.held()) return false;
    if (size == 0 || size > kMaxValueBytes) return false;

    ReadRequest request{key.raw, size};
    uint8_t buffer[kMaxValueBytes] = {};
    uint32_t returned = 0;
    if (!device_->ioctl(applesmc::kIoctlReadKey, &request, sizeof request, buffer,
                        kMaxValueBytes, &returned)) {
        return false;
    }
    // A driver that does not know the key reports success with no bytes.
    if (returned == 0 || returned > kMaxValueBytes) return false;

    const uint32_t copied = returned < size ? returned : size;
    std::memcpy(out.data, buffer, copied);
    out.size = copied;
    return true;
}

bool ApplesmcTransport::writeKey(Key key, const uint8_t* data, uint8_t size) {
    if (!device_ || !device_->isOpen() || !data) return false;
    TransactionGuard guard(lock_);
    if (!guard.held()) return false;
    if (size == 0 || size > kMaxValueBytes) return false;

    WriteRequest request{};
    request.key = key.raw;
    request.size = size;
    std::memcpy(request.data, data, size);

    uint8_t response = 0;
    uint32_t returned = 0;
    // The driver expects exactly the bytes present: 4 + 1 + size.
    if (!device_->ioctl(applesmc::kIoctlWriteKey, &request, 5u + size, &response, 1, &returned)) {
        return false;
    }
    return returned == 1;
}

bool ApplesmcTransport::keyInfo(Key key, KeyInfo& out) {
    if (!device_ || !device_->isOpen()) return false;
    TransactionGuard guard(lock_);
    if (!guard.held()) return false;

    const uint32_t raw = key.raw;
    uint8_t buffer[6] = {};
    uint32_t returned = 0;
    if (!device_->ioctl(applesmc::kIoctlKeyInfo, &raw, sizeof raw, buffer, sizeof buffer,
                        &returned)) {
        return false;
    }
    if (returned != sizeof buffer) return false;

    out.dataSize = buffer[0];
    std::memcpy(&out.dataType, buffer + 1, 4);
    out.dataAttributes = buffer[5];
    return true;
}

bool ApplesmcTransport::keyByIndex(uint32_t index, Key& out) {
    if (!device_ || !device_->isOpen()) return false;
    TransactionGuard guard(lock_);
    if (!guard.held()) return false;

    uint32_t raw = 0;
    uint32_t returned = 0;
    if (!device_->ioctl(applesmc::kIoctlKeyByIndex, &index, sizeof index, &raw, sizeof raw,
                        &returned)) {
        return false;
    }
    if (returned != sizeof raw) return false;

    out.raw = raw;
    return true;
}

}  // namespace fanforge
