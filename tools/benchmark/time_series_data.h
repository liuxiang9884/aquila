#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <random>
#include <span>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

namespace aquila::tools::benchmark_data {

inline constexpr std::array<char, 8> kTimeSeriesFileMagic{'A', 'Q', 'T', 'S',
                                                          'F', '6', '4', '\0'};
inline constexpr std::uint32_t kTimeSeriesFileVersion = 1;

struct TimeSeriesPoint {
  std::uint64_t timestamp_ns{};
  double value{};
};

static_assert(std::is_trivially_copyable_v<TimeSeriesPoint>);
static_assert(std::is_standard_layout_v<TimeSeriesPoint>);
static_assert(sizeof(TimeSeriesPoint) == 16);

struct TimeSeriesFileHeader {
  char magic[8]{};
  std::uint32_t version{};
  std::uint32_t record_size{};
  std::uint64_t count{};
  std::uint64_t duration_ns{};
  std::uint64_t seed{};
  double min_value{};
  double max_value{};
};

static_assert(std::is_trivially_copyable_v<TimeSeriesFileHeader>);
static_assert(std::is_standard_layout_v<TimeSeriesFileHeader>);

struct TimeSeriesGenerationOptions {
  std::uint64_t count{1'000'000};
  std::uint64_t duration_ns{1'000'000'000'000ULL};
  double min_value{900.0};
  double max_value{1100.0};
  std::uint64_t seed{20260510};
  std::uint64_t min_timestamp_step_ns{1};
};

inline bool IsTimeSeriesFileHeader(
    const TimeSeriesFileHeader& header) noexcept {
  return std::memcmp(header.magic, kTimeSeriesFileMagic.data(),
                     kTimeSeriesFileMagic.size()) == 0 &&
         header.version == kTimeSeriesFileVersion &&
         header.record_size == sizeof(TimeSeriesPoint);
}

inline TimeSeriesFileHeader MakeTimeSeriesFileHeader(
    const TimeSeriesGenerationOptions& options) noexcept {
  TimeSeriesFileHeader header{};
  std::copy(kTimeSeriesFileMagic.begin(), kTimeSeriesFileMagic.end(),
            header.magic);
  header.version = kTimeSeriesFileVersion;
  header.record_size = sizeof(TimeSeriesPoint);
  header.count = options.count;
  header.duration_ns = options.duration_ns;
  header.seed = options.seed;
  header.min_value = options.min_value;
  header.max_value = options.max_value;
  return header;
}

inline void ValidateTimeSeriesGenerationOptions(
    const TimeSeriesGenerationOptions& options) {
  if (options.count == 0) {
    throw std::invalid_argument("count must be positive");
  }
  if (options.duration_ns == 0) {
    throw std::invalid_argument("duration_ns must be positive");
  }
  if (options.min_timestamp_step_ns == 0) {
    throw std::invalid_argument("min_timestamp_step_ns must be positive");
  }
  if (options.min_value > options.max_value) {
    throw std::invalid_argument("min_value must be <= max_value");
  }
  if (options.count > options.duration_ns / options.min_timestamp_step_ns) {
    throw std::invalid_argument(
        "count cannot fit into duration with the requested minimum step");
  }
}

inline std::vector<TimeSeriesPoint> GenerateTimeSeries(
    const TimeSeriesGenerationOptions& options) {
  ValidateTimeSeriesGenerationOptions(options);

  std::vector<TimeSeriesPoint> points;
  points.reserve(static_cast<std::size_t>(options.count));

  std::mt19937_64 rng(options.seed);
  std::uniform_real_distribution<double> value_distribution(options.min_value,
                                                            options.max_value);

  std::uint64_t timestamp_ns = 0;
  for (std::uint64_t i = 0; i < options.count; ++i) {
    const std::uint64_t remaining_points = options.count - i;
    const std::uint64_t remaining_after = remaining_points - 1;
    const std::uint64_t latest_next_timestamp =
        options.duration_ns - remaining_after * options.min_timestamp_step_ns;
    const std::uint64_t max_step_by_end = latest_next_timestamp - timestamp_ns;
    const std::uint64_t average_remaining_step =
        (options.duration_ns - timestamp_ns) / remaining_points;
    const std::uint64_t preferred_max_step =
        average_remaining_step > (std::numeric_limits<std::uint64_t>::max() / 2)
            ? std::numeric_limits<std::uint64_t>::max()
            : average_remaining_step * 2;
    const std::uint64_t max_step =
        std::min(max_step_by_end,
                 std::max(options.min_timestamp_step_ns, preferred_max_step));

    std::uniform_int_distribution<std::uint64_t> step_distribution(
        options.min_timestamp_step_ns, max_step);
    timestamp_ns += step_distribution(rng);
    points.push_back(TimeSeriesPoint{
        .timestamp_ns = timestamp_ns,
        .value = value_distribution(rng),
    });
  }

  return points;
}

inline void WriteTimeSeriesFile(const std::filesystem::path& output_path,
                                const TimeSeriesGenerationOptions& options,
                                std::span<const TimeSeriesPoint> points) {
  if (points.size() != options.count) {
    throw std::invalid_argument("points size must match options.count");
  }
  if (output_path.has_parent_path()) {
    std::filesystem::create_directories(output_path.parent_path());
  }

  std::ofstream output(output_path, std::ios::binary | std::ios::trunc);
  if (!output.is_open()) {
    throw std::runtime_error("failed to open output file");
  }

  const TimeSeriesFileHeader header = MakeTimeSeriesFileHeader(options);
  output.write(reinterpret_cast<const char*>(&header), sizeof(header));
  output.write(reinterpret_cast<const char*>(points.data()),
               static_cast<std::streamsize>(points.size_bytes()));
  if (!output.good()) {
    throw std::runtime_error("failed to write output file");
  }
}

}  // namespace aquila::tools::benchmark_data
