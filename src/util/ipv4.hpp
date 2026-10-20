#pragma once

#include <string>
#include <vector>
#include <sstream>
#include <algorithm>
#include <cctype>

namespace pixelvr {

// Validates whether a string is a complete, strictly valid IPv4 address (e.g. "192.168.1.1").
inline bool isValidIpv4(const std::string& str) {
    if (str.empty()) return false;
    std::stringstream ss(str);
    std::string seg;
    int count = 0;
    while (std::getline(ss, seg, '.')) {
        ++count;
        if (seg.empty() || seg.length() > 3) return false;
        for (char c : seg) {
            if (!std::isdigit(static_cast<unsigned char>(c))) return false;
        }
        // Leading zeros not allowed if more than 1 digit (e.g. "01" is invalid, "0" is valid)
        if (seg.length() > 1 && seg[0] == '0') return false;
        try {
            int val = std::stoi(seg);
            if (val < 0 || val > 255) return false;
        } catch (...) {
            return false;
        }
    }
    // Must have exactly 4 octets and not end with a trailing dot
    return (count == 4 && str.back() != '.');
}

// Sanitizes and formats an in-progress or finalized IPv4 input:
// - Only digits '0'..'9' and '.' are accepted; any other characters are discarded.
// - At most 4 sections (octets).
// - Each section has leading zeros dropped (e.g. "001" -> "1", "050" -> "50", "00" -> "0").
// - Each section is clamped to max 255.
// - If !finalized, a trailing dot is preserved while typing (e.g. "192.168.").
// - If finalized, any trailing dot is trimmed and empty trailing sections are removed.
inline std::string sanitizeIpv4(const std::string& input, bool finalized = false) {
    if (input.empty()) return "";

    std::vector<std::string> sections;
    std::string current;
    bool trailingDot = false;

    for (char c : input) {
        if (c == '.') {
            if (!current.empty() && sections.size() < 4) {
                sections.push_back(current);
                current.clear();
                trailingDot = true;
            }
        } else if (std::isdigit(static_cast<unsigned char>(c))) {
            if (sections.size() >= 4) {
                continue;
            }
            if (current.length() < 3) {
                current += c;
                trailingDot = false;
            }
        }
    }
    if (!current.empty() && sections.size() < 4) {
        sections.push_back(current);
        trailingDot = false;
    }

    if (sections.empty() && !trailingDot) return "";

    std::string result;
    for (std::size_t i = 0; i < sections.size(); ++i) {
        std::string s = sections[i];
        if (!s.empty()) {
            while (s.length() > 1 && s[0] == '0') {
                s.erase(s.begin());
            }
            try {
                int val = std::stoi(s);
                if (val > 255) val = 255;
                s = std::to_string(val);
            } catch (...) {
                s = "255";
            }
        }

        if (i > 0) result += '.';
        result += s;
    }

    if (!finalized && trailingDot && sections.size() < 4 && !result.empty()) {
        result += '.';
    }

    if (finalized) {
        while (!result.empty() && result.back() == '.') {
            result.pop_back();
        }
    }

    return result;
}

} // namespace pixelvr
