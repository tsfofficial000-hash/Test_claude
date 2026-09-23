#include "core/csv.h"

#include <cstdio>

#include "core/config.h"

namespace fanforge {

CsvRecorder::~CsvRecorder() { close(); }

std::string CsvRecorder::escapeCell(const std::string& text) {
    bool needsQuoting = false;
    for (char c : text) {
        if (c == ',' || c == '"' || c == '\n' || c == '\r') {
            needsQuoting = true;
            break;
        }
    }
    if (!needsQuoting) return text;

    std::string out = "\"";
    for (char c : text) {
        if (c == '"') out += '"';  // doubled inside a quoted cell
        out += c;
    }
    out += '"';
    return out;
}

bool CsvRecorder::open(const std::string& path) {
    close();
    // A new log starts from zero. This lives here rather than in close()
    // because closing does not un-write anything: the row count is how a caller
    // reports what it just logged, and it is read after the close.
    rowsWritten_ = 0;
    path_ = path;
    if (path.empty()) {
        error_ = "no log path was given";
        return false;
    }
    ensureParentDirectory(path);

    file_.open(path.c_str(), std::ios::binary | std::ios::trunc);
    if (!file_.is_open()) {
        error_ = "could not open " + path + " for writing";
        return false;
    }
    error_.clear();
    return true;
}

void CsvRecorder::close() {
    if (file_.is_open()) {
        file_.flush();
        file_.close();
    }
    headerWritten_ = false;
    headerPending_ = false;
    // rowsWritten_ deliberately survives: reporting the count happens after
    // this call, and resetting it here made every report say "0 rows".
}

void CsvRecorder::setHeader(const std::vector<std::string>& columns) {
    header_ = columns;
    headerPending_ = true;
    headerWritten_ = false;
}

void CsvRecorder::writeRow(const std::vector<double>& values) {
    if (!file_.is_open()) return;

    if (headerPending_ && !headerWritten_) {
        for (size_t i = 0; i < header_.size(); ++i) {
            if (i) file_ << ',';
            file_ << escapeCell(header_[i]);
        }
        file_ << '\n';
        headerWritten_ = true;
        headerPending_ = false;
    }

    char cell[64];
    const size_t columns = header_.empty() ? values.size() : header_.size();
    for (size_t i = 0; i < columns; ++i) {
        if (i) file_ << ',';
        if (i >= values.size()) continue;  // leave the cell empty
        const double v = values[i];
        if (v == v) {
            std::snprintf(cell, sizeof cell, "%.2f", v);
            file_ << cell;
        }
        // A NaN reading is written as an empty cell rather than "nan", so the
        // file stays loadable by a spreadsheet.
    }
    file_ << '\n';
    ++rowsWritten_;

    if (!file_.good()) error_ = "the log file became unwritable";
}

}  // namespace fanforge
