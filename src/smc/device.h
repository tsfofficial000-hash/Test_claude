#pragma once
//
// The high-level SMC API. Everything above this is policy; nothing here is
// OS aware.
//
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "smc/key.h"
#include "smc/transport.h"

namespace fanforge {

class SmcDevice {
public:
    explicit SmcDevice(ISmcTransport& transport) : transport_(&transport) {}

    bool isOpen() const { return transport_ && transport_->isOpen(); }
    bool open();
    void close();

    const char* transportName() const { return transport_ ? transport_->name() : "none"; }
    std::string lastError() const { return transport_ ? transport_->lastError() : "no transport"; }
    ISmcTransport& transport() { return *transport_; }

    // -- metadata ----------------------------------------------------------
    bool keyInfo(Key key, KeyInfo& out) const;
    bool hasKey(Key key) const;
    bool isWritable(Key key) const;
    uint32_t keyCount() const;
    std::vector<Key> allKeys() const;

    // -- reads -------------------------------------------------------------
    // Fills `out.type` from the key's declared type, then reads its bytes.
    bool read(Key key, Value& out) const;
    bool readNumber(Key key, double& out) const;
    std::optional<double> number(Key key) const;
    bool readU32(Key key, uint32_t& out) const;
    bool readString(Key key, std::string& out) const;

    // -- writes ------------------------------------------------------------
    // Encodes `value` using the key's declared type. Refuses to write a key the
    // controller does not declare writable, and refuses to write a payload
    // whose length does not match the declared size.
    bool writeNumber(Key key, double value) const;

    // Writes then reads back and compares. The port transport cannot report a
    // rejected write, so this is the only honest confirmation available.
    bool writeNumberVerified(Key key, double value, double tolerance) const;

    // -- convenience -------------------------------------------------------
    // "Left side" for F0ID, or "Fan 0" when the label is unusable.
    std::string fanLabel(int index) const;

    void clearCache() { infoCache_.clear(); }

private:
    ISmcTransport* transport_;
    mutable std::map<uint32_t, KeyInfo> infoCache_;
    mutable bool infoCacheValid_ = false;
};

}  // namespace fanforge
