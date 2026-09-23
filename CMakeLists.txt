cmake_minimum_required(VERSION 3.16)
project(FanForge LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# Standalone Windows binaries: no runtime DLLs to copy alongside them, so the
# result is two .exe files you can put anywhere.
if(WIN32 AND MINGW)
  add_link_options(-static -static-libgcc -static-libstdc++)
endif()

# ---------------------------------------------------------------------------
# Warnings. GCC/Clang only; the Windows builds use MinGW-w64.
# ---------------------------------------------------------------------------
function(fanforge_warnings target)
  if(NOT MSVC)
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
  endif()
endfunction()

# ---------------------------------------------------------------------------
# fanforge_core - portable, no OS dependencies beyond the transport shims.
# This is what the test suite exercises on every platform.
# ---------------------------------------------------------------------------
add_library(fanforge_core STATIC
  src/smc/key.cpp
  src/smc/port_io_transport.cpp
  src/smc/transport_applesmc.cpp
  src/smc/device.cpp
  src/smc/smc_sim.cpp
  src/core/policy.cpp
  src/core/curve.cpp
  src/core/sensordb.cpp
  src/core/fan.cpp
  src/core/controller.cpp
  src/core/config.cpp
  src/core/selftest.cpp
  src/core/units.cpp
  src/core/csv.cpp
  src/sim/simulate.cpp
)
target_include_directories(fanforge_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
fanforge_warnings(fanforge_core)

# ---------------------------------------------------------------------------
# The one OS-specific unit. Exactly one of these is compiled.
# ---------------------------------------------------------------------------
if(WIN32)
  add_library(fanforge_platform STATIC src/platform/smc_session_win.cpp)
  target_link_libraries(fanforge_platform PUBLIC fanforge_core)
else()
  add_library(fanforge_platform STATIC src/platform/smc_session_stub.cpp)
  target_link_libraries(fanforge_platform PUBLIC fanforge_core)
endif()
fanforge_warnings(fanforge_platform)

# ---------------------------------------------------------------------------
# Command line tool. Builds on every platform: on non-Windows it reports
# accurately that there is no SMC, rather than failing to build.
# ---------------------------------------------------------------------------
add_executable(fanforge-cli src/cli/main.cpp)
target_link_libraries(fanforge-cli PRIVATE fanforge_platform)
fanforge_warnings(fanforge-cli)

# ---------------------------------------------------------------------------
# The window. Windows only, obviously.
# ---------------------------------------------------------------------------
if(WIN32)
  add_executable(FanForge WIN32 src/win/gui.cpp)
  target_link_libraries(FanForge PRIVATE fanforge_platform shell32 user32 gdi32)
  # Every call into the Win32 API is the wide one, so the character-set macros
  # have to agree. Without this, IDI_APPLICATION and friends expand to their
  # ANSI forms and the wide entry points reject them.
  target_compile_definitions(FanForge PRIVATE UNICODE _UNICODE)
  fanforge_warnings(FanForge)
endif()

# ---------------------------------------------------------------------------
# Test suite. Builds and runs natively anywhere.
# ---------------------------------------------------------------------------
enable_testing()

add_executable(fanforge_tests
  tests/test_main.cpp
  tests/test_key.cpp
  tests/test_port_transport.cpp
  tests/test_applesmc_transport.cpp
  tests/test_device.cpp
  tests/test_sensordb.cpp
  tests/test_curve.cpp
  tests/test_fans.cpp
  tests/test_controller.cpp
  tests/test_config.cpp
  tests/test_selftest.cpp
  tests/test_units_csv.cpp
  tests/test_session.cpp
  tests/test_simulation.cpp
)
# The session lifecycle lives in the platform unit, and the tests have to link
# the same one the tool does.
target_link_libraries(fanforge_tests PRIVATE fanforge_platform)
fanforge_warnings(fanforge_tests)

add_test(NAME fanforge_tests COMMAND fanforge_tests)
