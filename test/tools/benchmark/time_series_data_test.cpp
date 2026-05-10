#include "tools/benchmark/time_series_data.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <vector>

#include <gtest/gtest.h>

namespace aquila::tools::benchmark_data {
namespace {

TEST(TimeSeriesDataTest,
     GeneratesStrictlyIncreasingTimestampsAndBoundedValues) {
  TimeSeriesGenerationOptions options;
  options.count = 4096;
  options.duration_ns = 1'000'000'000;
  options.min_value = 900.0;
  options.max_value = 1100.0;
  options.seed = 20260510;

  const std::vector<TimeSeriesPoint> points = GenerateTimeSeries(options);

  ASSERT_EQ(points.size(), options.count);
  std::uint64_t previous_timestamp = 0;
  bool has_previous = false;
  for (const TimeSeriesPoint& point : points) {
    EXPECT_LE(point.timestamp_ns, options.duration_ns);
    EXPECT_GE(point.value, options.min_value);
    EXPECT_LE(point.value, options.max_value);
    if (has_previous) {
      EXPECT_GT(point.timestamp_ns, previous_timestamp);
    }
    previous_timestamp = point.timestamp_ns;
    has_previous = true;
  }
}

TEST(TimeSeriesDataTest, UsesSeedForRepeatableOutput) {
  TimeSeriesGenerationOptions options;
  options.count = 128;
  options.duration_ns = 10'000'000;
  options.min_value = 900.0;
  options.max_value = 1100.0;
  options.seed = 12345;

  const std::vector<TimeSeriesPoint> first = GenerateTimeSeries(options);
  const std::vector<TimeSeriesPoint> second = GenerateTimeSeries(options);

  ASSERT_EQ(first.size(), second.size());
  for (std::size_t i = 0; i < first.size(); ++i) {
    EXPECT_EQ(first[i].timestamp_ns, second[i].timestamp_ns);
    EXPECT_EQ(first[i].value, second[i].value);
  }
}

TEST(TimeSeriesDataTest, RejectsImpossibleTimestampRange) {
  TimeSeriesGenerationOptions options;
  options.count = 10;
  options.duration_ns = 5;
  options.min_timestamp_step_ns = 1;

  EXPECT_THROW(static_cast<void>(GenerateTimeSeries(options)),
               std::invalid_argument);
}

TEST(TimeSeriesDataTest, WritesHeaderAndRecords) {
  const std::filesystem::path output_path =
      std::filesystem::temp_directory_path() /
      "aquila_time_series_data_test.bin";
  std::filesystem::remove(output_path);

  TimeSeriesGenerationOptions options;
  options.count = 64;
  options.duration_ns = 1'000'000;
  options.min_value = 900.0;
  options.max_value = 1100.0;
  options.seed = 7;

  const std::vector<TimeSeriesPoint> points = GenerateTimeSeries(options);
  WriteTimeSeriesFile(output_path, options, points);

  const auto expected_size = static_cast<std::uintmax_t>(
      sizeof(TimeSeriesFileHeader) + points.size() * sizeof(TimeSeriesPoint));
  EXPECT_EQ(std::filesystem::file_size(output_path), expected_size);

  std::ifstream input(output_path, std::ios::binary);
  ASSERT_TRUE(input.is_open());
  TimeSeriesFileHeader header{};
  input.read(reinterpret_cast<char*>(&header), sizeof(header));
  ASSERT_TRUE(input.good());
  EXPECT_TRUE(IsTimeSeriesFileHeader(header));
  EXPECT_EQ(header.version, kTimeSeriesFileVersion);
  EXPECT_EQ(header.record_size, sizeof(TimeSeriesPoint));
  EXPECT_EQ(header.count, options.count);
  EXPECT_EQ(header.duration_ns, options.duration_ns);
  EXPECT_EQ(header.seed, options.seed);

  TimeSeriesPoint first_point{};
  input.read(reinterpret_cast<char*>(&first_point), sizeof(first_point));
  ASSERT_TRUE(input.good());
  EXPECT_EQ(first_point.timestamp_ns, points.front().timestamp_ns);
  EXPECT_EQ(first_point.value, points.front().value);

  std::filesystem::remove(output_path);
}

TEST(TimeSeriesDataTest, ReadsWrittenFile) {
  const std::filesystem::path output_path =
      std::filesystem::temp_directory_path() /
      "aquila_time_series_data_read_test.bin";
  std::filesystem::remove(output_path);

  TimeSeriesGenerationOptions options;
  options.count = 128;
  options.duration_ns = 10'000'000;
  options.min_value = 900.0;
  options.max_value = 1100.0;
  options.seed = 20260510;

  const std::vector<TimeSeriesPoint> points = GenerateTimeSeries(options);
  WriteTimeSeriesFile(output_path, options, points);

  TimeSeriesFileHeader header{};
  const std::vector<TimeSeriesPoint> loaded =
      ReadTimeSeriesFile(output_path, &header);

  EXPECT_TRUE(IsTimeSeriesFileHeader(header));
  EXPECT_EQ(header.count, options.count);
  ASSERT_EQ(loaded.size(), points.size());
  for (std::size_t i = 0; i < loaded.size(); ++i) {
    EXPECT_EQ(loaded[i].timestamp_ns, points[i].timestamp_ns);
    EXPECT_EQ(loaded[i].value, points[i].value);
  }

  std::filesystem::remove(output_path);
}

}  // namespace
}  // namespace aquila::tools::benchmark_data
