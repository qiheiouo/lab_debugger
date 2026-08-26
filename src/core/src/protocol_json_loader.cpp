#include "lab/core/protocol_json_loader.hpp"

#include <charconv>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <map>
#include <stdexcept>
#include <string>
#include <variant>

namespace lab::core {
namespace {

struct JsonValue {
    using Array = std::vector<JsonValue>;
    using Object = std::map<std::string, JsonValue>;
    std::variant<std::nullptr_t, bool, double, std::string, Array, Object> value;

    [[nodiscard]] const Object* object() const { return std::get_if<Object>(&value); }
    [[nodiscard]] const Array* array() const { return std::get_if<Array>(&value); }
    [[nodiscard]] const std::string* string() const { return std::get_if<std::string>(&value); }
    [[nodiscard]] const double* number() const { return std::get_if<double>(&value); }
};

class JsonError final : public std::runtime_error {
public:
    JsonError(std::string message, std::size_t line, std::size_t column)
        : std::runtime_error(
              std::move(message) + " at line " + std::to_string(line) +
              ", column " + std::to_string(column)) {}
};

class JsonParser {
public:
    explicit JsonParser(std::string_view input) : input_(input) {}

    JsonValue parse() {
        skipWhitespace();
        auto value = parseValue();
        skipWhitespace();
        if (!atEnd()) fail("Unexpected trailing characters");
        return value;
    }

private:
    [[nodiscard]] bool atEnd() const noexcept { return position_ >= input_.size(); }
    [[nodiscard]] char peek() const { return atEnd() ? '\0' : input_[position_]; }

    char take() {
        if (atEnd()) fail("Unexpected end of input");
        const auto character = input_[position_++];
        if (character == '\n') {
            ++line_;
            column_ = 1;
        } else {
            ++column_;
        }
        return character;
    }

    [[noreturn]] void fail(const std::string& message) const {
        throw JsonError(message, line_, column_);
    }

    void skipWhitespace() {
        while (!atEnd() && (peek() == ' ' || peek() == '\t' ||
                            peek() == '\r' || peek() == '\n')) {
            take();
        }
    }

    JsonValue parseValue() {
        if (atEnd()) fail("Expected JSON value");
        switch (peek()) {
        case '{': return JsonValue{parseObject()};
        case '[': return JsonValue{parseArray()};
        case '"': return JsonValue{parseString()};
        case 't': parseLiteral("true"); return JsonValue{true};
        case 'f': parseLiteral("false"); return JsonValue{false};
        case 'n': parseLiteral("null"); return JsonValue{nullptr};
        default:
            if (peek() == '-' || (peek() >= '0' && peek() <= '9')) {
                return JsonValue{parseNumber()};
            }
            fail("Expected JSON value");
        }
    }

    JsonValue::Object parseObject() {
        JsonValue::Object result;
        take();
        skipWhitespace();
        if (peek() == '}') {
            take();
            return result;
        }
        while (true) {
            if (peek() != '"') fail("Expected object key");
            auto key = parseString();
            skipWhitespace();
            if (take() != ':') fail("Expected ':' after object key");
            skipWhitespace();
            if (!result.emplace(std::move(key), parseValue()).second) {
                fail("Duplicate object key");
            }
            skipWhitespace();
            const auto delimiter = take();
            if (delimiter == '}') break;
            if (delimiter != ',') fail("Expected ',' or '}' in object");
            skipWhitespace();
        }
        return result;
    }

    JsonValue::Array parseArray() {
        JsonValue::Array result;
        take();
        skipWhitespace();
        if (peek() == ']') {
            take();
            return result;
        }
        while (true) {
            result.push_back(parseValue());
            skipWhitespace();
            const auto delimiter = take();
            if (delimiter == ']') break;
            if (delimiter != ',') fail("Expected ',' or ']' in array");
            skipWhitespace();
        }
        return result;
    }

    static void appendUtf8(std::string& output, std::uint32_t codePoint) {
        if (codePoint <= 0x7FU) {
            output.push_back(static_cast<char>(codePoint));
        } else if (codePoint <= 0x7FFU) {
            output.push_back(static_cast<char>(0xC0U | (codePoint >> 6U)));
            output.push_back(static_cast<char>(0x80U | (codePoint & 0x3FU)));
        } else {
            output.push_back(static_cast<char>(0xE0U | (codePoint >> 12U)));
            output.push_back(static_cast<char>(0x80U | ((codePoint >> 6U) & 0x3FU)));
            output.push_back(static_cast<char>(0x80U | (codePoint & 0x3FU)));
        }
    }

    std::string parseString() {
        if (take() != '"') fail("Expected string");
        std::string result;
        while (!atEnd()) {
            const auto character = take();
            if (character == '"') return result;
            if (static_cast<unsigned char>(character) < 0x20U) {
                fail("Control character in string");
            }
            if (character != '\\') {
                result.push_back(character);
                continue;
            }
            const auto escape = take();
            switch (escape) {
            case '"': result.push_back('"'); break;
            case '\\': result.push_back('\\'); break;
            case '/': result.push_back('/'); break;
            case 'b': result.push_back('\b'); break;
            case 'f': result.push_back('\f'); break;
            case 'n': result.push_back('\n'); break;
            case 'r': result.push_back('\r'); break;
            case 't': result.push_back('\t'); break;
            case 'u': {
                std::uint32_t codePoint = 0;
                for (int index = 0; index < 4; ++index) {
                    const auto digit = take();
                    codePoint <<= 4U;
                    if (digit >= '0' && digit <= '9') codePoint |= digit - '0';
                    else if (digit >= 'a' && digit <= 'f') codePoint |= digit - 'a' + 10;
                    else if (digit >= 'A' && digit <= 'F') codePoint |= digit - 'A' + 10;
                    else fail("Invalid unicode escape");
                }
                if (codePoint >= 0xD800U && codePoint <= 0xDFFFU) {
                    fail("Surrogate pairs are not supported in protocol JSON");
                }
                appendUtf8(result, codePoint);
                break;
            }
            default: fail("Invalid string escape");
            }
        }
        fail("Unterminated string");
    }

    double parseNumber() {
        const auto start = position_;
        if (peek() == '-') take();
        if (peek() == '0') {
            take();
        } else {
            if (peek() < '1' || peek() > '9') fail("Invalid number");
            while (peek() >= '0' && peek() <= '9') take();
        }
        if (peek() == '.') {
            take();
            if (peek() < '0' || peek() > '9') fail("Invalid fractional number");
            while (peek() >= '0' && peek() <= '9') take();
        }
        if (peek() == 'e' || peek() == 'E') {
            take();
            if (peek() == '+' || peek() == '-') take();
            if (peek() < '0' || peek() > '9') fail("Invalid exponent");
            while (peek() >= '0' && peek() <= '9') take();
        }
        const std::string token(input_.substr(start, position_ - start));
        char* end = nullptr;
        const auto value = std::strtod(token.c_str(), &end);
        if (end != token.c_str() + token.size() || !std::isfinite(value)) {
            fail("Invalid finite number");
        }
        return value;
    }

    void parseLiteral(std::string_view literal) {
        for (const auto expected : literal) {
            if (take() != expected) fail("Invalid JSON literal");
        }
    }

    std::string_view input_;
    std::size_t position_{};
    std::size_t line_{1};
    std::size_t column_{1};
};

const JsonValue* member(const JsonValue::Object& object, const std::string& name) {
    const auto iterator = object.find(name);
    return iterator == object.end() ? nullptr : &iterator->second;
}

void issue(std::vector<ProtocolIssue>& issues, std::string code, std::string message) {
    issues.push_back({std::move(code), std::move(message)});
}

std::optional<std::string> stringMember(
    const JsonValue::Object& object,
    const std::string& name,
    std::vector<ProtocolIssue>& issues,
    bool required = false) {
    const auto* value = member(object, name);
    if (!value) {
        if (required) issue(issues, "json.required", "Missing required string: " + name);
        return std::nullopt;
    }
    if (!value->string()) {
        issue(issues, "json.type", "Expected string: " + name);
        return std::nullopt;
    }
    return *value->string();
}

std::optional<double> numberMember(
    const JsonValue::Object& object,
    const std::string& name,
    std::vector<ProtocolIssue>& issues,
    bool required = false) {
    const auto* value = member(object, name);
    if (!value) {
        if (required) issue(issues, "json.required", "Missing required number: " + name);
        return std::nullopt;
    }
    if (!value->number()) {
        issue(issues, "json.type", "Expected number: " + name);
        return std::nullopt;
    }
    return *value->number();
}

std::optional<std::size_t> sizeMember(
    const JsonValue::Object& object,
    const std::string& name,
    std::vector<ProtocolIssue>& issues,
    bool required = false) {
    const auto value = numberMember(object, name, issues, required);
    if (!value) return std::nullopt;
    if (*value < 0 || std::floor(*value) != *value ||
        *value > static_cast<double>(std::numeric_limits<std::size_t>::max())) {
        issue(issues, "json.integer", "Expected non-negative integer: " + name);
        return std::nullopt;
    }
    return static_cast<std::size_t>(*value);
}

std::optional<std::int64_t> signedMember(
    const JsonValue::Object& object,
    const std::string& name,
    std::vector<ProtocolIssue>& issues) {
    const auto value = numberMember(object, name, issues, false);
    if (!value) return std::nullopt;
    if (std::floor(*value) != *value ||
        *value < static_cast<double>(std::numeric_limits<std::int64_t>::min()) ||
        *value > static_cast<double>(std::numeric_limits<std::int64_t>::max())) {
        issue(issues, "json.integer", "Expected signed integer: " + name);
        return std::nullopt;
    }
    return static_cast<std::int64_t>(*value);
}

std::optional<FieldType> parseFieldType(
    const std::string& value,
    std::vector<ProtocolIssue>& issues) {
    static const std::map<std::string, FieldType> types{
        {"uint8", FieldType::UInt8}, {"int8", FieldType::Int8},
        {"uint16", FieldType::UInt16}, {"int16", FieldType::Int16},
        {"uint32", FieldType::UInt32}, {"int32", FieldType::Int32},
        {"float32", FieldType::Float32}, {"float64", FieldType::Float64},
        {"bool", FieldType::Boolean}, {"boolean", FieldType::Boolean},
        {"byte_array", FieldType::ByteArray}, {"bytes", FieldType::ByteArray}};
    const auto iterator = types.find(value);
    if (iterator == types.end()) {
        issue(issues, "field.type.unknown", "Unknown field type: " + value);
        return std::nullopt;
    }
    return iterator->second;
}

Endian parseEndian(
    const JsonValue::Object& object,
    std::vector<ProtocolIssue>& issues,
    Endian fallback = Endian::Little) {
    const auto value = stringMember(object, "endian", issues);
    if (!value) return fallback;
    if (*value == "little") return Endian::Little;
    if (*value == "big") return Endian::Big;
    issue(issues, "endian.unknown", "Endian must be 'little' or 'big'");
    return fallback;
}

std::optional<std::uint8_t> parseHeaderByte(
    const JsonValue& value,
    std::vector<ProtocolIssue>& issues) {
    if (const auto* number = value.number()) {
        if (*number >= 0 && *number <= 255 && std::floor(*number) == *number) {
            return static_cast<std::uint8_t>(*number);
        }
    } else if (const auto* text = value.string()) {
        auto input = std::string_view(*text);
        if (input.starts_with("0x") || input.starts_with("0X")) input.remove_prefix(2);
        unsigned parsed = 0;
        const auto result = std::from_chars(input.data(), input.data() + input.size(), parsed, 16);
        if (!input.empty() && result.ec == std::errc{} &&
            result.ptr == input.data() + input.size() && parsed <= 255) {
            return static_cast<std::uint8_t>(parsed);
        }
    }
    issue(issues, "frame.header.byte", "Header byte must be 0..255 or a hex string");
    return std::nullopt;
}

std::optional<std::int64_t> parseEnumKey(const std::string& key) {
    std::int64_t value = 0;
    const auto result = std::from_chars(key.data(), key.data() + key.size(), value, 10);
    if (result.ec != std::errc{} || result.ptr != key.data() + key.size()) {
        return std::nullopt;
    }
    return value;
}

ChecksumType parseChecksumType(
    const std::string& value,
    std::vector<ProtocolIssue>& issues) {
    if (value == "none") return ChecksumType::None;
    if (value == "sum8" || value == "checksum") return ChecksumType::Sum8;
    if (value == "crc8" || value == "crc8_atm") return ChecksumType::Crc8Atm;
    if (value == "crc16" || value == "crc16_modbus") return ChecksumType::Crc16Modbus;
    if (value == "crc16_ccitt_false") return ChecksumType::Crc16CcittFalse;
    issue(issues, "checksum.type.unknown", "Unknown checksum type: " + value);
    return ChecksumType::None;
}

}  // namespace

ProtocolLoadResult loadProtocolJson(std::string_view json) {
    ProtocolLoadResult result;
    JsonValue root;
    try {
        root = JsonParser(json).parse();
    } catch (const std::exception& error) {
        result.issues.push_back({"json.syntax", error.what()});
        return result;
    }
    const auto* rootObject = root.object();
    if (!rootObject) {
        result.issues.push_back({"json.root", "Protocol JSON root must be an object"});
        return result;
    }

    ProtocolDefinition definition;
    if (const auto name = stringMember(*rootObject, "name", result.issues, true)) {
        definition.name = *name;
    }
    if (const auto maximum = sizeMember(*rootObject, "maximum_frame_length", result.issues)) {
        definition.maximumFrameLength = *maximum;
    }

    const auto* frameValue = member(*rootObject, "frame");
    const auto* frame = frameValue ? frameValue->object() : nullptr;
    if (!frame) {
        issue(result.issues, "frame.required", "Missing required frame object");
    } else {
        const auto* headerValue = member(*frame, "header");
        const auto* header = headerValue ? headerValue->array() : nullptr;
        if (!header) {
            issue(result.issues, "frame.header.required", "frame.header must be an array");
        } else {
            for (const auto& value : *header) {
                if (const auto byte = parseHeaderByte(value, result.issues)) {
                    definition.header.push_back(*byte);
                }
            }
        }
        if (const auto fixed = sizeMember(*frame, "length", result.issues)) {
            definition.fixedFrameLength = *fixed;
        }
        if (const auto* value = member(*frame, "length_field")) {
            if (const auto* object = value->object()) {
                LengthFieldDefinition length;
                if (const auto offset = sizeMember(*object, "offset", result.issues, true)) {
                    length.byteOffset = *offset;
                }
                if (const auto type = stringMember(*object, "type", result.issues, true)) {
                    if (const auto parsed = parseFieldType(*type, result.issues)) {
                        length.type = *parsed;
                    }
                }
                length.endian = parseEndian(*object, result.issues);
                if (const auto adjustment = signedMember(*object, "adjustment", result.issues)) {
                    length.adjustment = *adjustment;
                }
                definition.lengthField = length;
            } else {
                issue(result.issues, "frame.length_field.type", "length_field must be an object");
            }
        }
    }

    std::size_t sequentialOffset = definition.header.size();
    if (const auto* fieldsValue = member(*rootObject, "fields")) {
        if (const auto* fields = fieldsValue->array()) {
            for (std::size_t index = 0; index < fields->size(); ++index) {
                const auto* object = (*fields)[index].object();
                if (!object) {
                    issue(result.issues, "field.object", "Each field must be an object");
                    continue;
                }
                FieldDefinition field;
                if (const auto name = stringMember(*object, "name", result.issues, true)) {
                    field.name = *name;
                }
                if (const auto type = stringMember(*object, "type", result.issues, true)) {
                    if (const auto parsed = parseFieldType(*type, result.issues)) {
                        field.type = *parsed;
                    }
                }
                field.byteOffset = sizeMember(*object, "byte_offset", result.issues)
                                       .value_or(sequentialOffset);
                if (field.type == FieldType::ByteArray) {
                    field.arrayLength = sizeMember(*object, "length", result.issues, true)
                                            .value_or(0);
                }
                field.endian = parseEndian(*object, result.issues);
                if (const auto scale = numberMember(*object, "scale", result.issues)) {
                    field.scale = *scale;
                }
                if (const auto valueOffset = numberMember(*object, "value_offset", result.issues)) {
                    field.valueOffset = *valueOffset;
                } else if (const auto legacyOffset = numberMember(*object, "offset", result.issues)) {
                    field.valueOffset = *legacyOffset;
                }
                if (const auto unit = stringMember(*object, "unit", result.issues)) {
                    field.unit = *unit;
                }
                if (const auto* enumValue = member(*object, "enum")) {
                    if (const auto* enumObject = enumValue->object()) {
                        for (const auto& [key, value] : *enumObject) {
                            const auto parsedKey = parseEnumKey(key);
                            const auto* label = value.string();
                            if (!parsedKey || !label) {
                                issue(result.issues, "field.enum", "Enum keys must be integers and values strings");
                                continue;
                            }
                            field.enumValues[*parsedKey] = *label;
                        }
                    } else {
                        issue(result.issues, "field.enum.type", "Field enum must be an object");
                    }
                }
                sequentialOffset = field.byteOffset + fieldByteSize(field);
                definition.fields.push_back(std::move(field));
            }
        } else {
            issue(result.issues, "fields.type", "fields must be an array");
        }
    } else {
        issue(result.issues, "fields.required", "Missing required fields array");
    }

    if (const auto* checksumValue = member(*rootObject, "checksum")) {
        if (const auto* object = checksumValue->object()) {
            if (const auto type = stringMember(*object, "type", result.issues, true)) {
                definition.checksum.type = parseChecksumType(*type, result.issues);
            }
            definition.checksum.byteOffset = sizeMember(*object, "offset", result.issues);
            definition.checksum.rangeStart = sizeMember(*object, "range_start", result.issues)
                                                 .value_or(0);
            definition.checksum.rangeLength = sizeMember(*object, "range_length", result.issues);
            definition.checksum.endian = parseEndian(*object, result.issues);
        } else {
            issue(result.issues, "checksum.object", "checksum must be an object");
        }
    }

    auto validation = validateProtocol(definition);
    result.issues.insert(
        result.issues.end(),
        std::make_move_iterator(validation.begin()),
        std::make_move_iterator(validation.end()));
    if (result.issues.empty()) {
        result.definition = std::move(definition);
    }
    return result;
}

}  // namespace lab::core
