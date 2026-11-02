/**
 * @file src/frame_timing.h
 * @brief Bounded capture/encode timing statistics without hot-path allocation.
 */
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace video {
  /**
   * @brief Keep lifetime aggregates and the most recent 2048 timing samples.
   *
   * Percentiles describe the bounded recent window; count, mean, maximum and
   * frame-budget misses cover the complete lifetime. Each producer owns its
   * accumulator; no locks, allocation or sorting occur when recording a sample.
   */
  class timing_samples_t {
  public:
    static constexpr std::size_t capacity = 2048;  ///< Maximum retained samples.

    /** @brief Summary of microsecond samples; quantiles cover window_samples. */
    struct summary_t {
      std::uint64_t count {};  ///< Lifetime sample count.
      std::size_t window_samples {};  ///< Samples used for percentiles.
      double mean {};  ///< Lifetime mean in microseconds.
      double p50 {};  ///< Recent-window median.
      double p95 {};  ///< Recent-window 95th percentile.
      double p99 {};  ///< Recent-window 99th percentile.
      double maximum {};  ///< Lifetime maximum.
      std::uint64_t over_budget {};  ///< Lifetime samples exceeding 1/60 second.
    };

    /** @brief Record a finite nonnegative duration in microseconds. */
    void push_back(double microseconds) {
      if (!std::isfinite(microseconds) || microseconds < 0) {
        return;
      }
      samples_[count_ % capacity] = microseconds;
      ++count_;
      total_ += microseconds;
      maximum_ = std::max(maximum_, microseconds);
      over_budget_ += microseconds > 1000000.0 / 60.0;
    }

    /** @brief Return the number of samples recorded over the full lifetime. */
    std::uint64_t size() const { return count_; }

    /** @brief Compute quantiles on a bounded stack copy, outside the frame path. */
    summary_t summary() const {
      const auto size = static_cast<std::size_t>(std::min<std::uint64_t>(count_, capacity));
      if (!size) {
        return {};
      }
      auto sorted = samples_;
      std::sort(sorted.begin(), sorted.begin() + size);
      const auto percentile = [&](double fraction) {
        return sorted[static_cast<std::size_t>(std::ceil(size * fraction)) - 1];
      };
      return {count_, size, static_cast<double>(total_ / count_),
              percentile(0.50), percentile(0.95), percentile(0.99), maximum_, over_budget_};
    }

  private:
    std::array<double, capacity> samples_ {};  ///< Recent circular window.
    std::uint64_t count_ {};  ///< Lifetime count and next circular slot.
    long double total_ {};  ///< Lifetime sum without unbounded sample storage.
    double maximum_ {};  ///< Lifetime maximum.
    std::uint64_t over_budget_ {};  ///< Lifetime 60-Hz budget misses.
  };

  /** @brief Log bounded timing statistics; name identifies the measured stage. */
  void log_frame_timing(std::string_view name, const timing_samples_t &samples);
}
