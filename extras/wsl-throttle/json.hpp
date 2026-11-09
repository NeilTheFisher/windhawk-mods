#pragma once

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace json {

struct Value {
    enum class Type { Null, Bool, Number, String, Array, Object };

    Type type = Type::Null;
    bool boolean = false;
    double number = 0.0;
    std::string string;
    std::vector<Value> array;
    std::vector<std::pair<std::string, Value>> object;

    const Value* get(std::string_view key) const;
    bool is_object() const { return type == Type::Object; }
};

// Parses one complete JSON document. Returns false and fills `error` otherwise.
bool parse(std::string_view text, Value& out, std::string& error);

// Compact serialization, member order preserved.
std::string serialize(const Value& value);

inline Value object_value() {
    Value value;
    value.type = Value::Type::Object;
    return value;
}

inline Value array_value() {
    Value value;
    value.type = Value::Type::Array;
    return value;
}

inline Value string_value(std::string text) {
    Value value;
    value.type = Value::Type::String;
    value.string = std::move(text);
    return value;
}

inline Value number_value(double number) {
    Value value;
    value.type = Value::Type::Number;
    value.number = number;
    return value;
}

inline Value bool_value(bool flag) {
    Value value;
    value.type = Value::Type::Bool;
    value.boolean = flag;
    return value;
}

void set(Value& object, std::string key, Value value);
void push(Value& array, Value value);

// Typed accessors that never trust the document: wrong type falls back, and
// numbers are clamped so a hostile file cannot push a value out of range.
std::string as_string(const Value* value, std::string_view fallback = {});
bool as_bool(const Value* value, bool fallback);
long long as_int(const Value* value, long long fallback, long long low, long long high);

}  // namespace json
