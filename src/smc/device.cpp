#include "smc/device.h"

#include <cstring>

namespace fanforge {

bool SmcDevice::open() {
    if (!transport_) return false;
    infoCache_.clear();
    return transport_->open();
}

void SmcDevice::close() {
    if (!transport_) return;
    transport_->close();
    infoCache_.clear();
}

bool SmcDevice::keyInfo(Key key, KeyInfo& out) const {
    if (!transport_) return false;
    if (!transport_->isOpen()) return false;

    const auto it = infoCache_.find(key.raw);
    if (it != infoCache_.end()) {
        out = it->second;
        return true;
    }
    KeyInfo info;
    if (!transport_->keyInfo(key, info)) return false;
    infoCache_.emplace(key.raw, info);
    out = info;
    return true;
}

bool SmcDevice::hasKey(Key key) const {
    KeyInfo info;
    return keyInfo(key, info);
}

bool SmcDevice::isWritable(Key key) const {
    KeyInfo info;
    return keyInfo(key, info) && info.writable();
}

uint32_t SmcDevice::keyCount() const {
    Value v;
    if (!read(Key("#KEY"), v)) return 0;
    if (v.size != 4) return 0;
    return v.toU32();
}

std::vector<Key> SmcDevice::allKeys() const {
    std::vector<Key> keys;
    if (!transport_ || !transport_->isOpen()) return keys;
    const uint32_t n = keyCount();
    // A plausible upper bound: the SMC reports a few hundred to a couple of
    // thousand keys. Anything outside that is a misread, not a big machine.
    if (n == 0 || n > 8192) return keys;
    keys.reserve(n);
    for (uint32_t i = 0; i < n; ++i) {
        Key key;
        if (transport_->keyByIndex(i, key)) keys.push_back(key);
    }
    return keys;
}

bool SmcDevice::read(Key key, Value& out) const {
    if (!transport_ || !transport_->isOpen()) return false;

    KeyInfo info;
    if (!keyInfo(key, info)) return false;
    if (!info.plausiblySane()) return false;

    out.type = Key(info.dataType);
    out.size = 0;
    return transport_->readKey(key, info.dataSize, out);
}

bool SmcDevice::readNumber(Key key, double& out) const {
    Value v;
    if (!read(key, v)) return false;
    const auto decoded = v.toDouble();
    if (!decoded.has_value()) return false;
    out = *decoded;
    return true;
}

std::optional<double> SmcDevice::number(Key key) const {
    double value = 0;
    if (!readNumber(key, value)) return std::nullopt;
    return value;
}

bool SmcDevice::readU32(Key key, uint32_t& out) const {
    Value v;
    if (!read(key, v) || v.size == 0) return false;
    out = v.toU32();
    return true;
}

bool SmcDevice::readString(Key key, std::string& out) const {
    Value v;
    if (!read(key, v)) return false;
    out = v.toText();
    return true;
}

bool SmcDevice::writeNumber(Key key, double value) const {
    if (!transport_ || !transport_->isOpen()) return false;

    KeyInfo info;
    if (!keyInfo(key, info)) return false;
    if (!info.writable()) return false;
    if (!info.plausiblySane()) return false;

    uint8_t payload[kMaxValueBytes] = {};
    const uint32_t written = encodeValue(Key(info.dataType), value, payload, sizeof payload);
    // A payload that does not match the declared size means the type is one we
    // cannot encode; sending a short or long payload would corrupt the key.
    if (written == 0 || written != info.dataSize) return false;

    return transport_->writeKey(key, payload, static_cast<uint8_t>(written));
}

bool SmcDevice::writeNumberVerified(Key key, double value, double tolerance) const {
    if (!writeNumber(key, value)) return false;
    double readback = 0;
    if (!readNumber(key, readback)) return false;
    const double delta = readback - value;
    return (delta < 0 ? -delta : delta) <= tolerance;
}

std::string SmcDevice::fanLabel(int index) const {
    const Key idKey = makeFanKey(index, "ID");
    std::string label;
    if (readString(idKey, label)) {
        // Strip anything non-printable that survived, and reject a label that
        // is only punctuation from an all-zero key.
        bool hasLetter = false;
        for (char c : label) {
            if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) hasLetter = true;
        }
        if (hasLetter) return label;
    }
    return "Fan " + std::to_string(index);
}

}  // namespace fanforge
