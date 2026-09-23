#pragma once
//
// Recording readings to a CSV file.
//
// Deliberately dumb: it appends rows, writes the header once, and reports
// failure through lastError() rather than throwing. A logging failure must
// never take the fan control loop down with it.
//
#include <fstream>
#include <string>
#include <vector>

namespace fanforge {

class CsvRecorder {
public:
    CsvRecorder() = default;
    ~CsvRecorder();

    CsvRecorder(const CsvRecorder&) = delete;
    CsvRecorder& operator=(const CsvRecorder&) = delete;

    // Truncates and opens `path`, creating the parent directory if needed.
    // False leaves the recorder closed and lastError() explains why.
    bool open(const std::string& path);

    void close();
    bool isOpen() const { return file_.is_open(); }

    // Written once, on the first row after open().
    void setHeader(const std::vector<std::string>& columns);

    // Appends one row. Extra values are dropped and missing ones are left
    // empty, so a column count that changes mid-log cannot corrupt the file.
    void writeRow(const std::vector<double>& values);

    const std::string& lastError() const { return error_; }
    const std::string& path() const { return path_; }
    long long rowsWritten() const { return rowsWritten_; }

    // A CSV cell must not contain a comma or a quote; the header comes from
    // sensor keys, which never should, but a mangled header is worse than a
    // sanitised one.
    static std::string escapeCell(const std::string& text);

private:
    std::ofstream file_;
    std::string path_;
    std::string error_;
    std::vector<std::string> header_;
    bool headerPending_ = false;
    bool headerWritten_ = false;
    long long rowsWritten_ = 0;
};

}  // namespace fanforge
