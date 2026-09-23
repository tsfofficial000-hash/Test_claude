//
// The Windows side of everything. This is the only file in the project that
// knows the operating system exists, and it is deliberately small: two device
// shims, each only a few calls deep, sitting under interfaces that the test
// suite drives directly.
//
#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>

#include <memory>
#include <string>
#include <vector>

#include "platform/platform.h"
#include "smc/port_io_transport.h"
#include "smc/transport_applesmc.h"

namespace fanforge {
namespace {

// The cross-process SMC lock, taken under the name the other Boot Camp fan
// tools publish for this purpose. Two programs talking to the SMC at once
// interleave their register traffic and read each other's answers, which shows
// up as implausible readings rather than as an error; a shared name lets any
// tool that adopts it coexist with the rest instead of corrupting them.
//
// Best effort by design: if the name cannot be created, the lock does nothing
// and we carry on. Refusing to control the fans because an advisory
// cross-process lock was unavailable would be the worse failure.
class NamedSmcMutex final : public ITransactionLock {
public:
    NamedSmcMutex() {
        // Global first, so tools in other sessions are included; Local if the
        // privilege to create a global object is not held.
        handle_ = CreateMutexW(nullptr, FALSE, L"Global\\AppleSmcAccess");
        if (!handle_) handle_ = CreateMutexW(nullptr, FALSE, L"Local\\AppleSmcAccess");
    }

    ~NamedSmcMutex() override {
        if (handle_) CloseHandle(handle_);
    }

    bool acquire() override {
        if (!handle_) return true;  // no lock available: proceed unlocked
        const DWORD wait = WaitForSingleObject(handle_, 1000);
        // WAIT_ABANDONED means the previous holder died mid-transaction. The
        // SMC holds no state between transactions, so there is nothing to
        // repair and proceeding is correct.
        held_ = (wait == WAIT_OBJECT_0 || wait == WAIT_ABANDONED);
        return held_;
    }

    void release() override {
        if (handle_ && held_) {
            ReleaseMutex(handle_);
            held_ = false;
        }
    }

private:
    HANDLE handle_ = nullptr;
    bool held_ = false;
};

std::string wideToNarrow(const std::wstring& text) {
    if (text.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
                                         nullptr, 0, nullptr, nullptr);
    if (size <= 0) return {};
    std::string out(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), &out[0], size,
                        nullptr, nullptr);
    return out;
}

std::string windowsErrorText(DWORD code) {
    LPSTR buffer = nullptr;
    const DWORD length = FormatMessageA(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, code, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), reinterpret_cast<LPSTR>(&buffer),
        0, nullptr);
    std::string text;
    if (length && buffer) {
        text.assign(buffer, length);
        while (!text.empty() && (text.back() == '\r' || text.back() == '\n' || text.back() == ' ')) {
            text.pop_back();
        }
    }
    LocalFree(buffer);
    if (text.empty()) text = "Windows error " + std::to_string(code);
    return text;
}

// ---------------------------------------------------------------------------
// \\.\APPLESMC
// ---------------------------------------------------------------------------
class Win32KernelDevice final : public IKernelDevice {
public:
    ~Win32KernelDevice() override { close(); }

    bool open() {
        close();
        error_.clear();

        for (int attempt = 0; attempt < 2; ++attempt) {
            handle_ = CreateFileW(L"\\\\.\\APPLESMC", GENERIC_READ | GENERIC_WRITE,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                                  FILE_ATTRIBUTE_NORMAL, nullptr);
            if (handle_ != INVALID_HANDLE_VALUE) return true;

            const DWORD code = GetLastError();
            if (attempt == 0 && (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND)) {
                // The driver is installed but its service is stopped. Starting
                // it needs Administrator, so failure here is not fatal.
                if (startAppleSmcService()) continue;
            }

            if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND) {
                error_ =
                    "the AppleSMC driver is not installed or its service is stopped. It comes with "
                    "the Boot Camp support software; reinstalling that, or starting the AppleSMC "
                    "service, enables this path.";
            } else if (code == ERROR_ACCESS_DENIED || code == ERROR_SHARING_VIOLATION) {
                error_ =
                    "the device is already in use. It allows one program at a time, so close any "
                    "other SMC utility (including another copy of this one) and try again.";
            } else {
                error_ = "could not open the device: " + windowsErrorText(code);
            }
            return false;
        }
        return false;
    }

    void close() {
        if (handle_ != INVALID_HANDLE_VALUE) {
            CloseHandle(handle_);
            handle_ = INVALID_HANDLE_VALUE;
        }
    }

    bool isOpen() const override { return handle_ != INVALID_HANDLE_VALUE; }
    std::string lastError() const override { return error_; }

    bool ioctl(uint32_t code, const void* in, uint32_t inLength, void* out, uint32_t outCapacity,
               uint32_t* returned) override {
        if (!isOpen()) {
            error_ = "the device is not open";
            return false;
        }
        DWORD bytes = 0;
        const BOOL ok = DeviceIoControl(handle_, code, const_cast<void*>(in), inLength, out,
                                        outCapacity, &bytes, nullptr);
        if (returned) *returned = bytes;
        if (!ok) {
            error_ = "the SMC rejected the request: " + windowsErrorText(GetLastError());
            return false;
        }
        return true;
    }

private:
    static bool startAppleSmcService() {
        SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
        if (!manager) return false;

        bool running = false;
        if (SC_HANDLE service = OpenServiceW(manager, L"AppleSMC",
                                             SERVICE_QUERY_STATUS | SERVICE_START)) {
            SERVICE_STATUS status{};
            if (QueryServiceStatus(service, &status)) {
                if (status.dwCurrentState == SERVICE_RUNNING) {
                    running = true;
                } else if (StartServiceW(service, 0, nullptr) ||
                           GetLastError() == ERROR_SERVICE_ALREADY_RUNNING) {
                    for (int i = 0; i < 20 && !running; ++i) {
                        Sleep(100);
                        if (QueryServiceStatus(service, &status) &&
                            status.dwCurrentState == SERVICE_RUNNING) {
                            running = true;
                        }
                    }
                }
            }
            CloseServiceHandle(service);
        }
        CloseServiceHandle(manager);
        return running;
    }

    HANDLE handle_ = INVALID_HANDLE_VALUE;
    std::string error_;
};

// ---------------------------------------------------------------------------
// Port I/O, over whichever helper driver is installed
// ---------------------------------------------------------------------------
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wcast-function-type"
#endif

// GetProcAddress returns a generic FARPROC. Casting it to the real signature is
// the documented way to use it; the type mismatch is inherent to the API rather
// than a mistake here, which is what the warning is objecting to.
template <typename Signature>
Signature resolveSymbol(HMODULE module, const char* name) {
    return reinterpret_cast<Signature>(GetProcAddress(module, name));
}

#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

class DllByteIo : public IByteIo {
public:
    ~DllByteIo() override { unload(); }

    void idle() override { Sleep(0); }

    virtual bool load() = 0;
    virtual std::string description() const = 0;
    virtual std::string loadError() const = 0;

    // Virtual because a helper that needs a shutdown call has real work to do
    // here, not just to drop the library.
    virtual void unload() {
        if (module_) {
            FreeLibrary(module_);
            module_ = nullptr;
        }
    }

protected:
    HMODULE module_ = nullptr;
    std::string error_;
};

// InpOut32 / InpOutx64 by Highresolution Enterprises: a tiny signed driver plus
// a DLL, and the path most existing Boot Camp fan tools use.
class InpOutByteIo final : public DllByteIo {
public:
    bool load() override {
        const wchar_t* candidates[] = {L"inpoutx64.dll", L"inpout32.dll", L"InpOut32.dll"};
        for (const wchar_t* name : candidates) {
            module_ = LoadLibraryW(name);
            if (module_) {
                dllName_ = wideToNarrow(name);
                break;
            }
        }
        if (!module_) {
            error_ = "inpoutx64.dll is not installed";
            return false;
        }

        // Prefer the byte-sized helpers; fall back to the classic short-based
        // entry points, which every build of the DLL exports.
        readByte_ = resolveSymbol<ReadByteFn>(module_, "DlPortReadPortUchar");
        writeByte_ = resolveSymbol<WriteByteFn>(module_, "DlPortWritePortUchar");
        if (!readByte_ || !writeByte_) {
            readShort_ = resolveSymbol<ReadShortFn>(module_, "Inp32");
            writeShort_ = resolveSymbol<WriteShortFn>(module_, "Out32");
        }
        if ((!readByte_ || !writeByte_) && (!readShort_ || !writeShort_)) {
            error_ = dllName_ + " does not export the expected port functions";
            unload();
            return false;
        }

        // The DLL loading proves nothing: the kernel driver has to be running
        // too. Ask it, rather than finding out through garbage readings.
        using DriverOpenFn = BOOL(__stdcall*)();
        if (auto driverOpen = resolveSymbol<DriverOpenFn>(module_, "IsInpOutDriverOpen")) {
            if (!driverOpen()) {
                error_ = dllName_ + " is present but its kernel driver is not running. Install or "
                                    "start the InpOut driver package.";
                unload();
                return false;
            }
        }
        return true;
    }

    std::string description() const override {
        return "InpOut32 (" + dllName_ + ")";
    }

    std::string loadError() const override { return error_; }

    uint8_t inb(uint16_t port) override {
        if (readByte_) return static_cast<uint8_t>(readByte_(port) & 0xFF);
        return static_cast<uint8_t>(readShort_(static_cast<short>(port)) & 0xFF);
    }

    void outb(uint16_t port, uint8_t value) override {
        if (writeByte_) {
            writeByte_(port, value);
        } else {
            writeShort_(static_cast<short>(port), static_cast<short>(value));
        }
    }

private:
    using ReadByteFn = unsigned char(__stdcall*)(unsigned long);
    using WriteByteFn = void(__stdcall*)(unsigned long, unsigned char);
    using ReadShortFn = short(__stdcall*)(short);
    using WriteShortFn = void(__stdcall*)(short, short);

    ReadByteFn readByte_ = nullptr;
    WriteByteFn writeByte_ = nullptr;
    ReadShortFn readShort_ = nullptr;
    WriteShortFn writeShort_ = nullptr;
    std::string dllName_;
};

// WinRing0: the other widely used helper, and the only option on a machine that
// already ships it for another tool. Note that Windows may refuse to load it
// where the vulnerable-driver blocklist or memory integrity is enabled, which
// is exactly why InpOut32 is tried first.
class WinRing0ByteIo final : public DllByteIo {
public:
    bool load() override {
        const wchar_t* candidates[] = {L"WinRing0x64.dll", L"WinRing0.dll"};
        for (const wchar_t* name : candidates) {
            module_ = LoadLibraryW(name);
            if (module_) {
                dllName_ = wideToNarrow(name);
                break;
            }
        }
        if (!module_) {
            error_ = "WinRing0 is not installed";
            return false;
        }

        initialize_ = resolveSymbol<InitFn>(module_, "InitializeOls");
        shutdown_ = resolveSymbol<ShutdownFn>(module_, "DeinitializeOls");
        readByte_ = resolveSymbol<ReadFn>(module_, "ReadIoPortByte");
        writeByte_ = resolveSymbol<WriteFn>(module_, "WriteIoPortByte");
        if (!initialize_ || !readByte_ || !writeByte_) {
            error_ = dllName_ + " does not export the expected port functions";
            unload();
            return false;
        }
        if (!initialize_()) {
            error_ = dllName_ + " loaded but its driver could not be opened. Windows may be "
                                "blocking it (memory integrity or the vulnerable driver list).";
            unload();
            return false;
        }
        active_ = true;
        return true;
    }

    void unload() override {
        if (active_ && shutdown_) shutdown_();
        active_ = false;
        DllByteIo::unload();
    }

    std::string description() const override { return "WinRing0 (" + dllName_ + ")"; }
    std::string loadError() const override { return error_; }

    uint8_t inb(uint16_t port) override { return readByte_(port); }
    void outb(uint16_t port, uint8_t value) override { writeByte_(port, value); }

private:
    using InitFn = BOOL(*)();
    using ShutdownFn = void(*)();
    using ReadFn = unsigned char(*)(unsigned short);
    using WriteFn = void(*)(unsigned short, unsigned char);

    InitFn initialize_ = nullptr;
    ShutdownFn shutdown_ = nullptr;
    ReadFn readByte_ = nullptr;
    WriteFn writeByte_ = nullptr;
    bool active_ = false;
    std::string dllName_;
};

std::unique_ptr<DllByteIo> loadPortIo(std::string& detail) {
    auto inout = std::make_unique<InpOutByteIo>();
    if (inout->load()) {
        detail = inout->description();
        return inout;
    }
    const std::string inoutError = inout->loadError();

    auto winring = std::make_unique<WinRing0ByteIo>();
    if (winring->load()) {
        detail = winring->description();
        return winring;
    }
    detail = "no helper driver: " + inoutError + "; and " + winring->loadError();
    return nullptr;
}

// A path that opens is not the same as a path that works. Reading the key count
// is the cheapest proof that the SMC is really answering.
bool answers(ISmcTransport& transport) {
    Value value;
    if (!transport.readKey(Key("#KEY"), 4, value)) return false;
    if (value.size != 4) return false;
    const uint32_t count = value.toU32();
    return count > 0 && count <= 8192;
}

}  // namespace

struct SmcSession::State {
    std::unique_ptr<NamedSmcMutex> smcLock;
    std::unique_ptr<Win32KernelDevice> device;
    std::unique_ptr<DllByteIo> portIo;
    std::unique_ptr<ApplesmcTransport> applesmc;
    std::unique_ptr<PortIoTransport> port;
    ISmcTransport* active = nullptr;
    std::vector<TransportAttempt> attempts;
};

SmcSession::SmcSession() : state_(new State()) {}
SmcSession::~SmcSession() { close(); }

SmcSession::SmcSession(SmcSession&& other) noexcept = default;
SmcSession& SmcSession::operator=(SmcSession&& other) noexcept = default;

void SmcSession::close() {
    if (state_->applesmc) state_->applesmc->close();
    if (state_->device) state_->device->close();
    state_->applesmc.reset();
    state_->port.reset();
    state_->portIo.reset();
    state_->device.reset();
    state_->active = nullptr;
}

ISmcTransport* SmcSession::transport() const { return state_->active; }

const std::vector<TransportAttempt>& SmcSession::attempts() const { return state_->attempts; }

std::string SmcSession::activeName() const {
    return state_->active ? state_->active->name() : "none";
}

std::string SmcSession::summary() const {
    std::string out;
    for (const TransportAttempt& attempt : state_->attempts) {
        out += attempt.name;
        out += attempt.ok ? ": ok" : ": unavailable";
        out += " - " + attempt.detail + "\n";
    }
    return out;
}

bool SmcSession::open() {
    close();
    state_->attempts.clear();

    // One lock for the session, shared by whichever transport ends up active.
    state_->smcLock = std::make_unique<NamedSmcMutex>();

    // 1. The device the Boot Camp drivers publish. Preferred: no driver of our
    //    own, and the driver serialises access for us.
    {
        TransportAttempt attempt;
        attempt.name = "applesmc";
        state_->device = std::make_unique<Win32KernelDevice>();
        if (!state_->device->open()) {
            attempt.detail = state_->device->lastError();
            state_->attempts.push_back(attempt);
            state_->device.reset();
        } else {
            state_->applesmc = std::make_unique<ApplesmcTransport>(*state_->device);
            state_->applesmc->setTransactionLock(state_->smcLock.get());
            if (answers(*state_->applesmc)) {
                attempt.ok = true;
                attempt.detail = "\\\\.\\APPLESMC responded";
                state_->attempts.push_back(attempt);
                state_->active = state_->applesmc.get();
                return true;
            }
            attempt.detail =
                "the device opened but the SMC did not answer a key count read";
            state_->attempts.push_back(attempt);
            state_->applesmc.reset();
            state_->device.reset();
        }
    }

    // 2. The SMC's own I/O ports, through a helper driver.
    {
        TransportAttempt attempt;
        attempt.name = "port-io";
        std::string detail;
        state_->portIo = loadPortIo(detail);
        if (!state_->portIo) {
            attempt.detail = detail;
            state_->attempts.push_back(attempt);
        } else {
            state_->port = std::make_unique<PortIoTransport>(*state_->portIo);
            state_->port->setTransactionLock(state_->smcLock.get());
            if (answers(*state_->port)) {
                attempt.ok = true;
                attempt.detail = detail + ", ports 0x300/0x304 answered";
                state_->attempts.push_back(attempt);
                state_->active = state_->port.get();
                return true;
            }
            attempt.detail = detail +
                             ", but the SMC did not answer on ports 0x300/0x304";
            state_->attempts.push_back(attempt);
            state_->port.reset();
            state_->portIo.reset();
        }
    }

    return false;
}

}  // namespace fanforge

#endif  // _WIN32
