#pragma once
//
// The seam to the Windows kernel device that publishes SMC access
// (\\.\APPLESMC, backed by the Boot Camp "AppleSMC" service).
//
// Kept behind a one-method interface so the IOCTL protocol logic is ordinary
// portable C++ and gets the same test coverage as the port path.
//
#include <cstdint>
#include <string>

namespace fanforge {

class IKernelDevice {
public:
    virtual ~IKernelDevice() = default;

    // value == DeviceIoControl on Windows.
    virtual bool ioctl(uint32_t code, const void* in, uint32_t inLen, void* out,
                       uint32_t outCap, uint32_t* bytesReturned) = 0;

    virtual bool isOpen() const = 0;
    virtual std::string lastError() const = 0;
};

// The IOCTL codes and payload shapes used by the Apple SMC device over Boot
// Camp. Confirmed on hardware in the reference implementation (see README).
namespace applesmc {
constexpr uint32_t kIoctlReadKey = 0x220000;      // in {u32 key; u8 size} -> <=32 bytes
constexpr uint32_t kIoctlWriteKey = 0x220004;     // in {u32 key; u8 size; u8 data[]} -> 1 byte
constexpr uint32_t kIoctlKeyByIndex = 0x220008;   // in u32 index -> 4 bytes
constexpr uint32_t kIoctlKeyInfo = 0x22000C;      // in u32 key -> 6 bytes
constexpr uint32_t kIoctlGetProtocol = 0x220020;  // -> 1 byte (1 = mmio)
}  // namespace applesmc

}  // namespace fanforge
