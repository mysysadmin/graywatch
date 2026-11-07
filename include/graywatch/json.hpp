#pragma once

#include <cstddef>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace graywatch::json {

// A small JSON document model for reading Graywatch's own events. It accepts
// RFC 8259 JSON, limits nesting depth, and never throws on lookups: missing
// members and type mismatches yield the supplied default.
class Value {
  public:
    enum class Type { null, boolean, number, string, array, object };

    Value() = default;
    static Value make_bool(bool value);
    static Value make_number(double value);
    static Value make_string(std::string value);
    static Value make_array(std::vector<Value> values);
    static Value make_object(std::map<std::string, Value> members);

    [[nodiscard]] Type type() const noexcept { return type_; }
    [[nodiscard]] bool is_null() const noexcept { return type_ == Type::null; }
    [[nodiscard]] bool is_object() const noexcept { return type_ == Type::object; }
    [[nodiscard]] bool is_array() const noexcept { return type_ == Type::array; }

    // Member access; returns a shared null value when absent or not an object.
    [[nodiscard]] const Value& operator[](std::string_view key) const;
    [[nodiscard]] bool contains(std::string_view key) const;

    [[nodiscard]] double number(double fallback = 0.0) const;
    [[nodiscard]] bool boolean(bool fallback = false) const;
    [[nodiscard]] const std::string& string() const;  // empty when not a string
    [[nodiscard]] const std::vector<Value>& array() const;  // empty when not an array
    using Members = std::map<std::string, Value, std::less<>>;
    [[nodiscard]] const Members& members() const;  // empty when not an object

    // Convenience: object member as number/string/bool.
    [[nodiscard]] double number(std::string_view key, double fallback) const {
        return (*this)[key].number(fallback);
    }
    [[nodiscard]] const std::string& string(std::string_view key) const {
        return (*this)[key].string();
    }
    [[nodiscard]] bool boolean(std::string_view key, bool fallback) const {
        return (*this)[key].boolean(fallback);
    }

  private:
    Type type_{Type::null};
    bool boolean_{};
    double number_{};
    std::shared_ptr<const std::string> string_;
    std::shared_ptr<const std::vector<Value>> array_;
    std::shared_ptr<const Members> object_;
};

// Throws std::runtime_error on malformed input or trailing garbage.
Value parse(std::string_view text);

// Escape a string for inclusion inside JSON double quotes.
std::string escape(std::string_view text);

}  // namespace graywatch::json
