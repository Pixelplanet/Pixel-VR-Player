#pragma once
// Minimal JSON reader for Chrome DevTools Protocol messages (objects, arrays,
// strings with escapes, numbers, booleans, null). Not a general-purpose
// library: no streaming, and small documents are copied into a value tree.

#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace pvrjson {

struct Value {
    enum class Type { Null, Bool, Number, String, Array, Object };
    Type type = Type::Null;
    bool boolean = false;
    double number = 0.0;
    std::string str;
    std::vector<Value> array;
    std::vector<std::pair<std::string, Value>> object;

    const Value* get(std::string_view key) const {
        if (type != Type::Object) return nullptr;
        for (const auto& kv : object) {
            if (kv.first == key) return &kv.second;
        }
        return nullptr;
    }
    std::string getString(std::string_view key) const {
        const Value* v = get(key);
        return (v && v->type == Type::String) ? v->str : std::string();
    }
    double getNumber(std::string_view key, double fallback = 0.0) const {
        const Value* v = get(key);
        return (v && v->type == Type::Number) ? v->number : fallback;
    }
};

namespace detail {

inline void appendUtf8(std::string& out, uint32_t cp) {
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

struct Parser {
    std::string_view s;
    size_t i = 0;
    int depth = 0;

    void ws() {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\n' || s[i] == '\r' || s[i] == '\t')) ++i;
    }
    bool hex4(uint32_t& out) {
        if (i + 4 > s.size()) return false;
        out = 0;
        for (int k = 0; k < 4; ++k) {
            const char c = s[i++];
            out <<= 4;
            if (c >= '0' && c <= '9') out |= static_cast<uint32_t>(c - '0');
            else if (c >= 'a' && c <= 'f') out |= static_cast<uint32_t>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') out |= static_cast<uint32_t>(c - 'A' + 10);
            else return false;
        }
        return true;
    }
    bool string(std::string& out) {
        if (i >= s.size() || s[i] != '"') return false;
        ++i;
        while (i < s.size()) {
            const char c = s[i++];
            if (c == '"') return true;
            if (c != '\\') {
                out += c;
                continue;
            }
            if (i >= s.size()) return false;
            const char e = s[i++];
            switch (e) {
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                case '/': out += '/'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                case 'u': {
                    uint32_t cp = 0;
                    if (!hex4(cp)) return false;
                    if (cp >= 0xD800 && cp <= 0xDBFF && i + 6 <= s.size() && s[i] == '\\' && s[i + 1] == 'u') {
                        i += 2;
                        uint32_t lo = 0;
                        if (!hex4(lo)) return false;
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    }
                    appendUtf8(out, cp);
                    break;
                }
                default: return false;
            }
        }
        return false;
    }
    bool value(Value& v) {
        if (++depth > 64) return false;
        ws();
        if (i >= s.size()) return false;
        const char c = s[i];
        bool ok = false;
        if (c == '{') {
            v.type = Value::Type::Object;
            ++i;
            ws();
            if (i < s.size() && s[i] == '}') {
                ++i;
                ok = true;
            } else {
                for (;;) {
                    ws();
                    std::string key;
                    if (!string(key)) break;
                    ws();
                    if (i >= s.size() || s[i] != ':') break;
                    ++i;
                    v.object.emplace_back(std::move(key), Value{});
                    if (!value(v.object.back().second)) break;
                    ws();
                    if (i < s.size() && s[i] == ',') { ++i; continue; }
                    if (i < s.size() && s[i] == '}') { ++i; ok = true; }
                    break;
                }
            }
        } else if (c == '[') {
            v.type = Value::Type::Array;
            ++i;
            ws();
            if (i < s.size() && s[i] == ']') {
                ++i;
                ok = true;
            } else {
                for (;;) {
                    v.array.emplace_back();
                    if (!value(v.array.back())) break;
                    ws();
                    if (i < s.size() && s[i] == ',') { ++i; continue; }
                    if (i < s.size() && s[i] == ']') { ++i; ok = true; }
                    break;
                }
            }
        } else if (c == '"') {
            v.type = Value::Type::String;
            ok = string(v.str);
        } else if (s.compare(i, 4, "true") == 0) {
            v.type = Value::Type::Bool;
            v.boolean = true;
            i += 4;
            ok = true;
        } else if (s.compare(i, 5, "false") == 0) {
            v.type = Value::Type::Bool;
            i += 5;
            ok = true;
        } else if (s.compare(i, 4, "null") == 0) {
            i += 4;
            ok = true;
        } else {
            const size_t start = i;
            while (i < s.size() && (s[i] == '-' || s[i] == '+' || s[i] == '.' || s[i] == 'e' || s[i] == 'E' ||
                                    (s[i] >= '0' && s[i] <= '9'))) {
                ++i;
            }
            if (i > start) {
                v.type = Value::Type::Number;
                v.number = std::strtod(std::string(s.substr(start, i - start)).c_str(), nullptr);
                ok = true;
            }
        }
        --depth;
        return ok;
    }
};

}  // namespace detail

inline bool parse(std::string_view text, Value& out) {
    detail::Parser p{text};
    out = Value{};
    return p.value(out);
}

// Quotes and escapes a string for embedding in JSON.
inline std::string quote(std::string_view in) {
    std::string out = "\"";
    for (const char ch : in) {
        const unsigned char c = static_cast<unsigned char>(ch);
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    static const char* hexd = "0123456789abcdef";
                    out += "\\u00";
                    out += hexd[c >> 4];
                    out += hexd[c & 15];
                } else {
                    out += ch;
                }
        }
    }
    out += '"';
    return out;
}

// Writes `v` back out as compact JSON (integral numbers without a fraction).
inline std::string dump(const Value& v) {
    switch (v.type) {
        case Value::Type::Null: return "null";
        case Value::Type::Bool: return v.boolean ? "true" : "false";
        case Value::Type::Number: {
            const long long i = static_cast<long long>(v.number);
            if (static_cast<double>(i) == v.number) return std::to_string(i);
            return std::to_string(v.number);
        }
        case Value::Type::String: return quote(v.str);
        case Value::Type::Array: {
            std::string out = "[";
            for (size_t k = 0; k < v.array.size(); ++k) {
                if (k) out += ',';
                out += dump(v.array[k]);
            }
            return out + "]";
        }
        case Value::Type::Object: {
            std::string out = "{";
            for (size_t k = 0; k < v.object.size(); ++k) {
                if (k) out += ',';
                out += quote(v.object[k].first) + ":" + dump(v.object[k].second);
            }
            return out + "}";
        }
    }
    return "null";
}

}  // namespace pvrjson
