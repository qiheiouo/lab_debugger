#include "lab/core/derived_field_engine.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <deque>
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
constexpr std::size_t maximumStatefulFunctions = 512;

enum class NodeKind { Number, Variable, Unary, Binary, Function };

struct Node {
    NodeKind kind{NodeKind::Number};
    double number{};
    char operation{};
    std::string text;
    std::vector<std::unique_ptr<Node>> children;
    std::size_t stateIndex{std::numeric_limits<std::size_t>::max()};
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
                           (node->text == "derivative" && count == 1) ||
                           (node->text == "integral" && count == 1) ||
                           (node->text == "unwrap_angle" && count == 1) ||
                           (node->text == "min" && count == 2) ||
                           (node->text == "max" && count == 2) ||
                           (node->text == "pow" && count == 2) ||
                           (node->text == "lowpass" && count == 2) ||
                           (node->text == "highpass" && count == 2) ||
                           (node->text == "moving_average" && count == 2) ||
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

bool isStatefulFunction(std::string_view name) {
    return name == "lowpass" || name == "highpass" ||
           name == "moving_average" || name == "derivative" ||
           name == "integral" || name == "unwrap_angle";
}

void assignStateIndices(Node& node, std::size_t& stateCount) {
    if (node.kind == NodeKind::Function && isStatefulFunction(node.text)) {
        node.stateIndex = stateCount++;
    }
    for (auto& child : node.children) assignStateIndices(*child, stateCount);
}

struct FunctionState {
    bool initialized{};
    Timestamp timestamp{};
    double previousInput{};
    double previousOutput{};
    double accumulated{};
    std::size_t windowLimit{};
    std::deque<double> window;
    double windowSum{};
};

enum class EvaluationStatus { Value, Warmup, Invalid };

struct EvaluationResult {
    EvaluationStatus status{EvaluationStatus::Invalid};
    double value{};
};

enum class StateUpdateKind { Replace, PushWindow };

struct StateUpdate {
    std::size_t index{};
    StateUpdateKind kind{StateUpdateKind::Replace};
    FunctionState replacement;
    double windowValue{};
    std::size_t windowLimit{};
    Timestamp timestamp{};
};

struct EvaluationContext {
    const std::unordered_map<std::string, double>& values;
    const std::vector<FunctionState>& states;
    std::vector<StateUpdate> updates;
    Timestamp timestamp{};
};

EvaluationResult valueResult(double value) {
    return std::isfinite(value)
               ? EvaluationResult{EvaluationStatus::Value, value}
               : EvaluationResult{};
}

EvaluationResult evaluate(Node& node, EvaluationContext& context) {
    switch (node.kind) {
    case NodeKind::Number:
        return valueResult(node.number);
    case NodeKind::Variable: {
        const auto found = context.values.find(node.text);
        if (found != context.values.end()) return valueResult(found->second);
        if (node.text == "pi") return valueResult(3.14159265358979323846);
        if (node.text == "e") return valueResult(2.71828182845904523536);
        return {};
    }
    case NodeKind::Unary: {
        const auto operand = evaluate(*node.children.front(), context);
        if (operand.status != EvaluationStatus::Value) return operand;
        return valueResult(node.operation == '-' ? -operand.value : operand.value);
    }
    case NodeKind::Binary: {
        const auto left = evaluate(*node.children[0], context);
        if (left.status != EvaluationStatus::Value) return left;
        const auto right = evaluate(*node.children[1], context);
        if (right.status != EvaluationStatus::Value) return right;
        double result{};
        switch (node.operation) {
        case '+': result = left.value + right.value; break;
        case '-': result = left.value - right.value; break;
        case '*': result = left.value * right.value; break;
        case '/':
            if (right.value == 0.0) return {};
            result = left.value / right.value;
            break;
        default: return {};
        }
        return valueResult(result);
    }
    case NodeKind::Function:
        break;
    }

    std::vector<double> arguments;
    arguments.reserve(node.children.size());
    for (auto& child : node.children) {
        const auto argument = evaluate(*child, context);
        if (argument.status != EvaluationStatus::Value) return argument;
        arguments.push_back(argument.value);
    }

    if (isStatefulFunction(node.text)) {
        if (node.stateIndex >= context.states.size()) return {};
        const auto& state = context.states[node.stateIndex];
        const auto input = arguments[0];
        FunctionState next = state;
        next.initialized = true;
        next.timestamp = context.timestamp;
        next.previousInput = input;

        if (node.text == "lowpass") {
            const auto alpha = arguments[1];
            if (!(alpha > 0.0 && alpha <= 1.0)) return {};
            const auto output = state.initialized
                                    ? alpha * input + (1.0 - alpha) * state.previousOutput
                                    : input;
            if (!std::isfinite(output)) return {};
            next.previousOutput = output;
            context.updates.push_back(
                {node.stateIndex, StateUpdateKind::Replace, std::move(next), 0.0, 0, 0});
            return valueResult(output);
        }
        if (node.text == "highpass") {
            const auto alpha = arguments[1];
            if (!(alpha > 0.0 && alpha <= 1.0)) return {};
            const auto output = state.initialized
                                    ? alpha * (state.previousOutput + input -
                                               state.previousInput)
                                    : 0.0;
            if (!std::isfinite(output)) return {};
            next.previousOutput = output;
            context.updates.push_back(
                {node.stateIndex, StateUpdateKind::Replace, std::move(next), 0.0, 0, 0});
            return valueResult(output);
        }
        if (node.text == "moving_average") {
            const auto requested = arguments[1];
            if (requested < 1.0 || requested > 4096.0 ||
                std::floor(requested) != requested) {
                return {};
            }
            const auto limit = static_cast<std::size_t>(requested);
            double sum = input;
            std::size_t count = 1;
            if (state.initialized && state.windowLimit == limit) {
                sum += state.windowSum;
                count += state.window.size();
                if (state.window.size() >= limit) {
                    sum -= state.window.front();
                    --count;
                }
            }
            const auto output = sum / static_cast<double>(count);
            if (!std::isfinite(output)) return {};
            context.updates.push_back({node.stateIndex,
                                       StateUpdateKind::PushWindow,
                                       {},
                                       input,
                                       limit,
                                       context.timestamp});
            return valueResult(output);
        }
        if (node.text == "derivative") {
            if (!state.initialized || context.timestamp <= state.timestamp) {
                context.updates.push_back(
                    {node.stateIndex, StateUpdateKind::Replace, std::move(next), 0.0, 0, 0});
                return {EvaluationStatus::Warmup, 0.0};
            }
            const auto elapsed = static_cast<double>(
                static_cast<long double>(context.timestamp) -
                static_cast<long double>(state.timestamp)) / 1'000'000'000.0;
            const auto output = (input - state.previousInput) / elapsed;
            if (!std::isfinite(output)) return {};
            next.previousOutput = output;
            context.updates.push_back(
                {node.stateIndex, StateUpdateKind::Replace, std::move(next), 0.0, 0, 0});
            return valueResult(output);
        }
        if (node.text == "integral") {
            if (state.initialized && context.timestamp <= state.timestamp) {
                return valueResult(state.accumulated);
            }
            auto output = 0.0;
            if (state.initialized) {
                const auto elapsed = static_cast<double>(
                    static_cast<long double>(context.timestamp) -
                    static_cast<long double>(state.timestamp)) / 1'000'000'000.0;
                output = state.accumulated +
                         0.5 * (state.previousInput + input) * elapsed;
            }
            if (!std::isfinite(output)) return {};
            next.accumulated = output;
            next.previousOutput = output;
            context.updates.push_back(
                {node.stateIndex, StateUpdateKind::Replace, std::move(next), 0.0, 0, 0});
            return valueResult(output);
        }
        if (node.text == "unwrap_angle") {
            auto output = input;
            if (state.initialized) {
                constexpr double twoPi = 6.28318530717958647692;
                const auto delta = std::remainder(input - state.previousInput, twoPi);
                output = state.previousOutput + delta;
            }
            if (!std::isfinite(output)) return {};
            next.previousOutput = output;
            context.updates.push_back(
                {node.stateIndex, StateUpdateKind::Replace, std::move(next), 0.0, 0, 0});
            return valueResult(output);
        }
        return {};
    }

    double result{};
    if (node.text == "abs") {
        result = std::abs(arguments[0]);
    } else if (node.text == "sqrt") {
        if (arguments[0] < 0.0) return {};
        result = std::sqrt(arguments[0]);
    } else if (node.text == "min") {
        result = std::min(arguments[0], arguments[1]);
    } else if (node.text == "max") {
        result = std::max(arguments[0], arguments[1]);
    } else if (node.text == "pow") {
        result = std::pow(arguments[0], arguments[1]);
    } else if (node.text == "clamp") {
        if (arguments[1] > arguments[2]) return {};
        result = std::clamp(arguments[0], arguments[1], arguments[2]);
    } else {
        return {};
    }
    return valueResult(result);
}

void commitStateUpdates(std::vector<FunctionState>& states,
                        const std::vector<StateUpdate>& updates) {
    for (const auto& update : updates) {
        if (update.index >= states.size()) continue;
        if (update.kind == StateUpdateKind::Replace) {
            states[update.index] = update.replacement;
            continue;
        }
        auto& state = states[update.index];
        if (!state.initialized || state.windowLimit != update.windowLimit) {
            state = {};
            state.initialized = true;
            state.windowLimit = update.windowLimit;
        }
        state.timestamp = update.timestamp;
        state.previousInput = update.windowValue;
        state.window.push_back(update.windowValue);
        state.windowSum += update.windowValue;
        while (state.window.size() > state.windowLimit) {
            state.windowSum -= state.window.front();
            state.window.pop_front();
        }
        state.previousOutput =
            state.windowSum / static_cast<double>(state.window.size());
    }
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
        std::vector<FunctionState> states;
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
    std::size_t totalStatefulFunctions = 0;
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
        std::size_t stateCount = 0;
        assignStateIndices(*parsed.root, stateCount);
        if (stateCount > maximumStatefulFunctions - totalStatefulFunctions) {
            result.issues.push_back({index,
                                     "too_many_stateful_functions",
                                     "at most 512 stateful functions are allowed",
                                     0});
            continue;
        }
        totalStatefulFunctions += stateCount;
        compiled.push_back({std::move(definition),
                            std::move(parsed.root),
                            std::move(dependencies),
                            std::vector<FunctionState>(stateCount)});
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
    for (auto& definition : impl_->compiled) {
        definition.states.assign(definition.states.size(), {});
    }
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
        EvaluationContext context{
            impl_->latestValues, definition.states, {}, trigger.timestamp};
        const auto value = evaluate(*definition.expression, context);
        if (value.status == EvaluationStatus::Warmup) {
            commitStateUpdates(definition.states, context.updates);
            impl_->latestValues.erase(definition.definition.name);
            changed.insert(definition.definition.name);
            continue;
        }
        if (value.status != EvaluationStatus::Value) {
            impl_->latestValues.erase(definition.definition.name);
            changed.insert(definition.definition.name);
            continue;
        }
        commitStateUpdates(definition.states, context.updates);
        impl_->latestValues[definition.definition.name] = value.value;
        changed.insert(definition.definition.name);
        output.push_back({trigger.timestamp,
                          "derived",
                          definition.definition.name,
                          value.value,
                          definition.definition.unit,
                          impl_->nextSequence++});
    }
    return output;
}

}  // namespace lab::core
