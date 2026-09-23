//
// Temperature units and the CSV recorder.
//
#include <cmath>
#include <cstdio>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>

#include "core/csv.h"
#include "core/units.h"
#include "test_framework.h"

using namespace fanforge;

namespace {

std::string readAll(const std::string& path) {
    std::ifstream file(path.c_str(), std::ios::binary);
    std::ostringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

const char* kTempCsv = "fanforge_test_units_csv.tmp";

}  // namespace

// ---------------------------------------------------------------------------
// Units
// ---------------------------------------------------------------------------

TEST(fahrenheit_conversion_matches_the_known_fixed_points) {
    CHECK_NEAR(celsiusToFahrenheit(0.0), 32.0, 1e-9);
    CHECK_NEAR(celsiusToFahrenheit(100.0), 212.0, 1e-9);
    CHECK_NEAR(celsiusToFahrenheit(-40.0), -40.0, 1e-9);
    CHECK_NEAR(fahrenheitToCelsius(212.0), 100.0, 1e-9);
    CHECK_NEAR(fahrenheitToCelsius(celsiusToFahrenheit(37.5)), 37.5, 1e-9);
}

TEST(temperature_formatting_respects_the_unit) {
    CHECK(formatTemperature(62.5, false) == "62.5");
    CHECK(formatTemperature(62.5, true) == "144.5");
    CHECK(formatTemperatureWithUnit(62.5, false) == "62.5 C");
    CHECK(formatTemperatureWithUnit(62.5, true) == "144.5 F");
    CHECK(formatTemperatureRounded(60.0, false) == "60");
    CHECK(formatTemperatureRounded(60.0, true) == "140");
    CHECK(std::string(temperatureUnit(false)) == "C");
    CHECK(std::string(temperatureUnit(true)) == "F");
}

TEST(a_missing_reading_formats_as_a_dash_not_as_nan) {
    const double nan = std::numeric_limits<double>::quiet_NaN();
    CHECK(formatTemperature(nan, false) == "--");
    CHECK(formatTemperatureWithUnit(nan, true) == "--");
    CHECK(formatTemperatureRounded(nan, false) == "--");
}

// ---------------------------------------------------------------------------
// CSV
// ---------------------------------------------------------------------------

TEST(csv_writes_a_header_once_and_one_row_per_reading) {
    {
        CsvRecorder recorder;
        CHECK(recorder.open(kTempCsv));
        CHECK(recorder.isOpen());
        recorder.setHeader({"seconds", "F0", "F1"});
        recorder.writeRow({1.0, 2000.0, 3000.0});
        recorder.writeRow({2.0, 2100.0, 3100.0});
        CHECK(recorder.rowsWritten() == 2);
        recorder.close();
        CHECK(!recorder.isOpen());
    }

    const std::string text = readAll(kTempCsv);
    CHECK(text == "seconds,F0,F1\n1.00,2000.00,3000.00\n2.00,2100.00,3100.00\n");
    std::remove(kTempCsv);
}

TEST(csv_writes_a_missing_reading_as_an_empty_cell) {
    {
        CsvRecorder recorder;
        CHECK(recorder.open(kTempCsv));
        recorder.setHeader({"seconds", "F0", "F1"});
        recorder.writeRow({1.0, std::numeric_limits<double>::quiet_NaN(), 3000.0});
        recorder.close();
    }

    const std::string text = readAll(kTempCsv);
    // "nan" would make the file unloadable in a spreadsheet; an empty cell
    // keeps it a number column with a gap.
    CHECK(text.find("nan") == std::string::npos);
    CHECK(text == "seconds,F0,F1\n1.00,,3000.00\n");
    std::remove(kTempCsv);
}

TEST(csv_keeps_the_column_count_when_a_row_is_short_or_long) {
    {
        CsvRecorder recorder;
        CHECK(recorder.open(kTempCsv));
        recorder.setHeader({"a", "b"});
        recorder.writeRow({1.0});                    // short
        recorder.writeRow({1.0, 2.0, 3.0});          // long
        recorder.close();
    }

    const std::string text = readAll(kTempCsv);
    CHECK(text == "a,b\n1.00,\n1.00,2.00\n");
    std::remove(kTempCsv);
}

TEST(csv_escapes_cells_that_contain_separators) {
    CHECK(CsvRecorder::escapeCell("plain") == "plain");
    CHECK(CsvRecorder::escapeCell("a,b") == "\"a,b\"");
    CHECK(CsvRecorder::escapeCell("a\"b") == "\"a\"\"b\"");
    CHECK(CsvRecorder::escapeCell("a\nb") == "\"a\nb\"");
}

TEST(csv_reports_a_bad_path_instead_of_throwing) {
    CsvRecorder recorder;
    CHECK(!recorder.open(""));
    CHECK(!recorder.isOpen());
    CHECK(!recorder.lastError().empty());
    CHECK(recorder.rowsWritten() == 0);

    // Writing while closed must be a no-op, not a crash.
    recorder.setHeader({"a"});
    recorder.writeRow({1.0});
    CHECK(recorder.rowsWritten() == 0);
}

TEST(csv_flush_persists_without_an_explicit_close) {
    {
        CsvRecorder recorder;
        CHECK(recorder.open(kTempCsv));
        recorder.setHeader({"a"});
        recorder.writeRow({7.0});
        // Destroyed here; the destructor has to close the stream.
    }
    const std::string text = readAll(kTempCsv);
    CHECK(text == "a\n7.00\n");
    std::remove(kTempCsv);
}
