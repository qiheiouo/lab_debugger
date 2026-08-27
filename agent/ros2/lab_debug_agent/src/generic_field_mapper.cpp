#include "lab_debug_agent/generic_field_mapper.hpp"

#include <rclcpp/serialization.hpp>
#include <rclcpp/typesupport_helpers.hpp>
#include <rcpputils/shared_library.hpp>
#include <rcutils/error_handling.h>
#include <rosidl_runtime_cpp/message_initialization.hpp>
#include <rosidl_typesupport_cpp/identifier.hpp>
#include <rosidl_typesupport_introspection_cpp/field_types.hpp>
#include <rosidl_typesupport_introspection_cpp/identifier.hpp>
#include <rosidl_typesupport_introspection_cpp/message_introspection.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace lab_debug_agent {
namespace {

using FieldValue = lab::core::agent::FieldValue;
using FieldData = lab::core::agent::FieldData;
using MessageMember = rosidl_typesupport_introspection_cpp::MessageMember;
using MessageMembers = rosidl_typesupport_introspection_cpp::MessageMembers;

constexpr std::size_t maximumDepth = 32;
constexpr std::size_t maximumFields = 4096;
constexpr std::size_t maximumArrayElements = 1024;
constexpr std::size_t maximumStringBytes = 1U << 20U;
constexpr std::uint64_t maximumExactInteger = 1ULL << 53U;

struct TypeSupportEntry {
    std::shared_ptr<rcpputils::SharedLibrary> serializationLibrary;
    std::shared_ptr<rcpputils::SharedLibrary> introspectionLibrary;
    const rosidl_message_type_support_t* serializationHandle{};
    const MessageMembers* members{};
    std::string serializationError;
    std::string introspectionError;
};

class TypeSupportCache final {
public:
    std::shared_ptr<const TypeSupportEntry> get(const std::string& type) {
        std::scoped_lock lock(mutex_);
        const auto existing = entries_.find(type);
        if (existing != entries_.end()) return existing->second;

        auto entry = std::make_shared<TypeSupportEntry>();
        try {
            entry->serializationLibrary = rclcpp::get_typesupport_library(
                type, rosidl_typesupport_cpp::typesupport_identifier);
            entry->serializationHandle = rclcpp::get_typesupport_handle(
                type,
                rosidl_typesupport_cpp::typesupport_identifier,
                *entry->serializationLibrary);
            if (!entry->serializationHandle) {
                throw std::runtime_error(
                    "loaded C++ typesupport contains a null handle");
            }
        } catch (const std::exception& exception) {
            if (rcutils_error_is_set()) rcutils_reset_error();
            entry->serializationHandle = nullptr;
            entry->serializationError = exception.what();
        }
        try {
            entry->introspectionLibrary = rclcpp::get_typesupport_library(
                type,
                rosidl_typesupport_introspection_cpp::typesupport_identifier);
            const auto* introspectionHandle = rclcpp::get_typesupport_handle(
                type,
                rosidl_typesupport_introspection_cpp::typesupport_identifier,
                *entry->introspectionLibrary);
            if (!introspectionHandle || !introspectionHandle->data) {
                throw std::runtime_error(
                    "loaded introspection typesupport contains a null handle");
            }
            entry->members = static_cast<const MessageMembers*>(
                introspectionHandle->data);
            if (!entry->members->init_function || !entry->members->fini_function) {
                throw std::runtime_error(
                    "introspection metadata has no message lifecycle functions");
            }
        } catch (const std::exception& exception) {
            if (rcutils_error_is_set()) rcutils_reset_error();
            entry->members = nullptr;
            entry->introspectionError = exception.what();
        }
        entries_.emplace(type, entry);
        return entry;
    }

private:
    std::mutex mutex_;
    std::unordered_map<std::string, std::shared_ptr<const TypeSupportEntry>> entries_;
};

TypeSupportCache& typeSupportCache() {
    static TypeSupportCache cache;
    return cache;
}

std::string boundedReason(std::string value) {
    std::replace(value.begin(), value.end(), '\r', ' ');
    std::replace(value.begin(), value.end(), '\n', ' ');
    constexpr std::size_t maximumReasonBytes = 512;
    if (value.size() > maximumReasonBytes) {
        value.resize(maximumReasonBytes - 3);
        value.append("...");
    }
    return value;
}

class MessageStorage final {
public:
    explicit MessageStorage(const MessageMembers& members)
        : members_(members), data_(::operator new(members.size_of_)) {
        try {
            members_.init_function(
                data_, rosidl_runtime_cpp::MessageInitialization::ALL);
            initialized_ = true;
        } catch (...) {
            ::operator delete(data_);
            data_ = nullptr;
            throw;
        }
    }

    ~MessageStorage() {
        if (initialized_) members_.fini_function(data_);
        ::operator delete(data_);
    }

    MessageStorage(const MessageStorage&) = delete;
    MessageStorage& operator=(const MessageStorage&) = delete;

    [[nodiscard]] void* get() noexcept { return data_; }
    [[nodiscard]] const void* get() const noexcept { return data_; }

private:
    const MessageMembers& members_;
    void* data_{};
    bool initialized_{};
};

class Warnings final {
public:
    void add(std::string value) {
        if (!seen_.insert(value).second) return;
        if (messages_.size() < 4) messages_.push_back(std::move(value));
        ++count_;
    }

    [[nodiscard]] std::string format(const std::string& type) const {
        if (count_ == 0) return {};
        std::string result = "Structured field mapping for " + type +
                             " was partial: ";
        for (std::size_t index = 0; index < messages_.size(); ++index) {
            if (index != 0) result += "; ";
            result += messages_[index];
        }
        if (count_ > messages_.size()) {
            result += "; " + std::to_string(count_ - messages_.size()) +
                      " more issue(s)";
        }
        return result;
    }

private:
    std::unordered_set<std::string> seen_;
    std::vector<std::string> messages_;
    std::size_t count_{};
};

std::string childPath(const std::string& parent, std::string_view child) {
    if (parent.empty()) return std::string(child);
    std::string result;
    result.reserve(parent.size() + child.size() + 1);
    result.append(parent);
    result.push_back('.');
    result.append(child);
    return result;
}

std::string indexedPath(const std::string& path, std::size_t index) {
    return path + '[' + std::to_string(index) + ']';
}

const MessageMembers* nestedMembers(const MessageMember& member) {
    if (!member.members_ || !member.members_->data) return nullptr;
    return static_cast<const MessageMembers*>(member.members_->data);
}

std::size_t scalarSize(const MessageMember& member) {
    using namespace rosidl_typesupport_introspection_cpp;
    switch (member.type_id_) {
    case ROS_TYPE_FLOAT: return sizeof(float);
    case ROS_TYPE_DOUBLE: return sizeof(double);
    case ROS_TYPE_LONG_DOUBLE: return sizeof(long double);
    case ROS_TYPE_CHAR:
    case ROS_TYPE_UINT8:
    case ROS_TYPE_INT8: return sizeof(std::uint8_t);
    case ROS_TYPE_OCTET: return sizeof(unsigned char);
    case ROS_TYPE_BOOLEAN: return sizeof(bool);
    case ROS_TYPE_WCHAR: return sizeof(char16_t);
    case ROS_TYPE_UINT16:
    case ROS_TYPE_INT16: return sizeof(std::uint16_t);
    case ROS_TYPE_UINT32:
    case ROS_TYPE_INT32: return sizeof(std::uint32_t);
    case ROS_TYPE_UINT64:
    case ROS_TYPE_INT64: return sizeof(std::uint64_t);
    case ROS_TYPE_STRING: return sizeof(std::string);
    case ROS_TYPE_WSTRING: return sizeof(std::u16string);
    case ROS_TYPE_MESSAGE: {
        const auto* members = nestedMembers(member);
        return members ? members->size_of_ : 0;
    }
    default: return 0;
    }
}

void appendUtf8(std::string& result, std::uint32_t codePoint) {
    if (codePoint <= 0x7FU) {
        result.push_back(static_cast<char>(codePoint));
    } else if (codePoint <= 0x7FFU) {
        result.push_back(static_cast<char>(0xC0U | (codePoint >> 6U)));
        result.push_back(static_cast<char>(0x80U | (codePoint & 0x3FU)));
    } else if (codePoint <= 0xFFFFU) {
        result.push_back(static_cast<char>(0xE0U | (codePoint >> 12U)));
        result.push_back(static_cast<char>(0x80U | ((codePoint >> 6U) & 0x3FU)));
        result.push_back(static_cast<char>(0x80U | (codePoint & 0x3FU)));
    } else {
        result.push_back(static_cast<char>(0xF0U | (codePoint >> 18U)));
        result.push_back(static_cast<char>(0x80U | ((codePoint >> 12U) & 0x3FU)));
        result.push_back(static_cast<char>(0x80U | ((codePoint >> 6U) & 0x3FU)));
        result.push_back(static_cast<char>(0x80U | (codePoint & 0x3FU)));
    }
}

std::string utf16ToUtf8(const std::u16string& value) {
    std::string result;
    result.reserve(value.size());
    for (std::size_t index = 0; index < value.size(); ++index) {
        auto codePoint = static_cast<std::uint32_t>(value[index]);
        if (codePoint >= 0xD800U && codePoint <= 0xDBFFU) {
            if (index + 1 < value.size()) {
                const auto low = static_cast<std::uint32_t>(value[index + 1]);
                if (low >= 0xDC00U && low <= 0xDFFFU) {
                    codePoint = 0x10000U + ((codePoint - 0xD800U) << 10U) +
                                (low - 0xDC00U);
                    ++index;
                } else {
                    codePoint = 0xFFFDU;
                }
            } else {
                codePoint = 0xFFFDU;
            }
        } else if (codePoint >= 0xDC00U && codePoint <= 0xDFFFU) {
            codePoint = 0xFFFDU;
        }
        appendUtf8(result, codePoint);
    }
    return result;
}

class FieldWalker final {
public:
    FieldWalker(std::vector<FieldValue>& fields, Warnings& warnings)
        : fields_(fields), warnings_(warnings) {}

    void message(
        const MessageMembers& members,
        const void* data,
        const std::string& path,
        std::size_t depth) {
        if (depth > maximumDepth) {
            warnings_.add("maximum nesting depth was reached at " + path);
            return;
        }
        const auto* bytes = static_cast<const std::byte*>(data);
        for (std::uint32_t index = 0; index < members.member_count_; ++index) {
            if (fields_.size() >= maximumFields) {
                warnings_.add("flattened field limit of 4096 was reached");
                return;
            }
            const auto& member = members.members_[index];
            if (!member.name_) {
                warnings_.add("introspection metadata contains an unnamed member");
                continue;
            }
            const auto memberPath = childPath(path, member.name_);
            const auto* memberData = bytes + member.offset_;
            if (member.is_array_) {
                array(member, memberData, memberPath, depth);
            } else {
                value(member, memberData, memberPath, depth);
            }
        }
    }

private:
    void add(const std::string& path, FieldData value) {
        if (fields_.size() >= maximumFields) {
            warnings_.add("flattened field limit of 4096 was reached");
            return;
        }
        fields_.push_back({path, "", std::move(value)});
    }

    template<typename Integer>
    void integer(const std::string& path, Integer value) {
        if constexpr (std::is_signed_v<Integer>) {
            constexpr auto bound = static_cast<std::int64_t>(maximumExactInteger);
            const auto converted = static_cast<std::int64_t>(value);
            if (converted < -bound || converted > bound) {
                warnings_.add(
                    "integer at " + path + " is outside exact double range");
                return;
            }
        } else if (static_cast<std::uint64_t>(value) > maximumExactInteger) {
            warnings_.add(
                "integer at " + path + " is outside exact double range");
            return;
        }
        add(path, static_cast<double>(value));
    }

    void value(
        const MessageMember& member,
        const void* data,
        const std::string& path,
        std::size_t depth) {
        using namespace rosidl_typesupport_introspection_cpp;
        switch (member.type_id_) {
        case ROS_TYPE_FLOAT:
            add(path, static_cast<double>(*static_cast<const float*>(data)));
            break;
        case ROS_TYPE_DOUBLE:
            add(path, *static_cast<const double*>(data));
            break;
        case ROS_TYPE_LONG_DOUBLE: {
            const auto value = *static_cast<const long double*>(data);
            if (!std::isfinite(value) ||
                value > static_cast<long double>(std::numeric_limits<double>::max()) ||
                value < static_cast<long double>(std::numeric_limits<double>::lowest())) {
                warnings_.add("long double at " + path + " cannot be represented as double");
            } else {
                add(path, static_cast<double>(value));
            }
            break;
        }
        case ROS_TYPE_CHAR:
        case ROS_TYPE_UINT8:
            integer(path, *static_cast<const std::uint8_t*>(data));
            break;
        case ROS_TYPE_OCTET:
            integer(path, *static_cast<const unsigned char*>(data));
            break;
        case ROS_TYPE_INT8:
            integer(path, *static_cast<const std::int8_t*>(data));
            break;
        case ROS_TYPE_WCHAR:
            integer(path, static_cast<std::uint16_t>(
                              *static_cast<const char16_t*>(data)));
            break;
        case ROS_TYPE_UINT16:
            integer(path, *static_cast<const std::uint16_t*>(data));
            break;
        case ROS_TYPE_INT16:
            integer(path, *static_cast<const std::int16_t*>(data));
            break;
        case ROS_TYPE_UINT32:
            integer(path, *static_cast<const std::uint32_t*>(data));
            break;
        case ROS_TYPE_INT32:
            integer(path, *static_cast<const std::int32_t*>(data));
            break;
        case ROS_TYPE_UINT64:
            integer(path, *static_cast<const std::uint64_t*>(data));
            break;
        case ROS_TYPE_INT64:
            integer(path, *static_cast<const std::int64_t*>(data));
            break;
        case ROS_TYPE_BOOLEAN:
            add(path, *static_cast<const bool*>(data));
            break;
        case ROS_TYPE_STRING: {
            const auto& text = *static_cast<const std::string*>(data);
            if (text.size() > maximumStringBytes) {
                warnings_.add("string at " + path + " exceeds the 1 MiB protocol limit");
            } else {
                add(path, text);
            }
            break;
        }
        case ROS_TYPE_WSTRING: {
            const auto converted = utf16ToUtf8(*static_cast<const std::u16string*>(data));
            if (converted.size() > maximumStringBytes) {
                warnings_.add("wide string at " + path +
                              " exceeds the 1 MiB protocol limit after UTF-8 conversion");
            } else {
                add(path, converted);
            }
            break;
        }
        case ROS_TYPE_MESSAGE: {
            const auto* members = nestedMembers(member);
            if (!members) {
                warnings_.add("nested message metadata is unavailable at " + path);
                return;
            }
            message(*members, data, path, depth + 1);
            break;
        }
        default:
            warnings_.add(
                "unsupported ROS field type " + std::to_string(member.type_id_) +
                " at " + path);
            break;
        }
    }

    void fetchedValue(
        const MessageMember& member,
        const void* arrayData,
        std::size_t index,
        const std::string& path,
        std::size_t depth) {
        using namespace rosidl_typesupport_introspection_cpp;
        switch (member.type_id_) {
        case ROS_TYPE_FLOAT: fetch<float>(member, arrayData, index, path, depth); break;
        case ROS_TYPE_DOUBLE: fetch<double>(member, arrayData, index, path, depth); break;
        case ROS_TYPE_LONG_DOUBLE:
            fetch<long double>(member, arrayData, index, path, depth);
            break;
        case ROS_TYPE_CHAR:
        case ROS_TYPE_UINT8:
            fetch<std::uint8_t>(member, arrayData, index, path, depth);
            break;
        case ROS_TYPE_OCTET:
            fetch<unsigned char>(member, arrayData, index, path, depth);
            break;
        case ROS_TYPE_INT8:
            fetch<std::int8_t>(member, arrayData, index, path, depth);
            break;
        case ROS_TYPE_WCHAR:
            fetch<char16_t>(member, arrayData, index, path, depth);
            break;
        case ROS_TYPE_UINT16:
            fetch<std::uint16_t>(member, arrayData, index, path, depth);
            break;
        case ROS_TYPE_INT16:
            fetch<std::int16_t>(member, arrayData, index, path, depth);
            break;
        case ROS_TYPE_UINT32:
            fetch<std::uint32_t>(member, arrayData, index, path, depth);
            break;
        case ROS_TYPE_INT32:
            fetch<std::int32_t>(member, arrayData, index, path, depth);
            break;
        case ROS_TYPE_UINT64:
            fetch<std::uint64_t>(member, arrayData, index, path, depth);
            break;
        case ROS_TYPE_INT64:
            fetch<std::int64_t>(member, arrayData, index, path, depth);
            break;
        case ROS_TYPE_BOOLEAN: fetch<bool>(member, arrayData, index, path, depth); break;
        case ROS_TYPE_STRING:
            fetch<std::string>(member, arrayData, index, path, depth);
            break;
        case ROS_TYPE_WSTRING:
            fetch<std::u16string>(member, arrayData, index, path, depth);
            break;
        case ROS_TYPE_MESSAGE: {
            const auto* members = nestedMembers(member);
            if (!members) {
                warnings_.add("nested message metadata is unavailable at " + path);
                return;
            }
            MessageStorage storage(*members);
            member.fetch_function(arrayData, index, storage.get());
            message(*members, storage.get(), path, depth + 1);
            break;
        }
        default:
            warnings_.add(
                "unsupported ROS array element type at " + path);
            break;
        }
    }

    template<typename Value>
    void fetch(
        const MessageMember& member,
        const void* arrayData,
        std::size_t index,
        const std::string& path,
        std::size_t depth) {
        Value fetched{};
        member.fetch_function(arrayData, index, &fetched);
        value(member, &fetched, path, depth);
    }

    void array(
        const MessageMember& member,
        const void* data,
        const std::string& path,
        std::size_t depth) {
        const auto count = member.size_function
                               ? member.size_function(data)
                               : member.array_size_;
        const auto mappedCount = std::min(count, maximumArrayElements);
        if (count > mappedCount) {
            warnings_.add(
                "array at " + path + " was limited to 1024 elements");
        }
        for (std::size_t index = 0; index < mappedCount; ++index) {
            if (fields_.size() >= maximumFields) {
                warnings_.add("flattened field limit of 4096 was reached");
                return;
            }
            const auto elementPath = indexedPath(path, index);
            if (member.get_const_function) {
                const auto* element = member.get_const_function(data, index);
                if (!element) {
                    warnings_.add("array accessor returned null at " + elementPath);
                    continue;
                }
                value(member, element, elementPath, depth);
            } else if (member.fetch_function) {
                fetchedValue(member, data, index, elementPath, depth);
            } else {
                const auto stride = scalarSize(member);
                if (stride == 0 || member.is_upper_bound_ || member.array_size_ == 0) {
                    warnings_.add("array metadata has no readable accessor at " + path);
                    return;
                }
                const auto* element = static_cast<const std::byte*>(data) +
                                      index * stride;
                value(member, element, elementPath, depth);
            }
        }
    }

    std::vector<FieldValue>& fields_;
    Warnings& warnings_;
};

const MessageMember* findMember(
    const MessageMembers& members,
    std::string_view name) {
    for (std::uint32_t index = 0; index < members.member_count_; ++index) {
        const auto& member = members.members_[index];
        if (member.name_ && name == member.name_) return &member;
    }
    return nullptr;
}

bool isMessage(
    const MessageMembers& members,
    std::string_view messageNamespace,
    std::string_view messageName) {
    return members.message_namespace_ && members.message_name_ &&
           messageNamespace == members.message_namespace_ &&
           messageName == members.message_name_;
}

std::optional<lab::core::Timestamp> standardHeaderTimestamp(
    const MessageMembers& root,
    const void* rootData) {
    using namespace rosidl_typesupport_introspection_cpp;
    const auto* header = findMember(root, "header");
    if (!header || header->is_array_ || header->type_id_ != ROS_TYPE_MESSAGE) {
        return std::nullopt;
    }
    const auto* headerMembers = nestedMembers(*header);
    if (!headerMembers ||
        !isMessage(*headerMembers, "std_msgs::msg", "Header")) {
        return std::nullopt;
    }
    const auto* stamp = findMember(*headerMembers, "stamp");
    if (!stamp || stamp->is_array_ || stamp->type_id_ != ROS_TYPE_MESSAGE) {
        return std::nullopt;
    }
    const auto* stampMembers = nestedMembers(*stamp);
    if (!stampMembers ||
        !isMessage(*stampMembers, "builtin_interfaces::msg", "Time")) {
        return std::nullopt;
    }
    const auto* seconds = findMember(*stampMembers, "sec");
    const auto* nanoseconds = findMember(*stampMembers, "nanosec");
    if (!seconds || !nanoseconds || seconds->is_array_ || nanoseconds->is_array_ ||
        seconds->type_id_ != ROS_TYPE_INT32 ||
        nanoseconds->type_id_ != ROS_TYPE_UINT32) {
        return std::nullopt;
    }

    const auto* rootBytes = static_cast<const std::byte*>(rootData);
    const auto* headerData = rootBytes + header->offset_;
    const auto* stampData = headerData + stamp->offset_;
    const auto sec = *reinterpret_cast<const std::int32_t*>(
        stampData + seconds->offset_);
    const auto nanosec = *reinterpret_cast<const std::uint32_t*>(
        stampData + nanoseconds->offset_);
    if (nanosec >= 1'000'000'000U) return std::nullopt;
    return static_cast<lab::core::Timestamp>(sec) * 1'000'000'000LL +
           static_cast<lab::core::Timestamp>(nanosec);
}

}  // namespace

lab::core::agent::FieldMappingKind inspectGenericFieldMapping(
    const std::string& type,
    std::string* reason) {
    const auto typeSupport = typeSupportCache().get(type);
    if (!typeSupport->serializationHandle) {
        if (reason) {
            *reason = "C++ typesupport is unavailable";
            if (!typeSupport->serializationError.empty()) {
                *reason += ": " + boundedReason(typeSupport->serializationError);
            }
        }
        return lab::core::agent::FieldMappingKind::Unavailable;
    }
    if (!typeSupport->members) {
        if (reason) {
            *reason = "Introspection typesupport is unavailable; raw CDR remains available";
            if (!typeSupport->introspectionError.empty()) {
                *reason += ": " + boundedReason(typeSupport->introspectionError);
            }
        }
        return lab::core::agent::FieldMappingKind::RawOnly;
    }
    if (reason) *reason = "Runtime ROS2 introspection is available";
    return lab::core::agent::FieldMappingKind::Introspection;
}

MappedFields mapGenericSerializedFields(
    const std::string& type,
    const rclcpp::SerializedMessage& serialized) {
    MappedFields result;
    try {
        const auto typeSupport = typeSupportCache().get(type);
        if (!typeSupport->serializationHandle) {
            throw std::runtime_error(
                typeSupport->serializationError.empty()
                    ? "C++ typesupport is unavailable"
                    : typeSupport->serializationError);
        }
        if (!typeSupport->members) {
            throw std::runtime_error(
                typeSupport->introspectionError.empty()
                    ? "introspection typesupport is unavailable"
                    : typeSupport->introspectionError);
        }

        MessageStorage storage(*typeSupport->members);
        rclcpp::SerializationBase serialization(typeSupport->serializationHandle);
        serialization.deserialize_message(&serialized, storage.get());

        Warnings warnings;
        FieldWalker walker(result.fields, warnings);
        walker.message(*typeSupport->members, storage.get(), "", 0);
        result.sourceTimestamp = standardHeaderTimestamp(
                                     *typeSupport->members, storage.get())
                                     .value_or(0);
        result.warning = warnings.format(type);
    } catch (const std::exception& exception) {
        if (rcutils_error_is_set()) rcutils_reset_error();
        result.fields.clear();
        result.sourceTimestamp = 0;
        result.warning =
            "Structured field mapping failed for " + type + ": " + exception.what();
    }
    return result;
}

}  // namespace lab_debug_agent
