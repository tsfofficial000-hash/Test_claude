#include "test_framework.h"

#include <exception>

namespace testfw {

std::vector<Case>& registry() {
    static std::vector<Case> cases;
    return cases;
}

namespace {
int g_checks = 0;
int g_failures = 0;
const char* g_current = "";
}  // namespace

void check(bool ok, const char* expr, const char* file, int line) {
    ++g_checks;
    if (ok) return;
    ++g_failures;
    std::printf("  FAIL  %s\n        %s:%d: %s\n", g_current, file, line, expr);
}

void checkNear(double a, double b, double tolerance, const char* expr, const char* file,
               int line) {
    ++g_checks;
    if (std::fabs(a - b) <= tolerance) return;
    ++g_failures;
    std::printf("  FAIL  %s\n        %s:%d: %s  (%.6g vs %.6g, tolerance %.6g)\n", g_current, file,
                line, expr, a, b, tolerance);
}

int runAll(const char* suiteName) {
    const size_t total = registry().size();
    std::printf("=== %s: %zu cases ===\n", suiteName, total);
    size_t passed = 0;
    for (const Case& c : registry()) {
        g_current = c.name.c_str();
        const int before = g_failures;
        try {
            c.fn();
        } catch (const std::exception& e) {
            ++g_failures;
            std::printf("  FAIL  %s\n        threw: %s\n", g_current, e.what());
        } catch (...) {
            ++g_failures;
            std::printf("  FAIL  %s\n        threw an unknown exception\n", g_current);
        }
        const bool ok = (g_failures == before);
        if (ok) ++passed;
        std::printf("  %-52s %s\n", c.name.c_str(), ok ? "ok" : "FAILED");
    }
    std::printf("=== %zu/%zu cases passed, %d checks, %d failures ===\n", passed, total, g_checks,
                g_failures);
    return g_failures == 0 ? 0 : 1;
}

}  // namespace testfw

int main() { return testfw::runAll("fanforge"); }
