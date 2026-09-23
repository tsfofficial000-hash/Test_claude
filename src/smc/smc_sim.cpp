#include "smc/smc_sim.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "smc/port_io_transport.h"

namespace fanforge {
namespace {

void keyChars(Key k, uint8_t out[4]) { std::memcpy(out, &k.raw, 4); }

// The SMC enumerates its keys in byte order, so the simulation does too.
int compareKeys(const uint8_t a[4], const uint8_t b[4]) {
    for (int i = 0; i < 4; ++i) {
        if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1;
    }
    return 0;
}

int compareKeys(Key a, Key b) {
    uint8_t x[4], y[4];
    keyChars(a, x);
    keyChars(b, y);
    return compareKeys(x, y);
}

}  // namespace

// ---------------------------------------------------------------------------
// SmcModel
// ---------------------------------------------------------------------------

void SmcModel::insertSorted(SimKey entry) {
    auto it = keys_.begin();
    while (it != keys_.end() && compareKeys(it->key, entry.key) < 0) ++it;
    if (it != keys_.end() && it->key == entry.key) {
        *it = entry;
        return;
    }
    keys_.insert(it, entry);
}

void SmcModel::add(Key key, Key type, uint8_t attributes, const uint8_t* data, uint8_t size) {
    SimKey entry;
    entry.key = key;
    entry.type = type;
    entry.attributes = attributes;
    entry.size = size > kMaxValueBytes ? static_cast<uint8_t>(kMaxValueBytes) : size;
    if (data && entry.size) std::memcpy(entry.data, data, entry.size);
    insertSorted(entry);
}

void SmcModel::addU8(const char* key, uint8_t v) {
    add(Key(key), Key("ui8"), kAttrReadable, &v, 1);
}

void SmcModel::addU16(const char* key, uint16_t v) {
    uint8_t b[2];
    bigEndianWrite(b, 2, v);
    add(Key(key), Key("ui16"), kAttrReadable | kAttrWritable, b, 2);
}

void SmcModel::addU32(const char* key, uint32_t v) {
    uint8_t b[4];
    bigEndianWrite(b, 4, v);
    add(Key(key), Key("ui32"), kAttrReadable, b, 4);
}

void SmcModel::addFlag(const char* key, bool v) {
    uint8_t b = v ? 1 : 0;
    add(Key(key), Key("flag"), kAttrReadable, &b, 1);
}

void SmcModel::addString(const char* key, const std::string& text, uint8_t size) {
    uint8_t b[kMaxValueBytes] = {};
    const uint8_t n = size > kMaxValueBytes ? static_cast<uint8_t>(kMaxValueBytes) : size;
    for (uint8_t i = 0; i < n; ++i) b[i] = i < text.size() ? static_cast<uint8_t>(text[i]) : ' ';
    add(Key(key), Key("ch8*"), kAttrReadable, b, n);
}

// fpe2: unsigned fixed point, two fractional bits -> rpm * 4 as a big-endian u16.
void SmcModel::addFpe2(const char* key, double rpm) {
    const double scaled = std::round(rpm * 4.0);
    const uint16_t raw = scaled < 0 ? 0 : static_cast<uint16_t>(scaled > 65535 ? 65535 : scaled);
    uint8_t b[2];
    bigEndianWrite(b, 2, raw);
    add(Key(key), Key("fpe2"), kAttrReadable, b, 2);
}

// sp78: signed fixed point, eight fractional bits -> celsius * 256 as a
// big-endian s16. This is the SMC's temperature type.
void SmcModel::addSp78(const char* key, double celsius) {
    const double scaled = std::round(celsius * 256.0);
    const double clamped = scaled < -32768.0 ? -32768.0 : (scaled > 32767.0 ? 32767.0 : scaled);
    const int16_t raw = static_cast<int16_t>(clamped);
    uint8_t b[2];
    bigEndianWrite(b, 2, static_cast<uint16_t>(raw));
    add(Key(key), Key("sp78"), kAttrReadable, b, 2);
}

void SmcModel::addFan(int index, const std::string& label, double minimum, double maximum,
                      double actual, double target) {
    const std::string n = std::to_string(index);
    addString(("F" + n + "ID").c_str(), label, 16);

    auto addSpeed = [&](const char* suffix, double value, bool writable) {
        const std::string name = "F" + n + suffix;
        addFpe2(name.c_str(), value);
        if (writable) {
            SimKey* k = find(Key(name.c_str()));
            if (k) k->attributes = kAttrReadable | kAttrWritable;
        }
    };
    addSpeed("Ac", actual, false);
    addSpeed("Mn", minimum, true);
    addSpeed("Mx", maximum, false);
    addSpeed("Sf", maximum, false);
    addSpeed("Tg", target, true);
}

void SmcModel::setFanCount(uint8_t count) { addU8("FNum", count); }

void SmcModel::refreshKeyCount() {
    // #KEY must report the number of keys the enumeration will yield, itself
    // included.
    if (!contains("#KEY")) addU32("#KEY", 0);
    uint8_t b[4];
    bigEndianWrite(b, 4, static_cast<uint32_t>(keys_.size()));
    if (SimKey* k = find(Key("#KEY"))) std::memcpy(k->data, b, 4);
}

SmcModel SmcModel::macBookProTwoFans() {
    SmcModel m;

    m.setFanCount(2);
    m.addFan(0, "Left side ", 2160, 5927, 3552, 3553);
    m.addFan(1, "Right side", 2000, 5489, 3284, 3290);

    // "FS! " is the firmware's own manual-force bitmask, one bit per fan.
    {
        uint16_t mask = 0;
        uint8_t b[2];
        bigEndianWrite(b, 2, mask);
        m.add(Key("FS! "), Key("ui16"), kAttrReadable | kAttrWritable, b, 2);
    }

    // CPU cluster.
    m.addSp78("TC0P", 45.5);   // CPU proximity
    m.addSp78("TC0D", 52.25);  // CPU die
    m.addSp78("TC0E", 50.0);
    m.addSp78("TC0F", 48.75);
    m.addSp78("TC0H", 47.5);   // CPU heatsink
    m.addSp78("TCXC", 49.0);
    // GPU.
    m.addSp78("TG0P", 44.0);
    m.addSp78("TG0D", 46.5);
    m.addSp78("TG0H", 43.25);
    // Enclosure and battery.
    m.addSp78("TB0T", 31.0);
    m.addSp78("Ts0P", 29.5);
    m.addSp78("Ts0S", 30.75);
    m.addSp78("Th0H", 40.0);
    m.addSp78("Tm0P", 33.5);
    // A sensor that exists but reports the SMC's dead-value sentinel. It must
    // be filtered out of the UI.
    m.addSp78("TS2P", -127.0);

    // Non-temperature keys, so the type decoder sees every family on real data.
    m.addU8("FNum", 2);
    m.addString("REV ", "1.30", 4);
    m.addString("BVER", "0.0", 4);
    m.addU16("VC0C", 5000);  // millivolts
    {
        // A little-endian IEEE-754 float, the one non-big-endian type.
        const float watts = 12.5f;
        uint8_t b[4];
        std::memcpy(b, &watts, 4);
        m.add(Key("PCPC"), Key("flt"), kAttrReadable, b, 4);
    }

    m.addU32("#KEY", 0);
    m.refreshKeyCount();
    return m;
}

SimKey* SmcModel::find(Key key) {
    for (auto& k : keys_) {
        if (k.key == key) return &k;
    }
    return nullptr;
}

const SimKey* SmcModel::find(Key key) const {
    for (const auto& k : keys_) {
        if (k.key == key) return &k;
    }
    return nullptr;
}

bool SmcModel::keyAt(uint32_t index, Key& out) const {
    if (index >= keys_.size()) return false;
    out = keys_[index].key;
    return true;
}

bool SmcModel::info(Key key, KeyInfo& out) const {
    const SimKey* k = find(key);
    if (!k) return false;
    out.dataSize = k->size;
    out.dataType = k->type.raw;
    out.dataAttributes = k->attributes;
    return true;
}

bool SmcModel::read(Key key, uint8_t maxBytes, uint8_t* out, uint8_t& outSize) const {
    const SimKey* k = find(key);
    if (!k) return false;
    uint8_t n = k->size;
    if (n > maxBytes) n = maxBytes;
    if (out && n) std::memcpy(out, k->data, n);
    outSize = n;
    return true;
}

bool SmcModel::write(Key key, const uint8_t* data, uint8_t size) {
    SimKey* k = find(key);
    if (!k || !data) return false;
    if (!(k->attributes & kAttrWritable)) return false;
    if (size != k->size) return false;
    std::memcpy(k->data, data, size);
    return true;
}

std::optional<double> SmcModel::valueOf(const char* key) const {
    const SimKey* k = find(Key(key));
    if (!k) return std::nullopt;
    return decodeValue(k->type, k->data, k->size);
}

uint32_t SmcModel::u32Of(const char* key) const {
    const SimKey* k = find(Key(key));
    if (!k) return 0;
    return static_cast<uint32_t>(bigEndianRead(k->data, static_cast<int>(k->size < 4 ? k->size : 4)));
}

bool SmcModel::contains(const char* key) const { return find(Key(key)) != nullptr; }

// ---------------------------------------------------------------------------
// SimulatedSmcPortIo - the SMC register state machine
// ---------------------------------------------------------------------------

uint8_t SimulatedSmcPortIo::status() const {
    if (model_->stopAcceptingInput) {
        return static_cast<uint8_t>(smc_port::kStatusInputClosed |
                                    (busy_ ? smc_port::kStatusBusy : 0));
    }
    return static_cast<uint8_t>((awaitingData_ ? smc_port::kStatusAwaitingData : 0) |
                                (busy_ ? smc_port::kStatusBusy : 0));
}

void SimulatedSmcPortIo::beginCommand(uint8_t command) {
    command_ = command;
    ++commandsSeen;
    input_.clear();
    output_.clear();
    outputPos_ = 0;
    awaitingData_ = false;
    busy_ = true;
}

void SimulatedSmcPortIo::finishRequest() {
    awaitingData_ = false;
    busy_ = false;
    output_.clear();
    outputPos_ = 0;
}

void SimulatedSmcPortIo::startRequest(uint8_t length) {
    length_ = length;
    uint32_t arg = 0;
    std::memcpy(&arg, input_.data(), 4);
    const Key argKey(arg);

    switch (command_) {
        case smc_port::kCommandRead: {
            ++model_->readCount;
            if (model_->failNextReads > 0) {
                --model_->failNextReads;
                finishRequest();
                return;
            }
            uint8_t buffer[kMaxValueBytes] = {};
            uint8_t n = 0;
            if (model_->read(argKey, kMaxValueBytes, buffer, n) && n > 0) {
                output_.assign(buffer, buffer + n);
                outputPos_ = 0;
                awaitingData_ = true;
                busy_ = true;
            } else {
                finishRequest();
            }
            return;
        }
        case smc_port::kCommandKeyType: {
            KeyInfo info;
            if (model_->info(argKey, info)) {
                output_.clear();
                output_.push_back(info.dataSize);
                uint8_t t[4];
                std::memcpy(t, &info.dataType, 4);
                output_.insert(output_.end(), t, t + 4);
                output_.push_back(info.dataAttributes);
                outputPos_ = 0;
                awaitingData_ = true;
                busy_ = true;
            } else {
                finishRequest();
            }
            return;
        }
        case smc_port::kCommandKeyByIndex: {
            const uint32_t index = static_cast<uint32_t>(bigEndianRead(input_.data(), 4));
            Key found;
            if (model_->keyAt(index, found)) {
                uint8_t t[4];
                std::memcpy(t, &found.raw, 4);
                output_.assign(t, t + 4);
                outputPos_ = 0;
                awaitingData_ = true;
                busy_ = true;
            } else {
                finishRequest();
            }
            return;
        }
        case smc_port::kCommandWrite: {
            if (length_ == 0) {
                finishRequest();
            } else {
                // Wait for the payload bytes.
                awaitingData_ = false;
                busy_ = true;
            }
            return;
        }
        default:
            finishRequest();
            return;
    }
}

void SimulatedSmcPortIo::consumeInputByte(uint8_t value) {
    if (!busy_) return;
    input_.push_back(value);

    if (input_.size() == 5) {
        startRequest(input_[4]);
        return;
    }
    if (command_ == smc_port::kCommandWrite && input_.size() == static_cast<size_t>(5) + length_) {
        uint32_t raw = 0;
        std::memcpy(&raw, input_.data(), 4);
        ++model_->writeCount;
        if (model_->failNextWrites > 0) {
            --model_->failNextWrites;
        } else {
            model_->write(Key(raw), input_.data() + 5, length_);
        }
        finishRequest();
    }
}

void SimulatedSmcPortIo::outb(uint16_t port, uint8_t value) {
    if (port == smc_port::kCommandPort) {
        beginCommand(value);
    } else if (port == smc_port::kDataPort) {
        consumeInputByte(value);
    }
}

uint8_t SimulatedSmcPortIo::inb(uint16_t port) {
    if (port == smc_port::kCommandPort) return status();
    if (port != smc_port::kDataPort) return 0;

    if (outputPos_ < output_.size()) {
        const uint8_t v = output_[outputPos_++];
        if (outputPos_ >= output_.size()) {
            // Last byte served: the controller is idle again.
            awaitingData_ = false;
            busy_ = false;
        }
        return v;
    }
    return 0;
}

// ---------------------------------------------------------------------------
// SimulatedKernelDevice
// ---------------------------------------------------------------------------

bool SimulatedKernelDevice::ioctl(uint32_t code, const void* in, uint32_t inLen, void* out,
                                 uint32_t outCap, uint32_t* bytesReturned) {
    if (!open_) {
        error_ = "device not open";
        return false;
    }
    if (bytesReturned) *bytesReturned = 0;

    switch (code) {
        case applesmc::kIoctlReadKey: {
            if (!in || inLen < 5) {
                error_ = "short read request";
                return false;
            }
            uint32_t raw = 0;
            std::memcpy(&raw, in, 4);
            ++model_->readCount;
            if (model_->failNextReads > 0) {
                --model_->failNextReads;
                error_ = "no data";
                return false;
            }
            uint8_t buffer[kMaxValueBytes] = {};
            uint8_t n = 0;
            if (!model_->read(Key(raw), kMaxValueBytes, buffer, n) || n == 0) {
                error_ = "unknown key";
                return false;
            }
            uint32_t copied = n;
            if (copied > outCap) copied = outCap;
            if (out && copied) std::memcpy(out, buffer, copied);
            if (bytesReturned) *bytesReturned = copied;
            return true;
        }
        case applesmc::kIoctlWriteKey: {
            if (!in || inLen < 5) {
                error_ = "short write request";
                return false;
            }
            uint32_t raw = 0;
            std::memcpy(&raw, in, 4);
            const uint8_t size = static_cast<const uint8_t*>(in)[4];
            if (inLen < 5u + size) {
                error_ = "truncated write payload";
                return false;
            }
            ++model_->writeCount;
            if (model_->failNextWrites > 0) {
                --model_->failNextWrites;
            } else if (!model_->write(Key(raw), static_cast<const uint8_t*>(in) + 5, size)) {
                error_ = "write rejected";
                return false;
            }
            if (outCap >= 1) {
                if (out) *static_cast<uint8_t*>(out) = 0;
                if (bytesReturned) *bytesReturned = 1;
            }
            return true;
        }
        case applesmc::kIoctlKeyByIndex: {
            if (!in || inLen < 4 || outCap < 4) {
                error_ = "bad index request";
                return false;
            }
            uint32_t index = 0;
            std::memcpy(&index, in, 4);
            Key found;
            if (!model_->keyAt(index, found)) {
                error_ = "index past the end of the key table";
                return false;
            }
            uint8_t t[4];
            std::memcpy(t, &found.raw, 4);
            if (out) std::memcpy(out, t, 4);
            if (bytesReturned) *bytesReturned = 4;
            return true;
        }
        case applesmc::kIoctlKeyInfo: {
            if (!in || inLen < 4 || outCap < 6) {
                error_ = "bad key-info request";
                return false;
            }
            uint32_t raw = 0;
            std::memcpy(&raw, in, 4);
            KeyInfo info;
            if (!model_->info(Key(raw), info)) {
                error_ = "unknown key";
                return false;
            }
            uint8_t b[6] = {info.dataSize};
            std::memcpy(b + 1, &info.dataType, 4);
            b[5] = info.dataAttributes;
            if (out) std::memcpy(out, b, 6);
            if (bytesReturned) *bytesReturned = 6;
            return true;
        }
        case applesmc::kIoctlGetProtocol: {
            if (outCap < 1) {
                error_ = "no room for the protocol byte";
                return false;
            }
            if (out) *static_cast<uint8_t*>(out) = 1;
            if (bytesReturned) *bytesReturned = 1;
            return true;
        }
        default:
            error_ = "unsupported control code";
            return false;
    }
}

}  // namespace fanforge
