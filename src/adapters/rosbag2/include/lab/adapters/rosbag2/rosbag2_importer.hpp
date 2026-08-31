#pragma once

#include "lab/core/timestamp.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace lab::adapters::rosbag2 {

struct Rosbag2TopicInfo {
    std::string name;
    std::string type;
    std::string serializationFormat;
    std::uint64_t messageCount{};
    bool structuredFields{};
};

struct Rosbag2TopicSelector {
    std::string name;
    std::string type;
};

struct Rosbag2MetadataInfo {
    bool present{};
    bool parsed{};
    std::filesystem::path sourcePath;
    std::uint64_t byteCount{};
    std::string sha256;
    std::optional<std::uint64_t> version;
    std::string storageIdentifier;
    std::optional<std::uint64_t> durationNanoseconds;
    std::optional<std::uint64_t> startingTimeNanoseconds;
    std::optional<std::uint64_t> messageCount;
    std::string compressionFormat;
    std::string compressionMode;
    std::string rosDistro;
    std::vector<std::string> relativeFilePaths;
    std::optional<bool> storageIdentifierMatches;
    std::optional<bool> databaseFilesMatch;
    std::optional<bool> messageCountMatches;
    std::vector<std::string> warnings;
};

struct Rosbag2InspectionResult {
    bool success{};
    bool cancelled{};
    std::vector<Rosbag2TopicInfo> topics;
    std::uint64_t databaseCount{};
    std::uint64_t messageCount{};
    std::uint64_t payloadBytes{};
    Rosbag2MetadataInfo metadata;
    std::string error;
};

struct Rosbag2ImportOptions {
    std::filesystem::path source;
    std::filesystem::path destination;
    std::string sessionName;
    std::string softwareVersion;
    std::vector<Rosbag2TopicSelector> includedTopics;
};

struct Rosbag2ImportResult {
    bool success{};
    bool cancelled{};
    std::filesystem::path sessionDirectory;
    std::vector<Rosbag2TopicInfo> topics;
    std::uint64_t databaseCount{};
    std::uint64_t messageCount{};
    std::uint64_t payloadBytes{};
    std::uint64_t mappedMessageCount{};
    std::uint64_t sampleCount{};
    std::uint64_t mappingFailures{};
    lab::core::Timestamp firstTimestamp{};
    lab::core::Timestamp lastTimestamp{};
    Rosbag2MetadataInfo metadata;
    std::string error;
};

using Rosbag2ProgressCallback =
    std::function<bool(std::uint64_t importedMessages, std::uint64_t totalMessages)>;

using Rosbag2CancellationCallback = std::function<bool()>;

[[nodiscard]] std::string rosbag2SourceId(std::string_view topic,
                                         std::string_view messageType);

Rosbag2InspectionResult inspectRosbag2(
    const std::filesystem::path& source,
    Rosbag2CancellationCallback cancelled = {});

Rosbag2ImportResult importRosbag2(
    const Rosbag2ImportOptions& options,
    Rosbag2ProgressCallback progress = {});

}  // namespace lab::adapters::rosbag2
