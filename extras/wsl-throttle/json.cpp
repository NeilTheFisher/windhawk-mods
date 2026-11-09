#include "json.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>

namespace json {
namespace {

// Bounded so a corrupt or hostile document cannot exhaust the stack.
constexpr int kMaxDepth = 32;

class Parser {
  public:
    Parser(std::string_view text, std::string& error)
        : text_(text), error_(error) {}

    bool parse_document(Value& out) {
        skip_space();
        if (!parse_value(out, 0)) {
            return false;
        }
        skip_space();
        if (pos_ != text_.size()) {
            return fail("trailing data");
        }
        return true;
    }

  private:
    std::string_view text_;
    std::string& error_;
    size_t pos_ = 0;

    bool fail(const char* what) {
        if (error_.empty()) {
            error_ = std::string(what) + " at byte " + std::to_string(pos_);
        }
        return false;
    }

    void skip_space() {
        while (pos_ < text_.size()) {
            const char c = text_[pos_];
            if (c != ' ' && c != '\t' && c != '\n' && c != '\r') {
                return;
            }
            ++pos_;
        }
    }

    bool literal(std::string_view word) {
        if (text_.substr(pos_, word.size()) != word) {
            return fail("invalid literal");
        }
        pos_ += word.size();
        return true;
    }

    bool parse_value(Value& out, int depth) {
        if (depth > kMaxDepth) {
            return fail("nesting too deep");
        }
        if (pos_ >= text_.size()) {
            return fail("unexpected end of input");
        }
        switch (text_[pos_]) {
            case '{':
                return parse_object(out, depth);
            case '[':
                return parse_array(out, depth);
            case '"':
                out.type = Value::Type::String;
                return parse_string(out.string);
            case 't':
                out.type = Value::Type::Bool;
                out.boolean = true;
                return literal("true");
            case 'f':
                out.type = Value::Type::Bool;
                out.boolean = false;
                return literal("false");
            case 'n':
                out.type = Value::Type::Null;
                return literal("null");
            default:
                out.type = Value::Type::Number;
                return parse_number(out.number);
        }
    }

    bool parse_object(Value& out, int depth) {
        out.type = Value::Type::Object;
        ++pos_;  // '{'
        skip_space();
        if (pos_ < text_.size() && text_[pos_] == '}') {
            ++pos_;
            return true;
        }
        while (true) {
            skip_space();
            std::string key;
            if (pos_ >= text_.size() || text_[pos_] != '"') {
                return fail("expected object key");
            }
            if (!parse_string(key)) {
                return false;
            }
            skip_space();
            if (pos_ >= text_.size() || text_[pos_] != ':') {
                return fail("expected ':'");
            }
            ++pos_;
            skip_space();
            out.object.emplace_back(std::move(key), Value{});
            if (!parse_value(out.object.back().second, depth + 1)) {
                return false;
            }
            skip_space();
            if (pos_ < text_.size() && text_[pos_] == ',') {
                ++pos_;
                continue;
            }
            if (pos_ < text_.size() && text_[pos_] == '}') {
                ++pos_;
                return true;
            }
            return fail("expected ',' or '}'");
        }
    }

    bool parse_array(Value& out, int depth) {
        out.type = Value::Type::Array;
        ++pos_;  // '['
        skip_space();
        if (pos_ < text_.size() && text_[pos_] == ']') {
            ++pos_;
            return true;
        }
        while (true) {
            skip_space();
            out.array.emplace_back();
            if (!parse_value(out.array.back(), depth + 1)) {
                return false;
            }
            skip_space();
            if (pos_ < text_.size() && text_[pos_] == ',') {
                ++pos_;
                continue;
            }
            if (pos_ < text_.size() && text_[pos_] == ']') {
                ++pos_;
                return true;
            }
            return fail("expected ',' or ']'");
        }
    }

    static void append_utf8(std::string& out, unsigned code_point) {
        if (code_point < 0x80) {
            out.push_back(static_cast<char>(code_point));
        } else if (code_point < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (code_point >> 6)));
            out.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
        } else if (code_point < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (code_point >> 12)));
            out.push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (code_point >> 18)));
            out.push_back(static_cast<char>(0x80 | ((code_point >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
        }
    }

    bool parse_hex4(unsigned& value) {
        if (pos_ + 4 > text_.size()) {
            return fail("truncated \\u escape");
        }
        value = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = text_[pos_++];
            value <<= 4;
            if (c >= '0' && c <= '9') {
                value |= static_cast<unsigned>(c - '0');
            } else if (c >= 'a' && c <= 'f') {
                value |= static_cast<unsigned>(c - 'a' + 10);
            } else if (c >= 'A' && c <= 'F') {
                value |= static_cast<unsigned>(c - 'A' + 10);
            } else {
                return fail("invalid hex digit");
            }
        }
        return true;
    }

    bool parse_string(std::string& out) {
        ++pos_;  // opening quote
        while (true) {
            if (pos_ >= text_.size()) {
                return fail("unterminated string");
            }
            const char c = text_[pos_++];
            if (c == '"') {
                return true;
            }
            if (c != '\\') {
                out.push_back(c);
                continue;
            }
            if (pos_ >= text_.size()) {
                return fail("truncated escape");
            }
            switch (text_[pos_++]) {
                case '"':
                    out.push_back('"');
                    break;
                case '\\':
                    out.push_back('\\');
                    break;
                case '/':
                    out.push_back('/');
                    break;
                case 'b':
                    out.push_back('\b');
                    break;
                case 'f':
                    out.push_back('\f');
                    break;
                case 'n':
                    out.push_back('\n');
                    break;
                case 'r':
                    out.push_back('\r');
                    break;
                case 't':
                    out.push_back('\t');
                    break;
                case 'u': {
                    unsigned code_point = 0;
                    if (!parse_hex4(code_point)) {
                        return false;
                    }
                    if (code_point >= 0xD800 && code_point <= 0xDBFF &&
                        text_.substr(pos_, 2) == "\\u") {
                        pos_ += 2;
                        unsigned low = 0;
                        if (!parse_hex4(low)) {
                            return false;
                        }
                        code_point = 0x10000 + ((code_point - 0xD800) << 10) +
                                     (low - 0xDC00);
                    }
                    append_utf8(out, code_point);
                    break;
                }
                default:
                    return fail("invalid escape");
            }
        }
    }

    bool parse_number(double& out) {
        const size_t start = pos_;
        if (pos_ < text_.size() && text_[pos_] == '-') {
            ++pos_;
        }
        bool digits = false;
        while (pos_ < text_.size() && text_[pos_] >= '0' && text_[pos_] <= '9') {
            ++pos_;
            digits = true;
        }
        if (pos_ < text_.size() && text_[pos_] == '.') {
            ++pos_;
            while (pos_ < text_.size() && text_[pos_] >= '0' &&
                   text_[pos_] <= '9') {
                ++pos_;
                digits = true;
            }
        }
        if (digits && pos_ < text_.size() &&
            (text_[pos_] == 'e' || text_[pos_] == 'E')) {
            ++pos_;
            if (pos_ < text_.size() &&
                (text_[pos_] == '+' || text_[pos_] == '-')) {
                ++pos_;
            }
            while (pos_ < text_.size() && text_[pos_] >= '0' &&
                   text_[pos_] <= '9') {
                ++pos_;
            }
        }
        if (!digits) {
            return fail("invalid number");
        }
        const std::string literal(text_.substr(start, pos_ - start));
        out = std::strtod(literal.c_str(), nullptr);
        return true;
    }
};

void write_escaped(const std::string& text, std::string& out) {
    out.push_back('"');
    for (const char c : text) {
        switch (c) {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buffer[8];
                    std::snprintf(buffer, sizeof(buffer), "\\u%04x",
                                  static_cast<unsigned char>(c));
                    out += buffer;
                } else {
                    out.push_back(c);
                }
        }
    }
    out.push_back('"');
}

void write_number(double number, std::string& out) {
    char buffer[32];
    const double rounded = std::nearbyint(number);
    if (std::fabs(number) < 9.0e15 && rounded == number) {
        std::snprintf(buffer, sizeof(buffer), "%.0f", number);
    } else {
        std::snprintf(buffer, sizeof(buffer), "%.17g", number);
    }
    out += buffer;
}

void write_value(const Value& value, std::string& out) {
    switch (value.type) {
        case Value::Type::Null:
            out += "null";
            break;
        case Value::Type::Bool:
            out += value.boolean ? "true" : "false";
            break;
        case Value::Type::Number:
            write_number(value.number, out);
            break;
        case Value::Type::String:
            write_escaped(value.string, out);
            break;
        case Value::Type::Array:
            out.push_back('[');
            for (size_t i = 0; i < value.array.size(); ++i) {
                if (i != 0) {
                    out.push_back(',');
                }
                write_value(value.array[i], out);
            }
            out.push_back(']');
            break;
        case Value::Type::Object:
            out.push_back('{');
            for (size_t i = 0; i < value.object.size(); ++i) {
                if (i != 0) {
                    out.push_back(',');
                }
                write_escaped(value.object[i].first, out);
                out.push_back(':');
                write_value(value.object[i].second, out);
            }
            out.push_back('}');
            break;
    }
}

}  // namespace

const Value* Value::get(std::string_view key) const {
    if (type != Type::Object) {
        return nullptr;
    }
    for (const auto& entry : object) {
        if (entry.first == key) {
            return &entry.second;
        }
    }
    return nullptr;
}

bool parse(std::string_view text, Value& out, std::string& error) {
    Parser parser(text, error);
    return parser.parse_document(out);
}

std::string serialize(const Value& value) {
    std::string out;
    write_value(value, out);
    return out;
}

void set(Value& object, std::string key, Value value) {
    object.type = Value::Type::Object;
    object.object.emplace_back(std::move(key), std::move(value));
}

void push(Value& array, Value value) {
    array.type = Value::Type::Array;
    array.array.push_back(std::move(value));
}

std::string as_string(const Value* value, std::string_view fallback) {
    if (value == nullptr || value->type != Value::Type::String) {
        return std::string(fallback);
    }
    return value->string;
}

bool as_bool(const Value* value, bool fallback) {
    if (value == nullptr || value->type != Value::Type::Bool) {
        return fallback;
    }
    return value->boolean;
}

long long as_int(const Value* value, long long fallback, long long low,
                 long long high) {
    if (value == nullptr || value->type != Value::Type::Number ||
        !std::isfinite(value->number)) {
        return fallback;
    }
    long long result = static_cast<long long>(value->number);
    if (result < low) {
        result = low;
    }
    if (result > high) {
        result = high;
    }
    return result;
}

}  // namespace json
