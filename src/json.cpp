#include "graywatch/json.hpp"

#include <charconv>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace graywatch::json {
namespace {

const Value& null_value() {
    static const Value value;
    return value;
}

const std::string& empty_string() {
    static const std::string value;
    return value;
}

const std::vector<Value>& empty_array() {
    static const std::vector<Value> value;
    return value;
}

constexpr int maximum_depth = 64;

class Parser {
  public:
    explicit Parser(std::string_view text) : text_(text) {}

    Value document() {
        Value value = parse_value(0);
        skip_space();
        if (position_ != text_.size()) fail("trailing characters");
        return value;
    }

  private:
    [[noreturn]] void fail(const char* message) const {
        throw std::runtime_error("JSON parse error at offset " +
                                 std::to_string(position_) + ": " + message);
    }

    void skip_space() {
        while (position_ < text_.size() &&
               (text_[position_] == ' ' || text_[position_] == '\t' ||
                text_[position_] == '\n' || text_[position_] == '\r')) {
            ++position_;
        }
    }

    bool consume(char expected) {
        skip_space();
        if (position_ < text_.size() && text_[position_] == expected) {
            ++position_;
            return true;
        }
        return false;
    }

    void expect(char expected) {
        if (!consume(expected)) fail("unexpected character");
    }

    bool literal(std::string_view word) {
        if (text_.substr(position_, word.size()) == word) {
            position_ += word.size();
            return true;
        }
        return false;
    }

    Value parse_value(int depth) {
        if (depth > maximum_depth) fail("nesting too deep");
        skip_space();
        if (position_ >= text_.size()) fail("unexpected end of input");
        const char character = text_[position_];
        if (character == '{') return parse_object(depth);
        if (character == '[') return parse_array(depth);
        if (character == '"') return Value::make_string(parse_string());
        if (literal("true")) return Value::make_bool(true);
        if (literal("false")) return Value::make_bool(false);
        if (literal("null")) return {};
        return Value::make_number(parse_number());
    }

    Value parse_object(int depth) {
        expect('{');
        std::map<std::string, Value> members;
        if (consume('}')) return Value::make_object(std::move(members));
        do {
            skip_space();
            if (position_ >= text_.size() || text_[position_] != '"') fail("expected key");
            std::string key = parse_string();
            expect(':');
            members[std::move(key)] = parse_value(depth + 1);
        } while (consume(','));
        expect('}');
        return Value::make_object(std::move(members));
    }

    Value parse_array(int depth) {
        expect('[');
        std::vector<Value> values;
        if (consume(']')) return Value::make_array(std::move(values));
        do {
            values.push_back(parse_value(depth + 1));
        } while (consume(','));
        expect(']');
        return Value::make_array(std::move(values));
    }

    unsigned int hex4() {
        if (position_ + 4 > text_.size()) fail("short unicode escape");
        unsigned int value{};
        const auto* begin = text_.data() + position_;
        const auto [end, error] = std::from_chars(begin, begin + 4, value, 16);
        if (error != std::errc{} || end != begin + 4) fail("bad unicode escape");
        position_ += 4;
        return value;
    }

    static void append_utf8(std::string& output, unsigned int code) {
        if (code < 0x80) {
            output.push_back(static_cast<char>(code));
        } else if (code < 0x800) {
            output.push_back(static_cast<char>(0xc0 | (code >> 6)));
            output.push_back(static_cast<char>(0x80 | (code & 0x3f)));
        } else if (code < 0x10000) {
            output.push_back(static_cast<char>(0xe0 | (code >> 12)));
            output.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3f)));
            output.push_back(static_cast<char>(0x80 | (code & 0x3f)));
        } else {
            output.push_back(static_cast<char>(0xf0 | (code >> 18)));
            output.push_back(static_cast<char>(0x80 | ((code >> 12) & 0x3f)));
            output.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3f)));
            output.push_back(static_cast<char>(0x80 | (code & 0x3f)));
        }
    }

    std::string parse_string() {
        ++position_;  // opening quote
        std::string output;
        while (true) {
            if (position_ >= text_.size()) fail("unterminated string");
            const char character = text_[position_++];
            if (character == '"') return output;
            if (static_cast<unsigned char>(character) < 0x20) fail("control character");
            if (character != '\\') {
                output.push_back(character);
                continue;
            }
            if (position_ >= text_.size()) fail("unterminated escape");
            const char escaped = text_[position_++];
            switch (escaped) {
                case '"': output.push_back('"'); break;
                case '\\': output.push_back('\\'); break;
                case '/': output.push_back('/'); break;
                case 'b': output.push_back('\b'); break;
                case 'f': output.push_back('\f'); break;
                case 'n': output.push_back('\n'); break;
                case 'r': output.push_back('\r'); break;
                case 't': output.push_back('\t'); break;
                case 'u': {
                    unsigned int code = hex4();
                    if (code >= 0xd800 && code <= 0xdbff &&
                        text_.substr(position_, 2) == "\\u") {
                        position_ += 2;
                        const unsigned int low = hex4();
                        if (low >= 0xdc00 && low <= 0xdfff) {
                            code = 0x10000 + ((code - 0xd800) << 10) + (low - 0xdc00);
                        } else {
                            fail("bad surrogate pair");
                        }
                    }
                    append_utf8(output, code);
                    break;
                }
                default: fail("bad escape");
            }
        }
    }

    double parse_number() {
        const std::size_t start = position_;
        if (position_ < text_.size() && text_[position_] == '-') ++position_;
        auto digits = [&] {
            const std::size_t begin = position_;
            while (position_ < text_.size() && text_[position_] >= '0' &&
                   text_[position_] <= '9') {
                ++position_;
            }
            return position_ - begin;
        };
        if (digits() == 0) fail("invalid value");
        if (position_ < text_.size() && text_[position_] == '.') {
            ++position_;
            if (digits() == 0) fail("invalid fraction");
        }
        if (position_ < text_.size() && (text_[position_] == 'e' || text_[position_] == 'E')) {
            ++position_;
            if (position_ < text_.size() && (text_[position_] == '+' || text_[position_] == '-'))
                ++position_;
            if (digits() == 0) fail("invalid exponent");
        }
        // strtod on a bounded copy: from_chars for double is not available in
        // every supported standard library.
        const std::string number(text_.substr(start, position_ - start));
        return std::strtod(number.c_str(), nullptr);
    }

    std::string_view text_;
    std::size_t position_{};
};

}  // namespace

Value Value::make_bool(bool value) {
    Value result;
    result.type_ = Type::boolean;
    result.boolean_ = value;
    return result;
}

Value Value::make_number(double value) {
    Value result;
    result.type_ = Type::number;
    result.number_ = value;
    return result;
}

Value Value::make_string(std::string value) {
    Value result;
    result.type_ = Type::string;
    result.string_ = std::make_shared<const std::string>(std::move(value));
    return result;
}

Value Value::make_array(std::vector<Value> values) {
    Value result;
    result.type_ = Type::array;
    result.array_ = std::make_shared<const std::vector<Value>>(std::move(values));
    return result;
}

Value Value::make_object(std::map<std::string, Value> members) {
    Value result;
    result.type_ = Type::object;
    result.object_ = std::make_shared<const Members>(members.begin(), members.end());
    return result;
}

const Value& Value::operator[](std::string_view key) const {
    if (type_ != Type::object) return null_value();
    const auto found = object_->find(key);
    return found == object_->end() ? null_value() : found->second;
}

bool Value::contains(std::string_view key) const {
    return type_ == Type::object && object_->find(key) != object_->end();
}

double Value::number(double fallback) const {
    return type_ == Type::number ? number_ : fallback;
}

bool Value::boolean(bool fallback) const {
    return type_ == Type::boolean ? boolean_ : fallback;
}

const std::string& Value::string() const {
    return type_ == Type::string ? *string_ : empty_string();
}

const std::vector<Value>& Value::array() const {
    return type_ == Type::array ? *array_ : empty_array();
}

const Value::Members& Value::members() const {
    static const Members empty;
    return type_ == Type::object ? *object_ : empty;
}

Value parse(std::string_view text) {
    return Parser(text).document();
}

std::string escape(std::string_view text) {
    std::ostringstream output;
    for (const unsigned char character : text) {
        switch (character) {
            case '"': output << "\\\""; break;
            case '\\': output << "\\\\"; break;
            case '\n': output << "\\n"; break;
            case '\r': output << "\\r"; break;
            case '\t': output << "\\t"; break;
            default:
                if (character < 0x20) {
                    output << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                           << static_cast<int>(character) << std::dec;
                } else {
                    output << character;
                }
        }
    }
    return output.str();
}

}  // namespace graywatch::json
