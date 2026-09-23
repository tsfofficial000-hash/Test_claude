//
// fanforge-cli - the same engine as the window, without the window.
//
// Useful for scripting, for checking a machine before trusting the GUI with it,
// and for diagnosing exactly which access path worked and why the others did
// not.
//
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "core/config.h"
#include "core/controller.h"
#include "core/csv.h"
#include "core/fan.h"
#include "core/selftest.h"
#include "core/sensordb.h"
#include "core/units.h"
#include "platform/platform.h"
#include "sim/simulate.h"
#include "smc/device.h"

using namespace fanforge;

namespace {

void printUsage() {
    std::cout <<
        "fanforge-cli - fan and thermal control for Intel Macs running Windows via Boot Camp\n"
        "\n"
        "Usage: fanforge-cli [--config <path>] <command> [arguments]\n"
        "\n"
        "Reading\n"
        "  info               Which access paths exist, and what the machine reports\n"
        "  temps              Every plausible temperature, hottest first\n"
        "  sensors            Temperatures, voltages, currents and power\n"
        "  fans               Fan speeds, limits and current control mode\n"
        "  keys               Every SMC key with its type and value\n"
        "  get <KEY>          Read one key, e.g. get TC0P\n"
        "\n"
        "Control\n"
        "  set <fan> <rpm>    Take one fan manual at a speed (clamped to its range)\n"
        "  auto [fan]         Return one fan, or every fan, to system control\n"
        "  restore            Return every fan to system control\n"
        "  selftest           Non-destructive check of the whole write path\n"
        "\n"
        "Running\n"
        "  watch [seconds]    Run the saved configuration's control loop\n"
        "  simulate [seconds] Run the whole loop against a simulated machine\n";
}

bool parseDouble(const std::string& text, double& out) {
    if (text.empty()) return false;
    char* end = nullptr;
    const double value = std::strtod(text.c_str(), &end);
    if (end == text.c_str() || (end && *end)) return false;
    out = value;
    return true;
}

int parseInt(const std::string& text, int fallback) {
    double value = 0;
    if (!parseDouble(text, value)) return fallback;
    return static_cast<int>(value + (value < 0 ? -0.5 : 0.5));
}

std::string formatRpm(double rpm) {
    std::ostringstream out;
    out << std::fixed << std::setprecision(0) << rpm;
    return out.str();
}

std::string formatTemp(double celsius) {
    std::ostringstream out;
    out << std::fixed << std::setprecision(1) << celsius;
    return out.str();
}

// A session that reports exactly why it failed, rather than a bare error.
std::optional<SmcSession> openSession() {
    SmcSession session;
    if (session.open()) {
        std::cout << "smc         : " << session.activeName() << "\n";
        return std::optional<SmcSession>(std::move(session));
    }

    std::cerr << "Could not reach the SMC on this machine.\n\n";
    std::cerr << session.summary();
    std::cerr <<
        "\nThe SMC is only reachable from Windows running natively on an Intel Mac.\n"
        "What each path needs:\n"
        "  applesmc  The Boot Camp support software, which installs the AppleSMC service.\n"
        "  port-io   A helper driver: InpOut32 (inpoutx64.dll + driver) or WinRing0.\n"
        "            Place the DLL next to this executable. See README.md.\n";
    return std::nullopt;
}

int commandInfo(const Config& config) {
    std::optional<SmcSession> session = openSession();
    if (!session) return 2;

    SmcDevice device(*session->transport());
    FanBank bank;
    if (!bank.discover(device)) {
        std::cerr << "The SMC did not report a fan count.\n";
        return 3;
    }

    std::cout << "keys        : " << device.keyCount() << "\n";
    std::cout << "fans        : " << bank.count() << "\n";
    std::cout << "control     : " << manualModeName(bank.manualMode()) << "\n";
    if (!bank.notes().empty()) std::cout << "notes       : " << bank.notes() << "\n";
    std::cout << "poll        : " << config.pollIntervalMs << " ms\n";
    return 0;
}

int commandTemps(const Config& config) {
    std::optional<SmcSession> session = openSession();
    if (!session) return 2;

    SmcDevice device(*session->transport());
    FanBank bank;
    bank.discover(device);
    Controller controller(device, bank);

    const std::vector<SensorReading> readings = controller.readTemperatures();
    if (readings.empty()) {
        std::cout << "No temperature sensor reported a plausible value.\n";
        return 3;
    }

    std::cout << "\n  " << readings.size() << " sensors, hottest first\n\n";
    std::cout << "  key   type   value      sensor\n";
    std::cout << "  ------------------------------------------------\n";
    for (const SensorReading& reading : readings) {
        std::cout << "  " << std::left << std::setw(6) << reading.key << std::setw(7) << "sp78"
                  << std::right << std::setw(6)
                  << formatTemperature(reading.celsius, config.fahrenheit) << " "
                  << temperatureUnit(config.fahrenheit) << "   " << reading.name << "\n";
    }
    return 0;
}

int commandSensors() {
    std::optional<SmcSession> session = openSession();
    if (!session) return 2;

    SmcDevice device(*session->transport());
    const std::vector<Key> keys = device.allKeys();
    std::cout << "\n  key   type   value                description\n";
    std::cout << "  ------------------------------------------------------------\n";
    int shown = 0;
    for (const Key& key : keys) {
        Value value;
        if (!device.read(key, value)) continue;
        if (!value.toDouble().has_value()) continue;  // numeric types only
        const std::string name = key.str();
        std::cout << "  " << std::left << std::setw(6) << name << std::setw(7) << value.type.str()
                  << std::right << std::setw(16) << value.toText() << "   "
                  << (name[0] == 'T' ? sensorName(name) : std::string("")) << "\n";
        ++shown;
    }
    std::cout << "\n  " << shown << " numeric keys\n";
    return 0;
}

int commandKeys() {
    std::optional<SmcSession> session = openSession();
    if (!session) return 2;

    SmcDevice device(*session->transport());
    const std::vector<Key> keys = device.allKeys();
    std::cout << "\n  " << keys.size() << " keys\n\n";
    for (const Key& key : keys) {
        KeyInfo info;
        Value value;
        if (!device.keyInfo(key, info)) continue;
        const bool read = device.read(key, value);
        std::cout << "  " << std::left << std::setw(6) << key.str() << std::setw(7)
                  << Key(info.dataType).str() << " " << (info.writable() ? "rw" : "r-") << " "
                  << std::right << std::setw(18) << (read ? value.toText() : std::string("-"))
                  << "\n";
    }
    return 0;
}

int commandGet(const std::string& name) {
    std::optional<SmcSession> session = openSession();
    if (!session) return 2;

    SmcDevice device(*session->transport());
    const Key key(name.c_str());
    KeyInfo info;
    if (!device.keyInfo(key, info)) {
        std::cerr << "No such key: " << name << "\n";
        return 3;
    }
    Value value;
    if (!device.read(key, value)) {
        std::cerr << "The key exists but could not be read.\n";
        return 3;
    }
    std::cout << "key         : " << key.str() << "  (" << value.size << " bytes)\n";
    std::cout << "type        : " << Key(info.dataType).str() << "\n";
    std::cout << "attributes  : " << (info.readable() ? "readable " : "")
              << (info.writable() ? "writable " : "")
              << (info.function() ? "function" : "") << "\n";
    std::cout << "value       : " << value.toText() << "\n";
    if (auto number = value.toDouble()) {
        std::cout << "as number   : " << std::setprecision(10) << *number << "\n";
    }
    return 0;
}

int commandFans() {
    std::optional<SmcSession> session = openSession();
    if (!session) return 2;

    SmcDevice device(*session->transport());
    FanBank bank;
    if (!bank.discover(device)) {
        std::cerr << "The SMC did not report a fan count.\n";
        return 3;
    }
    if (bank.empty()) {
        std::cout << "This machine reports no fans. Temperatures are still available.\n";
        return 0;
    }

    std::cout << "control     : " << manualModeName(bank.manualMode()) << "\n";
    std::cout << "force mask  : 0x" << std::hex << bank.forceMask() << std::dec << "\n\n";
    for (size_t i = 0; i < bank.count(); ++i) {
        const FanInfo& fan = bank.at(i);
        std::cout << "[" << i << "] " << fan.label << "\n";
        std::cout << "    speed   : " << formatRpm(fan.actualRpm) << " RPM\n";
        std::cout << "    target  : " << formatRpm(fan.targetRpm) << " RPM\n";
        if (fan.hasLimits) {
            std::cout << "    range   : " << formatRpm(fan.minRpm) << " - "
                      << formatRpm(fan.maxRpm) << " RPM (safe " << formatRpm(fan.safeRpm) << ")\n";
        } else {
            std::cout << "    range   : not reported; this fan will not be driven\n";
        }
        std::cout << "    control : " << (fan.manual ? "manual" : "system") << "\n";
    }
    return 0;
}

int commandSet(int fanIndex, double rpm) {
    std::optional<SmcSession> session = openSession();
    if (!session) return 2;

    SmcDevice device(*session->transport());
    FanBank bank;
    bank.discover(device);
    if (fanIndex < 0 || static_cast<size_t>(fanIndex) >= bank.count()) {
        std::cerr << "This machine has " << bank.count() << " fan(s).\n";
        return 3;
    }
    const FanInfo& fan = bank.at(static_cast<size_t>(fanIndex));
    if (!fan.controllable()) {
        std::cerr << "Fan " << fanIndex << " (" << fan.label
                  << ") does not report a usable speed range, so it will not be driven.\n";
        return 3;
    }

    const double clamped = bank.clamp(static_cast<size_t>(fanIndex), rpm);
    if (clamped != rpm) {
        std::cout << "clamped     : " << formatRpm(rpm) << " -> " << formatRpm(clamped)
                  << " RPM (fan range " << formatRpm(fan.minRpm) << "-" << formatRpm(fan.maxRpm)
                  << ")\n";
    }
    if (!bank.setTarget(device, static_cast<size_t>(fanIndex), clamped)) {
        std::cerr << "The speed could not be set and confirmed. The fan was returned to system "
                     "control.\n";
        return 4;
    }
    std::cout << "fan " << fanIndex << "   : " << formatRpm(clamped) << " RPM confirmed\n";
    std::cout << "note        : run 'fanforge-cli auto " << fanIndex
              << "' (or 'auto') to hand it back\n";
    return 0;
}

int commandAuto(const std::vector<std::string>& arguments) {
    std::optional<SmcSession> session = openSession();
    if (!session) return 2;

    SmcDevice device(*session->transport());
    FanBank bank;
    bank.discover(device);

    if (arguments.empty()) {
        if (!bank.returnAllToSystem(device)) {
            std::cerr << "At least one fan could not be returned to system control.\n";
            return 4;
        }
        std::cout << "All " << bank.count() << " fan(s) returned to system control.\n";
        return 0;
    }

    const int index = parseInt(arguments[0], -1);
    if (index < 0 || static_cast<size_t>(index) >= bank.count()) {
        std::cerr << "This machine has " << bank.count() << " fan(s).\n";
        return 3;
    }
    if (!bank.returnToSystem(device, static_cast<size_t>(index))) {
        std::cerr << "Fan " << index << " could not be returned to system control.\n";
        return 4;
    }
    std::cout << "fan " << index << "   : system control\n";
    return 0;
}

// Engages manual control at the speed each fan is already running at, so
// nothing audible changes, checks the readback, and puts everything back. The
// logic lives in the core so the window runs exactly the same test, and so it
// can be driven by the test suite against a simulated machine.
int commandSelfTest() {
    std::optional<SmcSession> session = openSession();
    if (!session) return 2;

    SmcDevice device(*session->transport());
    FanBank bank;
    bank.discover(device);

    std::cout << "manual control : " << manualModeName(bank.manualMode()) << "\n";
    if (bank.manualMode() == ManualMode::ForceMask) {
        std::cout << "force mask     : 0x" << std::hex << bank.forceMask() << std::dec << "\n";
    }

    const SelfTestReport report = runWritePathSelfTest(device, bank);
    if (!report.applicable) {
        std::cout << "Nothing to test: " << report.skippedReason << ".\n";
        return 0;
    }

    std::cout << "\n";
    for (const SelfTestStep& step : report.steps) {
        std::cout << (step.ok ? "  ok    " : "  FAIL  ") << step.label << " - " << step.detail
                  << "\n";
    }

    // Leaving a fan pinned in manual mode is the one outcome worse than any
    // problem this test is looking for, so it is reported as a failure.
    if (bank.manualMode() == ManualMode::ForceMask && bank.forceMask() != 0) {
        std::cout << "\nWARNING: the force mask is not clear. Run 'fanforge-cli auto'.\n";
        return 4;
    }

    std::cout << "\n" << report.summary() << "\n";
    if (!report.ok()) {
        std::cout << "Self test found problems (see above).\n";
        return 4;
    }
    std::cout << "The write path is confirmed working on this machine.\n";
    return 0;
}

int commandWatch(const Config& config, int seconds) {
    std::optional<SmcSession> session = openSession();
    if (!session) return 2;

    SmcDevice device(*session->transport());
    FanBank bank;
    bank.discover(device);
    Controller controller(device, bank);
    controller.initialise(config);

    // Recording is best effort: a filesystem that will not take the log must not
    // stop the fans being controlled.
    CsvRecorder log;
    if (config.recordCsv) {
        if (log.open(defaultLogPath())) {
            std::vector<std::string> header;
            header.push_back("seconds");
            header.push_back(std::string("hottest_") + temperatureUnit(config.fahrenheit));
            for (size_t f = 0; f < bank.count(); ++f) {
                header.push_back("F" + std::to_string(f) + "_rpm");
            }
            log.setHeader(header);
            std::cout << "recording to " << log.path() << "\n";
        } else {
            std::cout << "could not start recording: " << log.lastError() << "\n";
        }
    }

    const double interval = config.pollIntervalMs / 1000.0;
    const int ticks = interval > 0 ? static_cast<int>(seconds / interval) : 0;

    std::cout << "running the control loop for " << seconds << " s\n";
    std::cout << "  time   hottest  " << (bank.empty() ? "" : "fan0 target") << "\n";

    for (int i = 0; i < ticks; ++i) {
        const Controller::TickResult result = controller.tick(interval);
        if (log.isOpen()) {
            std::vector<double> row;
            row.push_back(i * interval);
            row.push_back(result.hottestCelsius);
            for (size_t f = 0; f < bank.count(); ++f) row.push_back(bank.at(f).actualRpm);
            log.writeRow(row);
        }
        if (i % 5 == 0) {
            std::cout << std::setw(6) << static_cast<int>(i * interval) << " "
                      << std::setw(7) << (result.hasTemperatures ? formatTemp(result.hottestCelsius)
                                                                : std::string("  n/a"))
                      << "  " << std::setw(7)
                      << (result.commandedRpm.empty() ? 0.0 : result.commandedRpm[0]) << "\n";
        }
        for (const std::string& note : result.notes) std::cout << "  note: " << note << "\n";
    }

    if (log.isOpen()) {
        log.close();
        std::cout << "wrote " << log.rowsWritten() << " row(s) to " << log.path() << "\n";
    }

    if (config.restoreOnExit) {
        controller.restoreAllToSystem();
        std::cout << "all fans returned to system control\n";
    } else {
        std::cout << "restore_on_exit is off: the fans are left where they are\n";
        std::cout << "run 'fanforge-cli auto' to hand them back\n";
    }
    return 0;
}

// Runs three configurations of the same machine and reports the trade-off
// between noise and temperature. Only the balanced run prints a table; the
// numbers are the point of the other two.
int commandSimulate(int seconds) {
    SimulationOptions base;
    if (seconds > 0) base.seconds = seconds;
    base.printTable = false;

    std::cout << "A simulated two-fan MacBook Pro under a step load (light for "
              << static_cast<int>(base.lightLoadUntil) << " s, then flat out).\n";
    std::cout << "The controller, the fan bank and the SMC protocol are the real ones;\n";
    std::cout << "only the machine's response to airflow is modelled. The temperatures\n";
    std::cout << "are therefore illustrative, but every write, every clamp and every\n";
    std::cout << "safety fallback is real code.\n\n";

    SimulationOptions cool = base;
    cool.curveStartCelsius = 40.0;
    cool.curveFullCelsius = 80.0;
    const SimulationReport coolReport = runSimulation(cool, nullptr);

    SimulationOptions balanced = base;
    balanced.printTable = true;
    std::cout << "Balanced curve (minimum below 50 C, full speed by 92 C):\n";
    const SimulationReport balancedReport = runSimulation(balanced, &std::cout);

    SimulationOptions quiet = base;
    quiet.curveStartCelsius = 70.0;
    quiet.curveFullCelsius = 105.0;
    const SimulationReport quietReport = runSimulation(quiet, nullptr);

    std::cout << "\n                      cool     balanced      quiet\n";
    auto row = [](const char* label, const std::string& a, const std::string& b,
                  const std::string& c) {
        std::cout << "  " << std::left << std::setw(18) << label << std::right << std::setw(8) << a
                  << std::setw(13) << b << std::setw(11) << c << "\n";
    };
    row("peak", formatTemp(coolReport.peakCelsius) + " C",
        formatTemp(balancedReport.peakCelsius) + " C", formatTemp(quietReport.peakCelsius) + " C");
    row("settled", formatTemp(coolReport.finalCelsius) + " C",
        formatTemp(balancedReport.finalCelsius) + " C", formatTemp(quietReport.finalCelsius) + " C");
    row("average fan", formatRpm(coolReport.averageRpm), formatRpm(balancedReport.averageRpm),
        formatRpm(quietReport.averageRpm));
    row("control writes", std::to_string(coolReport.controlWrites),
        std::to_string(balancedReport.controlWrites),
        std::to_string(quietReport.controlWrites));
    row("fans released", coolReport.fansReturnedToSystem ? "yes" : "no",
        balancedReport.fansReturnedToSystem ? "yes" : "no",
        quietReport.fansReturnedToSystem ? "yes" : "no");

    std::cout << "\nMore airflow buys a cooler machine, and less buys a quieter one.\n";
    std::cout << "Neither setting can push a fan outside the range the hardware reports.\n";

    const bool ok = coolReport.fansReturnedToSystem && balancedReport.fansReturnedToSystem &&
                    quietReport.fansReturnedToSystem && balancedReport.notes.empty();
    return ok ? 0 : 4;
}

}  // namespace

int main(int argc, char** argv) {
    std::vector<std::string> arguments;
    std::string configPath;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--config" && i + 1 < argc) {
            configPath = argv[++i];
        } else if (arg == "--help" || arg == "-h") {
            printUsage();
            return 0;
        } else if (arg == "--version") {
            std::cout << "fanforge-cli 1.0\n";
            return 0;
        } else {
            arguments.push_back(arg);
        }
    }

    if (arguments.empty()) {
        printUsage();
        return 0;
    }

    const Config config = configPath.empty() ? Config::read(defaultConfigPath())
                                             : Config::read(configPath);
    const std::string& command = arguments[0];

    if (command == "help") {
        printUsage();
        return 0;
    }
    if (command == "info") return commandInfo(config);
    if (command == "temps") return commandTemps(config);
    if (command == "sensors") return commandSensors();
    if (command == "keys") return commandKeys();
    if (command == "fans") return commandFans();
    if (command == "selftest") return commandSelfTest();
    if (command == "restore") return commandAuto({});
    if (command == "get") {
        if (arguments.size() < 2) {
            std::cerr << "usage: fanforge-cli get <KEY>\n";
            return 1;
        }
        return commandGet(arguments[1]);
    }
    if (command == "set") {
        if (arguments.size() < 3) {
            std::cerr << "usage: fanforge-cli set <fan> <rpm>\n";
            return 1;
        }
        double rpm = 0;
        if (!parseDouble(arguments[2], rpm)) {
            std::cerr << "not a speed: " << arguments[2] << "\n";
            return 1;
        }
        return commandSet(parseInt(arguments[1], -1), rpm);
    }
    if (command == "auto") {
        return commandAuto(std::vector<std::string>(arguments.begin() + 1, arguments.end()));
    }
    if (command == "watch") {
        return commandWatch(config, arguments.size() > 1 ? parseInt(arguments[1], 30) : 30);
    }
    if (command == "simulate") {
        return commandSimulate(arguments.size() > 1 ? parseInt(arguments[1], 0) : 0);
    }

    std::cerr << "Unknown command: " << command << "\n\n";
    printUsage();
    return 1;
}
