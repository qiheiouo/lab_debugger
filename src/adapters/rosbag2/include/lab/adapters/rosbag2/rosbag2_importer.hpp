#pragma once

#include "lab/core/timestamp.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace lab::adapters::rosbag2 {

struct Rosbag2TopicInfo {
    std::string name;
    std::string type;
    std::string serializationFormat;
    std::uint64_t messageCount{};
};

struct Rosbag2ImportOptions {
    std::filesystem::path source;
    std::filesystem::path destination;
    std::string sessionName;
    std::string softwareVersion;
};

struct Rosbag2ImportResult {
    bool success{};
    bool cancelled{};
    std::filesystem::path sessionDirectory;
    std::vector<Rosbag2TopicInfo> topics;
    std::uint64_t databaseCount{};
    std::uint64_t messageCount{};
    std::uint64_t payloadBytes{};
    lab::core::Timestamp firstTimestamp{};
    lab::core::Timestamp lastTimestamp{};
    std::string error;
};

using Rosbag2ProgressCallback =
    std::function<bool(std::uint64_t importedMessages, std::uint64_t totalMessages)>;

Rosbag2ImportResult importRosbag2(
    const Rosbag2ImportOptions& options,
    Rosbag2ProgressCallback progress = {});

}  // namespace lab::adapters::rosbag2
