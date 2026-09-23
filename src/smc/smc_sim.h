#pragma once
//
// A simulated SMC: a key/value table plus the two access paths a real Mac
// exposes (the port state machine, and the device/IOCTL interface).
//
// This is the oracle for the whole control loop. The port simulation
// reproduces the register-level handshake the hardware performs - status bits,
// command bytes, the four-byte key argument, the length byte, the trailing
// flush - so PortIoTransport is exercised as a protocol implementation, not
// stubbed out.
//
// Values are hand-encoded here rather than reusing the library's encoder, so a
// bug in one cannot mask a bug in the other.
//
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "smc/key.h"
#include "smc/kernel_device.h"
#include "smc/transport.h"

namespace fanforge {

struct SimKey {
    Key key;
    Key type;
    uint8_t attributes = kAttrReadable;
    uint8_t size = 0;
    uint8_t data[kMaxValueBytes] = {};
};

// ---------------------------------------------------------------------------
// The key table.
// ---------------------------------------------------------------------------
class SmcModel {
public:
    void add(Key key, Key type, uint8_t attributes, const uint8_t* data, uint8_t size);

    void addU8(const char* key, uint8_t v);
    void addU16(const char* key, uint16_t v);
    void addU32(const char* key, uint32_t v);
    void addFlag(const char* key, bool v);
    void addString(const char* key, const std::string& text, uint8_t size);
    void addFpe2(const char* key, double rpm);       // fan speed family
    void addSp78(const char* key, double celsius);   // temperature family

    // Adds F<n>ID / Ac / Mn / Mx / Sf / Tg for one fan.
    void addFan(int index, const std::string& label, double minimum, double maximum,
                double actual, double target);

    // FNum.
    void setFanCount(uint8_t count);

    // Fills the table with a plausible two-fan MacBook Pro: fan keys, fan
    // count, a set of real temperature keys, and the FS! force mask.
    static SmcModel macBookProTwoFans();

    // --- reads used by the transports ---
    size_t size() const { return keys_.size(); }
    bool keyAt(uint32_t index, Key& out) const;
    bool info(Key key, KeyInfo& out) const;
    bool read(Key key, uint8_t maxBytes, uint8_t* out, uint8_t& outSize) const;
    bool write(Key key, const uint8_t* data, uint8_t size);

    // --- helpers for assertions ---
    std::optional<double> valueOf(const char* key) const;
    uint32_t u32Of(const char* key) const;
    bool contains(const char* key) const;

    // Makes the #KEY key agree with the number of keys present.
    void refreshKeyCount();

    // --- fault injection ---
    int failNextReads = 0;    // reads answer with no data, as if unknown
    int failNextWrites = 0;   // writes are swallowed
    bool stopAcceptingInput = false;  // input-closed bit never clears

    int readCount = 0;
    int writeCount = 0;

private:
    SimKey* find(Key key);
    const SimKey* find(Key key) const;
    void insertSorted(SimKey entry);
    std::vector<SimKey> keys_;
};

// ---------------------------------------------------------------------------
// Port-level simulation: implements the SMC's register state machine.
// ---------------------------------------------------------------------------
class SimulatedSmcPortIo final : public IByteIo {
public:
    explicit SimulatedSmcPortIo(SmcModel& model) : model_(&model) {}

    uint8_t inb(uint16_t port) override;
    void outb(uint16_t port, uint8_t value) override;
    void idle() override { ++idlePolls; }

    // Diagnostics.
    int idlePolls = 0;
    int commandsSeen = 0;

private:
    uint8_t status() const;
    void beginCommand(uint8_t command);
    void consumeInputByte(uint8_t value);
    void startRequest(uint8_t length);
    void finishRequest();

    SmcModel* model_;
    uint8_t command_ = 0;
    uint8_t length_ = 0;
    std::vector<uint8_t> input_;
    std::vector<uint8_t> output_;
    size_t outputPos_ = 0;
    bool awaitingData_ = false;
    bool busy_ = false;
};

// ---------------------------------------------------------------------------
// Device-level simulation: the same table behind an IOCTL-style interface.
// ---------------------------------------------------------------------------
class SimulatedKernelDevice final : public IKernelDevice {
public:
    explicit SimulatedKernelDevice(SmcModel& model) : model_(&model) {}

    bool ioctl(uint32_t code, const void* in, uint32_t inLen, void* out, uint32_t outCap,
               uint32_t* bytesReturned) override;
    bool isOpen() const override { return open_; }
    void setOpen(bool v) { open_ = v; }
    std::string lastError() const override { return error_; }

private:
    SmcModel* model_;
    bool open_ = true;
    std::string error_;
};

}  // namespace fanforge
