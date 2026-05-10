#ifndef AQUILA_CORE_BASE_HISTOGRAM_QUANTILE_H_
#define AQUILA_CORE_BASE_HISTOGRAM_QUANTILE_H_

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <vector>

#if defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>
#endif

namespace aquila {

namespace histogram_quantile_detail {

#if defined(__x86_64__) || defined(__i386__)
[[gnu::target("avx2")]] inline std::size_t FindQuantileBinAvx2(
    const std::uint32_t* counts, std::size_t size,
    std::uint64_t target) noexcept {
  std::uint64_t cumulative = 0;
  std::size_t i = 0;
  alignas(32) std::uint64_t lane_sums[4];
  for (; i + 8 <= size; i += 8) {
    const __m128i low_counts =
        _mm_loadu_si128(reinterpret_cast<const __m128i*>(counts + i));
    const __m128i high_counts =
        _mm_loadu_si128(reinterpret_cast<const __m128i*>(counts + i + 4));
    const __m256i low_u64 = _mm256_cvtepu32_epi64(low_counts);
    const __m256i high_u64 = _mm256_cvtepu32_epi64(high_counts);
    const __m256i block_sums = _mm256_add_epi64(low_u64, high_u64);
    _mm256_store_si256(reinterpret_cast<__m256i*>(lane_sums), block_sums);
    const std::uint64_t block_sum =
        lane_sums[0] + lane_sums[1] + lane_sums[2] + lane_sums[3];
    if (cumulative + block_sum < target) {
      cumulative += block_sum;
      continue;
    }
    for (std::size_t j = 0; j < 8; ++j) {
      cumulative += counts[i + j];
      if (cumulative >= target) {
        return i + j;
      }
    }
  }
  for (; i < size; ++i) {
    cumulative += counts[i];
    if (cumulative >= target) {
      return i;
    }
  }
  return size - 1;
}

[[gnu::target("avx2")]] inline std::size_t FindQuantileBinReverseAvx2(
    const std::uint32_t* counts, std::size_t size,
    std::uint64_t target) noexcept {
  std::uint64_t cumulative = 0;
  std::size_t i = size;
  alignas(32) std::uint64_t lane_sums[4];
  while (i >= 8) {
    i -= 8;
    const __m128i low_counts =
        _mm_loadu_si128(reinterpret_cast<const __m128i*>(counts + i));
    const __m128i high_counts =
        _mm_loadu_si128(reinterpret_cast<const __m128i*>(counts + i + 4));
    const __m256i low_u64 = _mm256_cvtepu32_epi64(low_counts);
    const __m256i high_u64 = _mm256_cvtepu32_epi64(high_counts);
    const __m256i block_sums = _mm256_add_epi64(low_u64, high_u64);
    _mm256_store_si256(reinterpret_cast<__m256i*>(lane_sums), block_sums);
    const std::uint64_t block_sum =
        lane_sums[0] + lane_sums[1] + lane_sums[2] + lane_sums[3];
    if (cumulative + block_sum < target) {
      cumulative += block_sum;
      continue;
    }
    for (std::size_t j = 8; j > 0; --j) {
      const std::size_t index = i + j - 1;
      cumulative += counts[index];
      if (cumulative >= target) {
        return index;
      }
    }
  }
  while (i > 0) {
    --i;
    cumulative += counts[i];
    if (cumulative >= target) {
      return i;
    }
  }
  return 0;
}

[[gnu::target("avx512f,avx2")]] inline std::size_t FindQuantileBinAvx512(
    const std::uint32_t* counts, std::size_t size,
    std::uint64_t target) noexcept {
  std::uint64_t cumulative = 0;
  std::size_t i = 0;
  for (; i + 16 <= size; i += 16) {
    const __m256i low_counts =
        _mm256_loadu_si256(reinterpret_cast<const __m256i*>(counts + i));
    const __m256i high_counts =
        _mm256_loadu_si256(reinterpret_cast<const __m256i*>(counts + i + 8));
    const __m512i low_u64 = _mm512_cvtepu32_epi64(low_counts);
    const __m512i high_u64 = _mm512_cvtepu32_epi64(high_counts);
    const std::uint64_t block_sum =
        static_cast<std::uint64_t>(_mm512_reduce_add_epi64(low_u64)) +
        static_cast<std::uint64_t>(_mm512_reduce_add_epi64(high_u64));
    if (cumulative + block_sum < target) {
      cumulative += block_sum;
      continue;
    }
    for (std::size_t j = 0; j < 16; ++j) {
      cumulative += counts[i + j];
      if (cumulative >= target) {
        return i + j;
      }
    }
  }
  for (; i < size; ++i) {
    cumulative += counts[i];
    if (cumulative >= target) {
      return i;
    }
  }
  return size - 1;
}

[[gnu::target("avx512f,avx2")]] inline std::size_t FindQuantileBinReverseAvx512(
    const std::uint32_t* counts, std::size_t size,
    std::uint64_t target) noexcept {
  std::uint64_t cumulative = 0;
  std::size_t i = size;
  while (i >= 16) {
    i -= 16;
    const __m256i low_counts =
        _mm256_loadu_si256(reinterpret_cast<const __m256i*>(counts + i));
    const __m256i high_counts =
        _mm256_loadu_si256(reinterpret_cast<const __m256i*>(counts + i + 8));
    const __m512i low_u64 = _mm512_cvtepu32_epi64(low_counts);
    const __m512i high_u64 = _mm512_cvtepu32_epi64(high_counts);
    const std::uint64_t block_sum =
        static_cast<std::uint64_t>(_mm512_reduce_add_epi64(low_u64)) +
        static_cast<std::uint64_t>(_mm512_reduce_add_epi64(high_u64));
    if (cumulative + block_sum < target) {
      cumulative += block_sum;
      continue;
    }
    for (std::size_t j = 16; j > 0; --j) {
      const std::size_t index = i + j - 1;
      cumulative += counts[index];
      if (cumulative >= target) {
        return index;
      }
    }
  }
  while (i > 0) {
    --i;
    cumulative += counts[i];
    if (cumulative >= target) {
      return i;
    }
  }
  return 0;
}
#endif

}  // namespace histogram_quantile_detail

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
  static constexpr std::size_t kDefaultBinCount = 4096;

  HistogramQuantile() = default;

  HistogramQuantile(T min_value, T max_value, std::size_t bin_count,
                    double quantile,
                    HistogramQuantileValueMode value_mode =
                        HistogramQuantileValueMode::kUpperEdge) {
    Init(min_value, max_value, bin_count, quantile, value_mode);
  }

  void Init(T min_value, T max_value, double quantile,
            HistogramQuantileValueMode value_mode =
                HistogramQuantileValueMode::kUpperEdge) {
    Init(min_value, max_value, kDefaultBinCount, quantile, value_mode);
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
    ResetTouchedBins();
  }

  void InitWithReferenceError(T min_value, T max_value, T reference_value,
                              double max_error_bp, double quantile,
                              HistogramQuantileValueMode value_mode =
                                  HistogramQuantileValueMode::kUpperEdge) {
    assert(reference_value > T{});
    assert(max_error_bp > 0.0);
    const double max_abs_error =
        static_cast<double>(reference_value) * max_error_bp / 10000.0;
    const double range =
        static_cast<double>(max_value) - static_cast<double>(min_value);
    const double divisor = value_mode == HistogramQuantileValueMode::kMidpoint
                               ? 2.0 * max_abs_error
                               : max_abs_error;
    const auto required_bins =
        static_cast<std::size_t>(std::ceil(range / divisor));
    Init(min_value, max_value, required_bins, quantile, value_mode);
  }

  void InitWithRangePrecision(T min_value, T max_value, T precision,
                              double quantile,
                              HistogramQuantileValueMode value_mode =
                                  HistogramQuantileValueMode::kUpperEdge) {
    assert(precision > T{});
    const double range =
        static_cast<double>(max_value) - static_cast<double>(min_value);
    const double divisor = value_mode == HistogramQuantileValueMode::kMidpoint
                               ? 2.0 * static_cast<double>(precision)
                               : static_cast<double>(precision);
    const auto required_bins =
        static_cast<std::size_t>(std::ceil(range / divisor));
    Init(min_value, max_value, required_bins, quantile, value_mode);
  }

  void Reset() noexcept {
    if (HasValue()) {
      std::fill(
          counts_.begin() + static_cast<std::ptrdiff_t>(touched_min_bin_),
          counts_.begin() + static_cast<std::ptrdiff_t>(touched_max_bin_ + 1),
          std::uint32_t{0});
    }
    count_ = 0;
    underflow_count_ = 0;
    overflow_count_ = 0;
    ResetTouchedBins();
  }

  void Add(T value) noexcept {
    assert(!counts_.empty());
    const std::size_t index = BinIndex(value);
    ++counts_[index];
    ++count_;
    MarkTouchedBin(index);
  }

  [[nodiscard]] bool HasValue() const noexcept {
    return count_ != 0;
  }

  [[nodiscard]] T Value() const noexcept {
    return ValueScalar();
  }

  [[nodiscard]] T ValueScalar() const noexcept {
    if (!HasValue()) {
      return T{};
    }
    const std::uint64_t target = TargetRank();
    if (!UseForwardScan()) {
      return ValueScalarReverse(ReverseTargetRank(target));
    }
    return ValueScalarForward(target);
  }

  [[nodiscard]] T ValueAvx2() const noexcept {
    if (!HasValue()) {
      return T{};
    }
    const std::uint64_t target = TargetRank();
#if defined(__x86_64__) || defined(__i386__)
    const std::size_t touched_bin_count = TouchedBinCount();
    const std::uint32_t* touched_counts = counts_.data() + touched_min_bin_;
    const std::size_t index =
        UseForwardScan()
            ? histogram_quantile_detail::FindQuantileBinAvx2(
                  touched_counts, touched_bin_count, target)
            : histogram_quantile_detail::FindQuantileBinReverseAvx2(
                  touched_counts, touched_bin_count, ReverseTargetRank(target));
    return BinValue(touched_min_bin_ + index);
#else
    if (!UseForwardScan()) {
      return ValueScalarReverse(ReverseTargetRank(target));
    }
    return ValueScalarForward(target);
#endif
  }

  [[nodiscard]] T ValueAvx512() const noexcept {
    if (!HasValue()) {
      return T{};
    }
    const std::uint64_t target = TargetRank();
#if defined(__x86_64__) || defined(__i386__)
    const std::size_t touched_bin_count = TouchedBinCount();
    const std::uint32_t* touched_counts = counts_.data() + touched_min_bin_;
    const std::size_t index =
        UseForwardScan()
            ? histogram_quantile_detail::FindQuantileBinAvx512(
                  touched_counts, touched_bin_count, target)
            : histogram_quantile_detail::FindQuantileBinReverseAvx512(
                  touched_counts, touched_bin_count, ReverseTargetRank(target));
    return BinValue(touched_min_bin_ + index);
#else
    if (!UseForwardScan()) {
      return ValueScalarReverse(ReverseTargetRank(target));
    }
    return ValueScalarForward(target);
#endif
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
  [[nodiscard]] T ValueScalarForward(std::uint64_t target) const noexcept {
    std::uint64_t cumulative = 0;
    const std::size_t end = touched_max_bin_ + 1;
    for (std::size_t i = touched_min_bin_; i < end; ++i) {
      cumulative += counts_[i];
      if (cumulative >= target) {
        return BinValue(i);
      }
    }
    return BinValue(touched_max_bin_);
  }

  [[nodiscard]] T ValueScalarReverse(std::uint64_t target) const noexcept {
    std::uint64_t cumulative = 0;
    for (std::size_t i = touched_max_bin_ + 1; i > touched_min_bin_;) {
      --i;
      cumulative += counts_[i];
      if (cumulative >= target) {
        return BinValue(i);
      }
    }
    return BinValue(touched_min_bin_);
  }

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

  [[nodiscard]] std::uint64_t ReverseTargetRank(
      std::uint64_t target) const noexcept {
    return count_ - target + 1;
  }

  [[nodiscard]] bool UseForwardScan() const noexcept {
    return quantile_ <= 0.5;
  }

  [[nodiscard]] std::size_t TouchedBinCount() const noexcept {
    return touched_max_bin_ - touched_min_bin_ + 1;
  }

  void MarkTouchedBin(std::size_t index) noexcept {
    touched_min_bin_ = std::min(touched_min_bin_, index);
    touched_max_bin_ = std::max(touched_max_bin_, index);
  }

  void ResetTouchedBins() noexcept {
    touched_min_bin_ = counts_.size();
    touched_max_bin_ = 0;
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
  std::vector<std::uint32_t> counts_;
  std::uint64_t count_{0};
  std::uint64_t underflow_count_{0};
  std::uint64_t overflow_count_{0};
  std::size_t touched_min_bin_{0};
  std::size_t touched_max_bin_{0};
};

}  // namespace aquila

#endif  // AQUILA_CORE_BASE_HISTOGRAM_QUANTILE_H_
