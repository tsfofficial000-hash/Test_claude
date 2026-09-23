#include "core/sensordb.h"
#include "test_framework.h"

using namespace fanforge;

TEST(sensordb_names_the_well_known_keys) {
    CHECK(sensorName("TC0P") == "CPU proximity");
    CHECK(sensorName("TC0D") == "CPU die");
    CHECK(sensorName("TG0P") == "GPU proximity");
    CHECK(sensorName("TG0D") == "GPU die");
    CHECK(sensorName("TC0H") == "CPU heatsink");
    CHECK(sensorName("TB0T") == "Battery");
    CHECK(sensorName("Ts0P") == "Palm rest");
}

TEST(sensordb_names_numbered_families) {
    // Per-core CPU sensors are a family, not a fixed list: an eight-core
    // machine has TC1C through TC8C.
    CHECK(sensorName("TC1C") == "CPU core 1");
    CHECK(sensorName("TC7C") == "CPU core 7");
    // The same applies to the numbered battery, palm-rest and GPU families,
    // for indices the catalogue does not name explicitly.
    CHECK(sensorName("TB5T") == "Battery 5");
    CHECK(sensorName("Ts4S") == "Palm rest 4 (skin)");
    CHECK(sensorName("TG3D") == "GPU die 3");
    CHECK(sensorName("TM4P") == "Memory 4");
}

TEST(sensordb_always_gives_an_unknown_key_a_name) {
    // A key nobody has seen still has to be usable in the UI.
    const std::string name = sensorName("TQ9P");
    CHECK(!name.empty());
    CHECK(name.find("TQ9P") != std::string::npos);
}

TEST(sensordb_categorises_by_family) {
    CHECK(sensorCategory("TC0P") == SensorCategory::Cpu);
    CHECK(sensorCategory("TC9C") == SensorCategory::Cpu);
    CHECK(sensorCategory("TG0D") == SensorCategory::Gpu);
    CHECK(sensorCategory("TB0T") == SensorCategory::Battery);
    CHECK(sensorCategory("TM0P") == SensorCategory::Memory);
    CHECK(sensorCategory("Ts0P") == SensorCategory::Palm);
    CHECK(sensorCategory("TW0P") == SensorCategory::Wireless);
    CHECK(sensorCategory("TL0P") == SensorCategory::Display);
    CHECK(sensorCategory("ZZZZ") == SensorCategory::Other);
}

TEST(sensordb_every_category_has_a_name) {
    const SensorCategory all[] = {
        SensorCategory::Cpu,       SensorCategory::Gpu,       SensorCategory::Memory,
        SensorCategory::Storage,   SensorCategory::Battery,   SensorCategory::Enclosure,
        SensorCategory::Ambient,   SensorCategory::Palm,      SensorCategory::Wireless,
        SensorCategory::Power,     SensorCategory::Display,   SensorCategory::Other,
    };
    for (SensorCategory category : all) {
        const std::string name = categoryName(category);
        CHECK(!name.empty());
    }
}

TEST(plausibility_rejects_the_smc_dead_sensor_sentinels) {
    // These are the values a disconnected or dead sensor reports. Feeding one
    // to a curve would command a nonsense fan speed.
    CHECK(!plausibleTemperature(-127.0));
    CHECK(!plausibleTemperature(-128.0));
    CHECK(!plausibleTemperature(-38.375));
    CHECK(!plausibleTemperature(-1000.0));
}

TEST(plausibility_accepts_real_readings) {
    CHECK(plausibleTemperature(0.0));
    CHECK(plausibleTemperature(21.5));
    CHECK(plausibleTemperature(45.5));
    CHECK(plausibleTemperature(95.0));
    CHECK(plausibleTemperature(129.0));
}

TEST(plausibility_rejects_non_finite_and_absurd_values) {
    const double nan = 0.0 / 0.0;
    const double infinity = 1.0 / 0.0;
    CHECK(!plausibleTemperature(nan));
    CHECK(!plausibleTemperature(infinity));
    CHECK(!plausibleTemperature(-infinity));
    CHECK(!plausibleTemperature(1000.0));
    CHECK(!plausibleTemperature(130.0));
    CHECK(!plausibleTemperature(-20.0));
}

TEST(readings_sort_hottest_first) {
    std::vector<SensorReading> readings = {
        {"TC0P", "CPU proximity", SensorCategory::Cpu, 45.0},
        {"TG0D", "GPU die", SensorCategory::Gpu, 72.5},
        {"TB0T", "Battery", SensorCategory::Battery, 31.0},
        {"TC0D", "CPU die", SensorCategory::Cpu, 60.25},
    };
    sortByTemperature(readings);
    CHECK(readings.size() == 4);
    CHECK(readings[0].key == "TG0D");
    CHECK(readings[1].key == "TC0D");
    CHECK(readings[2].key == "TC0P");
    CHECK(readings[3].key == "TB0T");
}

TEST(readings_sort_is_stable_for_equal_temperatures) {
    std::vector<SensorReading> readings = {
        {"TG0P", "GPU proximity", SensorCategory::Gpu, 50.0},
        {"TC0P", "CPU proximity", SensorCategory::Cpu, 50.0},
    };
    sortByTemperature(readings);
    CHECK(readings[0].key == "TC0P");  // ties broken by key, so the order is defined
    CHECK(readings[1].key == "TG0P");
}
