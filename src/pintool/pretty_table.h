#ifndef PRETTY_TABLE_H
#define PRETTY_TABLE_H

#include <algorithm>
#include <iomanip>
#include <ostream>
#include <string>
#include <vector>

class PrettyTable {
public:
    explicit PrettyTable(std::vector<std::string> headers)
        : _headers(std::move(headers)), _widths(_headers.size(), 0) {
        for (size_t i = 0; i < _headers.size(); i++) {
            _widths[i] = _headers[i].size();
        }
    }

    void add_row(std::vector<std::string> row) {
        for (size_t i = 0; i < row.size() && i < _widths.size(); i++) {
            _widths[i] = std::max(_widths[i], row[i].size());
        }
        _rows.push_back(std::move(row));
    }

    void print(std::ostream& os) const {
        _print_row(os, _headers);
        _print_separator(os);
        for (const auto& r : _rows) {
            _print_row(os, r);
        }
    }

private:
    void _print_row(std::ostream& os, const std::vector<std::string>& row) const {
        os << "| ";
        for (size_t i = 0; i < _widths.size(); i++) {
            const std::string& cell = (i < row.size()) ? row[i] : std::string();
            os << std::left << std::setw(_widths[i]) << cell << " | ";
        }
        os << "\n";
    }

    void _print_separator(std::ostream& os) const {
        os << "|";
        for (size_t i = 0; i < _widths.size(); i++) {
            os << std::string(_widths[i] + 2, '-') << "|";
        }
        os << "\n";
    }

    std::vector<std::string> _headers;
    std::vector<size_t> _widths;
    std::vector<std::vector<std::string>> _rows;
};

#endif
