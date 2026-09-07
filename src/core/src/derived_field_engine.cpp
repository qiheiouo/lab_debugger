#include "lab/core/derived_field_engine.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <functional>
#include <limits>
#include <mutex>
#include <optional>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace lab::core {
namespace {

constexpr std::size_t maximumDefinitions = 128;
constexpr std::size_t maximumNameLength = 256;
constexpr std::size_t maximumUnitLength = 128;
constexpr std::size_t maximumExpressionLength = 4096;
constexpr std::size_t maximumNodes = 512;
constexpr std::size_t maximumDepth = 64;

enum class NodeKind { Number, Variable, Unary, Binary, Function };

struct Node {
    NodeKind kind{NodeKind::Number};
    double number{};
    char operation{};
    std::string text;
    std::vector<std::unique_ptr<Node>> children;
};

struct ParseResult {
    std::unique_ptr<Node> root;
    std::string error;
    std::size_t position{};
};

bool isSpace(char character) {
    return character == ' ' || character == '\t' || character == '\r' ||
           character == '\n';
}

bool isIdentifierStart(char character) {
    const auto byte = static_cast<unsigned char>(character);
    return (byte >= static_cast<unsigned char>('a') &&
            byte <= static_cast<unsigned char>('z')) ||
           (byte >= static_cast<unsigned char>('A') &&
            byte <= static_cast<unsigned char>('Z')) ||
           character == '_' || byte >= 0x80U;
}

bool isIdentifierPart(char character) {
    const auto byte = static_cast<unsigned char>(character);
    return isIdentifierStart(character) ||
           (byte >= static_cast<unsigned char>('0') &&
            byte <= static_cast<unsigned char>('9')) ||
           character == '.' || character == ':' || character == '[' ||
           character == ']';
}

bool containsControlCharacter(std::string_view value) {
    return std::any_of(value.begin(), value.end(), [](char character) {
        const auto byte = static_cast<unsigned char>(character);
        return byte < 0x20U || byte == 0x7fU;
    });
}

bool canBeUniqueSuffixAlias(std::string_view value) {
    return !value.empty() && value.find_first_of(".:/[]-") == std::string_view::npos;
}

class Parser {
public:
    explicit Parser(std::string_view input) : input_(input) {}

    ParseResult parse() {
        auto root = parseExpression(0);
        skipSpaces();
        if (error_.empty() && position_ != input_.size()) {
            fail("unexpected character");
        }
        if (!error_.empty()) {
            return {nullptr, error_, errorPosition_};
        }
        return {std::move(root), {}, 0};
    }

private:
    std::unique_ptr<Node> parseExpression(std::size_t depth) {
        auto left = parseProduct(depth + 1);
        while (left && error_.empty()) {
            skipSpaces();
            if (!take('+') && !take('-')) {
                break;
            }
            const auto operation = input_[position_ - 1];
            auto right = parseProduct(depth + 1);
            if (!right) {
                if (error_.empty()) fail("expected expression after operator");
                return nullptr;
            }
            left = makeOperator(NodeKind::Binary,
                                operation,
                                std::move(left),
                                std::move(right));
        }
        return left;
    }

    std::unique_ptr<Node> parseProduct(std::size_t depth) {
        auto left = parseUnary(depth + 1);
        while (left && error_.empty()) {
            skipSpaces();
            if (!take('*') && !take('/')) {
                break;
            }
            const auto operation = input_[position_ - 1];
            auto right = parseUnary(depth + 1);
            if (!right) {
                if (error_.empty()) fail("expected expression after operator");
                return nullptr;
            }
            left = makeOperator(NodeKind::Binary,
                                operation,
                                std::move(left),
                                std::move(right));
        }
        return left;
    }

    std::unique_ptr<Node> parseUnary(std::size_t depth) {
        if (!checkDepth(depth)) return nullptr;
        skipSpaces();
        if (take('+') || take('-')) {
            const auto operation = input_[position_ - 1];
            auto operand = parseUnary(depth + 1);
            if (!operand) {
                if (error_.empty()) fail("expected expression after unary operator");
                return nullptr;
            }
            return makeOperator(NodeKind::Unary,
                                operation,
                                std::move(operand),
                                nullptr);
        }
        return parsePrimary(depth + 1);
    }

    std::unique_ptr<Node> parsePrimary(std::size_t depth) {
        if (!checkDepth(depth)) return nullptr;
        skipSpaces();
        if (position_ >= input_.size()) {
            fail("expected a number, variable, function, or parenthesized expression");
            return nullptr;
        }
        if (take('(')) {
            auto expression = parseExpression(depth + 1);
            skipSpaces();
            if (!take(')')) {
                fail("missing closing parenthesis");
                return nullptr;
            }
            return expression;
        }
        if (input_[position_] == '`') {
            return parseQuotedVariable();
        }
        const auto byte = static_cast<unsigned char>(input_[position_]);
        if ((byte >= static_cast<unsigned char>('0') &&
             byte <= static_cast<unsigned char>('9')) ||
            (input_[position_] == '.' && position_ + 1 < input_.size() &&
             input_[position_ + 1] >= '0' && input_[position_ + 1] <= '9')) {
            return parseNumber();
        }
        if (!isIdentifierStart(input_[position_])) {
            fail("expected a number, variable, function, or parenthesized expression");
            return nullptr;
        }
        return parseIdentifierOrFunction(depth + 1);
    }

    std::unique_ptr<Node> parseNumber() {
        double value{};
        const auto* first = input_.data() + position_;
        const auto* last = input_.data() + input_.size();
        const auto result = std::from_chars(first, last, value, std::chars_format::general);
        if (result.ec != std::errc{} || result.ptr == first || !std::isfinite(value)) {
            fail("invalid or non-finite number");
            return nullptr;
        }
        position_ = static_cast<std::size_t>(result.ptr - input_.data());
        auto node = makeNode(NodeKind::Number);
        if (node) node->number = value;
        return node;
    }

    std::unique_ptr<Node> parseQuotedVariable() {
        const auto openingPosition = position_;
        ++position_;
        std::string name;
        while (position_ < input_.size()) {
            const auto character = input_[position_++];
            if (character == '`') {
                if (name.empty()) {
                    failAt("quoted variable cannot be empty", openingPosition);
                    return nullptr;
                }
                auto node = makeNode(NodeKind::Variable);
                if (node) node->text = std::move(name);
                return node;
            }
            if (character == '\\') {
                if (position_ >= input_.size() ||
                    (input_[position_] != '`' && input_[position_] != '\\')) {
                    fail("only backtick and backslash can be escaped in a quoted variable");
                    return nullptr;
                }
                name.push_back(input_[position_++]);
            } else if (character == '\r' || character == '\n') {
                failAt("quoted variable cannot contain a line break", openingPosition);
                return nullptr;
            } else {
                name.push_back(character);
            }
            if (name.size() > maximumNameLength * 4) {
                failAt("quoted variable is too long", openingPosition);
                return nullptr;
            }
        }
        failAt("missing closing backtick", openingPosition);
        return nullptr;
    }

    std::unique_ptr<Node> parseIdentifierOrFunction(std::size_t depth) {
        const auto begin = position_++;
        while (position_ < input_.size() && isIdentifierPart(input_[position_])) {
            ++position_;
        }
        std::string name(input_.substr(begin, position_ - begin));
        skipSpaces();
        if (!take('(')) {
            auto node = makeNode(NodeKind::Variable);
            if (node) node->text = std::move(name);
            return node;
        }

        auto node = makeNode(NodeKind::Function);
        if (!node) return nullptr;
        node->text = std::move(name);
        skipSpaces();
        if (!take(')')) {
            while (error_.empty()) {
                auto argument = parseExpression(depth + 1);
                if (!argument) return nullptr;
                node->children.push_back(std::move(argument));
                skipSpaces();
                if (take(')')) break;
                if (!take(',')) {
                    fail("expected a comma or closing parenthesis");
                    return nullptr;
                }
            }
        }

        const auto count = node->children.size();
        const auto valid = (node->text == "abs" && count == 1) ||
                           (node->text == "sqrt" && count == 1) ||
                           (node->text == "min" && count == 2) ||
                           (node->text == "max" && count == 2) ||
                           (node->text == "pow" && count == 2) ||
                           (node->text == "clamp" && count == 3);
        if (!valid) {
            failAt("unknown function or invalid argument count", begin);
            return nullptr;
        }
        return node;
    }

    std::unique_ptr<Node> makeOperator(NodeKind kind,
                                       char operation,
                                       std::unique_ptr<Node> left,
                                       std::unique_ptr<Node> right) {
        auto node = makeNode(kind);
        if (!node) return nullptr;
        node->operation = operation;
        node->children.push_back(std::move(left));
        if (right) node->children.push_back(std::move(right));
        return node;
    }

    std::unique_ptr<Node> makeNode(NodeKind kind) {
        if (++nodes_ > maximumNodes) {
            fail("expression contains too many operations");
            return nullptr;
        }
        auto node = std::make_unique<Node>();
        node->kind = kind;
        return node;
    }

    bool checkDepth(std::size_t depth) {
        if (depth <= maximumDepth) return true;
        fail("expression nesting is too deep");
        return false;
    }

    void skipSpaces() {
        while (position_ < input_.size() && isSpace(input_[position_])) ++position_;
    }

    bool take(char expected) {
        if (position_ >= input_.size() || input_[position_] != expected) return false;
        ++position_;
        return true;
    }

    void fail(std::string message) {
        failAt(std::move(message), position_);
    }

    void failAt(std::string message, std::size_t position) {
        if (!error_.empty()) return;
        error_ = std::move(message);
        errorPosition_ = position;
    }

    std::string_view input_;
    std::size_t position_{};
    std::size_t nodes_{};
    std::string error_;
    std::size_t errorPosition_{};
};

void collectDependencies(const Node& node, std::unordered_set<std::string>& result) {
    if (node.kind == NodeKind::Variable && node.text != "pi" && node.text != "e") {
        result.insert(node.text);
    }
    for (const auto& child : node.children) {
        collectDependencies(*child, result);
    }
}

std::optional<double> evaluate(
    const Node& node,
    const std::unordered_map<std::string, double>& values) {
    switch (node.kind) {
    case NodeKind::Number:
        return node.number;
    case NodeKind::Variable: {
        const auto found = values.find(node.text);
        if (found != values.end()) return found->second;
        if (node.text == "pi") return 3.14159265358979323846;
        if (node.text == "e") return 2.71828182845904523536;
        return std::nullopt;
    }
    case NodeKind::Unary: {
        const auto operand = evaluate(*node.children.front(), values);
        if (!operand) return std::nullopt;
        return node.operation == '-' ? -*operand : *operand;
    }
    case NodeKind::Binary: {
        const auto left = evaluate(*node.children[0], values);
        const auto right = evaluate(*node.children[1], values);
        if (!left || !right) return std::nullopt;
        double result{};
        switch (node.operation) {
        case '+': result = *left + *right; break;
        case '-': result = *left - *right; break;
        case '*': result = *left * *right; break;
        case '/':
            if (*right == 0.0) return std::nullopt;
            result = *left / *right;
            break;
        default: return std::nullopt;
        }
        return std::isfinite(result) ? std::optional<double>{result} : std::nullopt;
    }
    case NodeKind::Function:
        break;
    }

    std::vector<double> arguments;
    arguments.reserve(node.children.size());
    for (const auto& child : node.children) {
        const auto argument = evaluate(*child, values);
        if (!argument) return std::nullopt;
        arguments.push_back(*argument);
    }
    double result{};
    if (node.text == "abs") {
        result = std::abs(arguments[0]);
    } else if (node.text == "sqrt") {
        if (arguments[0] < 0.0) return std::nullopt;
        result = std::sqrt(arguments[0]);
    } else if (node.text == "min") {
        result = std::min(arguments[0], arguments[1]);
    } else if (node.text == "max") {
        result = std::max(arguments[0], arguments[1]);
    } else if (node.text == "pow") {
        result = std::pow(arguments[0], arguments[1]);
    } else if (node.text == "clamp") {
        if (arguments[1] > arguments[2]) return std::nullopt;
        result = std::clamp(arguments[0], arguments[1], arguments[2]);
    } else {
        return std::nullopt;
    }
    return std::isfinite(result) ? std::optional<double>{result} : std::nullopt;
}

std::string qualifiedName(const DataSample& sample) {
    if (sample.sourceId.empty()) return sample.field;
    const auto prefix = sample.sourceId + ".";
    if (sample.field.starts_with(prefix)) return sample.field;
    return prefix + sample.field;
}

}  // namespace

struct DerivedFieldEngine::Impl {
    struct CompiledDefinition {
        DerivedFieldDefinition definition;
        std::unique_ptr<Node> expression;
        std::unordered_set<std::string> dependencies;
    };

    mutable std::mutex mutex;
    std::vector<CompiledDefinition> compiled;
    std::vector<std::size_t> evaluationOrder;
    std::unordered_set<std::string> requiredInputs;
    std::unordered_set<std::string> suffixAliases;
    std::unordered_map<std::string, std::string> aliasOwners;
    std::unordered_set<std::string> ambiguousAliases;
    std::unordered_map<std::string, double> latestValues;
    std::uint64_t nextSequence{};
};

DerivedFieldEngine::DerivedFieldEngine() : impl_(std::make_unique<Impl>()) {}

DerivedFieldEngine::~DerivedFieldEngine() = default;

DerivedFieldConfigurationResult DerivedFieldEngine::setDefinitions(
    std::vector<DerivedFieldDefinition> definitions) {
    DerivedFieldConfigurationResult result;
    if (definitions.size() > maximumDefinitions) {
        result.issues.push_back(
            {0, "too_many_definitions", "at most 128 derived fields are allowed", 0});
        return result;
    }

    std::vector<Impl::CompiledDefinition> compiled;
    compiled.reserve(definitions.size());
    std::unordered_map<std::string, std::size_t> indices;
    for (std::size_t index = 0; index < definitions.size(); ++index) {
        auto& definition = definitions[index];
        if (definition.name.empty()) {
            result.issues.push_back(
                {index, "empty_name", "derived field name cannot be empty", 0});
            continue;
        }
        if (definition.name.size() > maximumNameLength) {
            result.issues.push_back(
                {index, "name_too_long", "derived field name is too long", 0});
            continue;
        }
        if (containsControlCharacter(definition.name)) {
            result.issues.push_back(
                {index, "invalid_name", "derived field name contains a control character", 0});
            continue;
        }
        if (definition.unit.size() > maximumUnitLength ||
            containsControlCharacter(definition.unit)) {
            result.issues.push_back(
                {index, "invalid_unit", "derived field unit is too long or invalid", 0});
            continue;
        }
        if (definition.name == "pi" || definition.name == "e") {
            result.issues.push_back(
                {index, "reserved_name", "pi and e are reserved constants", 0});
            continue;
        }
        if (!indices.emplace(definition.name, index).second) {
            result.issues.push_back(
                {index, "duplicate_name", "derived field names must be unique", 0});
            continue;
        }
        if (definition.expression.empty()) {
            result.issues.push_back(
                {index, "empty_expression", "derived field expression cannot be empty", 0});
            continue;
        }
        if (definition.expression.size() > maximumExpressionLength) {
            result.issues.push_back(
                {index, "expression_too_long", "derived field expression is too long", 0});
            continue;
        }
        auto parsed = Parser(definition.expression).parse();
        if (!parsed.root) {
            result.issues.push_back(
                {index, "invalid_expression", parsed.error, parsed.position});
            continue;
        }
        std::unordered_set<std::string> dependencies;
        collectDependencies(*parsed.root, dependencies);
        compiled.push_back(
            {std::move(definition), std::move(parsed.root), std::move(dependencies)});
    }
    if (!result.success()) return result;

    std::vector<std::size_t> order;
    order.reserve(compiled.size());
    std::vector<std::uint8_t> state(compiled.size());
    std::function<bool(std::size_t)> visit = [&](std::size_t index) {
        if (state[index] == 2U) return true;
        if (state[index] == 1U) {
            result.issues.push_back(
                {index, "dependency_cycle", "derived fields contain a dependency cycle", 0});
            return false;
        }
        state[index] = 1U;
        for (const auto& dependency : compiled[index].dependencies) {
            const auto found = indices.find(dependency);
            if (found != indices.end() && !visit(found->second)) return false;
        }
        state[index] = 2U;
        order.push_back(index);
        return true;
    };
    for (std::size_t index = 0; index < compiled.size(); ++index) {
        if (!visit(index)) return result;
    }

    std::unordered_set<std::string> requiredInputs;
    std::unordered_set<std::string> suffixAliases;
    for (const auto& definition : compiled) {
        for (const auto& dependency : definition.dependencies) {
            if (!indices.contains(dependency)) {
                requiredInputs.insert(dependency);
                if (canBeUniqueSuffixAlias(dependency)) {
                    suffixAliases.insert(dependency);
                }
            }
        }
    }

    std::scoped_lock lock(impl_->mutex);
    impl_->compiled = std::move(compiled);
    impl_->evaluationOrder = std::move(order);
    impl_->requiredInputs = std::move(requiredInputs);
    impl_->suffixAliases = std::move(suffixAliases);
    impl_->aliasOwners.clear();
    impl_->ambiguousAliases.clear();
    impl_->latestValues.clear();
    impl_->nextSequence = 0;
    return result;
}

void DerivedFieldEngine::clear() {
    std::scoped_lock lock(impl_->mutex);
    impl_->compiled.clear();
    impl_->evaluationOrder.clear();
    impl_->requiredInputs.clear();
    impl_->suffixAliases.clear();
    impl_->aliasOwners.clear();
    impl_->ambiguousAliases.clear();
    impl_->latestValues.clear();
    impl_->nextSequence = 0;
}

void DerivedFieldEngine::resetValues() {
    std::scoped_lock lock(impl_->mutex);
    impl_->latestValues.clear();
    impl_->aliasOwners.clear();
    impl_->ambiguousAliases.clear();
    impl_->nextSequence = 0;
}

std::vector<DerivedFieldDefinition> DerivedFieldEngine::definitions() const {
    std::scoped_lock lock(impl_->mutex);
    std::vector<DerivedFieldDefinition> result;
    result.reserve(impl_->compiled.size());
    for (const auto& definition : impl_->compiled) {
        result.push_back(definition.definition);
    }
    return result;
}

std::vector<DataSample> DerivedFieldEngine::consume(const DataSample& sample) {
    return consumeBatch(std::span(&sample, 1));
}

std::vector<DataSample> DerivedFieldEngine::consumeBatch(
    std::span<const DataSample> samples) {
    if (samples.empty()) return {};
    std::scoped_lock lock(impl_->mutex);
    if (impl_->compiled.empty()) return {};

    std::unordered_set<std::string> changed;
    for (const auto& sample : samples) {
        const auto qualified = qualifiedName(sample);
        const auto updateValue = [this, &sample, &changed](const std::string& name) {
            if (!impl_->requiredInputs.contains(name)) return;
            if (std::isfinite(sample.value)) {
                impl_->latestValues[name] = sample.value;
            } else {
                impl_->latestValues.erase(name);
            }
            changed.insert(name);
        };
        updateValue(sample.field);
        if (qualified != sample.field) updateValue(qualified);
        for (const auto& alias : impl_->suffixAliases) {
            const auto matches = sample.field == alias ||
                                 qualified.ends_with("." + alias);
            if (!matches) continue;
            if (impl_->ambiguousAliases.contains(alias)) {
                impl_->latestValues.erase(alias);
                changed.insert(alias);
                continue;
            }
            const auto [owner, inserted] =
                impl_->aliasOwners.emplace(alias, qualified);
            if (!inserted && owner->second != qualified) {
                impl_->ambiguousAliases.insert(alias);
                impl_->latestValues.erase(alias);
                changed.insert(alias);
                continue;
            }
            if (std::isfinite(sample.value)) {
                impl_->latestValues[alias] = sample.value;
            } else {
                impl_->latestValues.erase(alias);
            }
            changed.insert(alias);
        }
    }

    const auto& trigger = samples.back();
    std::vector<DataSample> output;
    for (const auto index : impl_->evaluationOrder) {
        auto& definition = impl_->compiled[index];
        const auto dependencyChanged = definition.dependencies.empty() ||
            std::any_of(definition.dependencies.begin(),
                        definition.dependencies.end(),
                        [&changed](const auto& dependency) {
                            return changed.contains(dependency);
                        });
        if (!dependencyChanged) continue;
        const auto value = evaluate(*definition.expression, impl_->latestValues);
        if (!value) {
            impl_->latestValues.erase(definition.definition.name);
            changed.insert(definition.definition.name);
            continue;
        }
        impl_->latestValues[definition.definition.name] = *value;
        changed.insert(definition.definition.name);
        output.push_back({trigger.timestamp,
                          "derived",
                          definition.definition.name,
                          *value,
                          definition.definition.unit,
                          impl_->nextSequence++});
    }
    return output;
}

}  // namespace lab::core
