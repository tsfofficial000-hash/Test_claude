#pragma once
//
// Minimal test harness. No external dependency, so the suite builds and runs
// identically under the native toolchain and under a Windows cross-compiler.
//
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace testfw {

struct Case {
    std::string name;
    void (*fn)();
};

std::vector<Case>& registry();

struct Registrar {
    Registrar(const char* name, void (*fn)()) { registry().push_back({name, fn}); }
};

void check(bool ok, const char* expr, const char* file, int line);
void checkNear(double a, double b, double tolerance, const char* expr, const char* file, int line);
int runAll(const char* suiteName);

}  // namespace testfw

#define TEST(name)                                                       \
    static void name();                                                  \
    [[maybe_unused]] static ::testfw::Registrar registrar_##name(#name, name); \
    static void name()

#define CHECK(cond) ::testfw::check(static_cast<bool>(cond), #cond, __FILE__, __LINE__)
#define CHECK_NEAR(a, b, tol) \
    ::testfw::checkNear((a), (b), (tol), #a " ~= " #b, __FILE__, __LINE__)
