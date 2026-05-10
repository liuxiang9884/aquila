#include <cmath>
#include <cstdint>
#include <exception>
#include <filesystem>

#include <CLI/CLI.hpp>
#include <fmt/core.h>

#include "tools/benchmark/time_series_data.h"

namespace {

namespace benchmark_data = aquila::tools::benchmark_data;

struct CliOptions {
  std::filesystem::path output_path{
      "data/benchmark/time_series_1m_1000s_f64.bin"};
  std::uint64_t count{1'000'000};
  double duration_sec{1000.0};
  double min_value{900.0};
  double max_value{1100.0};
  std::uint64_t seed{20260510};
  std::uint64_t min_timestamp_step_ns{1};
};

std::uint64_t DurationSecondsToNs(double duration_sec) {
  if (!std::isfinite(duration_sec) || duration_sec <= 0.0) {
    throw std::invalid_argument("duration-sec must be positive and finite");
  }
  constexpr double kNsPerSecond = 1'000'000'000.0;
  const double duration_ns = duration_sec * kNsPerSecond;
  if (duration_ns >
      static_cast<double>(std::numeric_limits<std::uint64_t>::max())) {
    throw std::invalid_argument("duration-sec is too large");
  }
  return static_cast<std::uint64_t>(std::llround(duration_ns));
}

benchmark_data::TimeSeriesGenerationOptions ToGenerationOptions(
    const CliOptions& cli) {
  return benchmark_data::TimeSeriesGenerationOptions{
      .count = cli.count,
      .duration_ns = DurationSecondsToNs(cli.duration_sec),
      .min_value = cli.min_value,
      .max_value = cli.max_value,
      .seed = cli.seed,
      .min_timestamp_step_ns = cli.min_timestamp_step_ns,
  };
}

}  // namespace

int main(int argc, char** argv) {
  CliOptions cli;
  CLI::App app{
      "Generate deterministic binary time-series data for Aquila benchmarks"};
  app.add_option("--output", cli.output_path, "Output binary file path");
  app.add_option("--count", cli.count, "Number of records");
  app.add_option("--duration-sec", cli.duration_sec,
                 "Maximum timestamp window in seconds");
  app.add_option("--min-value", cli.min_value, "Minimum generated value");
  app.add_option("--max-value", cli.max_value, "Maximum generated value");
  app.add_option("--seed", cli.seed, "PRNG seed");
  app.add_option("--min-step-ns", cli.min_timestamp_step_ns,
                 "Minimum positive timestamp increment in nanoseconds");
  CLI11_PARSE(app, argc, argv);

  try {
    const benchmark_data::TimeSeriesGenerationOptions options =
        ToGenerationOptions(cli);
    const auto points = benchmark_data::GenerateTimeSeries(options);
    benchmark_data::WriteTimeSeriesFile(cli.output_path, options, points);
    const auto file_size = std::filesystem::file_size(cli.output_path);
    fmt::print(
        "generated_time_series output={} count={} duration_ns={} min_value={} "
        "max_value={} seed={} min_step_ns={} first_timestamp_ns={} "
        "last_timestamp_ns={} file_size_bytes={}\n",
        cli.output_path.string(), options.count, options.duration_ns,
        options.min_value, options.max_value, options.seed,
        options.min_timestamp_step_ns, points.front().timestamp_ns,
        points.back().timestamp_ns, file_size);
  } catch (const std::exception& ex) {
    fmt::print(stderr, "[FAIL] {}\n", ex.what());
    return 1;
  }

  return 0;
}
