#ifndef AQUILA_CORE_BASE_HISTOGRAM_QUANTILE_H_
#define AQUILA_CORE_BASE_HISTOGRAM_QUANTILE_H_

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <vector>

namespace aquila {

enum class HistogramQuantileValueMode {
  kLowerEdge,
  kMidpoint,
  kUpperEdge,
};

template <typename T>
class HistogramQuantile {
 public:
  static_assert(std::is_arithmetic_v<T>,
                "HistogramQuantile requires an arithmetic value type");

  HistogramQuantile() = default;

  HistogramQuantile(T min_value, T max_value, std::size_t bin_count,
                    double quantile,
                    HistogramQuantileValueMode value_mode =
                        HistogramQuantileValueMode::kUpperEdge) {
    Init(min_value, max_value, bin_count, quantile, value_mode);
  }

  void Init(T min_value, T max_value, std::size_t bin_count, double quantile,
            HistogramQuantileValueMode value_mode =
                HistogramQuantileValueMode::kUpperEdge) {
    assert(bin_count > 0);
    assert(max_value > min_value);
    assert(quantile >= 0.0 && quantile <= 1.0);
    min_value_ = min_value;
    max_value_ = max_value;
    bin_width_ =
        (static_cast<double>(max_value_) - static_cast<double>(min_value_)) /
        static_cast<double>(bin_count);
    quantile_ = quantile;
    value_mode_ = value_mode;
    count_ = 0;
    underflow_count_ = 0;
    overflow_count_ = 0;
    counts_.assign(bin_count, 0);
  }

  void Reset() noexcept {
    std::fill(counts_.begin(), counts_.end(), std::uint64_t{0});
    count_ = 0;
    underflow_count_ = 0;
    overflow_count_ = 0;
  }

  void Add(T value) noexcept {
    assert(!counts_.empty());
    const std::size_t index = BinIndex(value);
    ++counts_[index];
    ++count_;
  }

  [[nodiscard]] bool HasValue() const noexcept {
    return count_ != 0;
  }

  [[nodiscard]] T Value() const noexcept {
    if (!HasValue()) {
      return T{};
    }
    const std::uint64_t target = TargetRank();
    std::uint64_t cumulative = 0;
    for (std::size_t i = 0; i < counts_.size(); ++i) {
      cumulative += counts_[i];
      if (cumulative >= target) {
        return BinValue(i);
      }
    }
    return BinValue(counts_.size() - 1);
  }

  [[nodiscard]] std::size_t bin_count() const noexcept {
    return counts_.size();
  }

  [[nodiscard]] double bin_width() const noexcept {
    return bin_width_;
  }

  [[nodiscard]] std::uint64_t count() const noexcept {
    return count_;
  }

  [[nodiscard]] std::uint64_t underflow_count() const noexcept {
    return underflow_count_;
  }

  [[nodiscard]] std::uint64_t overflow_count() const noexcept {
    return overflow_count_;
  }

  [[nodiscard]] std::size_t counts_capacity() const noexcept {
    return counts_.capacity();
  }

 private:
  [[nodiscard]] std::size_t BinIndex(T value) noexcept {
    if (value < min_value_) {
      ++underflow_count_;
      return 0;
    }
    if (value > max_value_) {
      ++overflow_count_;
      return counts_.size() - 1;
    }
    if (value == max_value_) {
      return counts_.size() - 1;
    }
    const double offset =
        (static_cast<double>(value) - static_cast<double>(min_value_)) /
        bin_width_;
    const auto index = static_cast<std::size_t>(offset);
    return std::min(index, counts_.size() - 1);
  }

  [[nodiscard]] std::uint64_t TargetRank() const noexcept {
    std::uint64_t target =
        static_cast<std::uint64_t>(std::ceil(quantile_ * count_));
    target = std::max<std::uint64_t>(1, target);
    return std::min(target, count_);
  }

  [[nodiscard]] T BinValue(std::size_t index) const noexcept {
    const double lower = static_cast<double>(min_value_) +
                         static_cast<double>(index) * bin_width_;
    const double upper = index + 1 == counts_.size()
                             ? static_cast<double>(max_value_)
                             : lower + bin_width_;
    switch (value_mode_) {
      case HistogramQuantileValueMode::kLowerEdge:
        return static_cast<T>(lower);
      case HistogramQuantileValueMode::kMidpoint:
        return static_cast<T>((lower + upper) * 0.5);
      case HistogramQuantileValueMode::kUpperEdge:
        return static_cast<T>(upper);
    }
    return static_cast<T>(upper);
  }

  T min_value_{};
  T max_value_{};
  double bin_width_{0.0};
  double quantile_{0.5};
  HistogramQuantileValueMode value_mode_{
      HistogramQuantileValueMode::kUpperEdge};
  std::vector<std::uint64_t> counts_;
  std::uint64_t count_{0};
  std::uint64_t underflow_count_{0};
  std::uint64_t overflow_count_{0};
};

}  // namespace aquila

#endif  // AQUILA_CORE_BASE_HISTOGRAM_QUANTILE_H_
