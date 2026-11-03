/**
 * @file src/nvenc/idr_scheduler.h
 * @brief Coalesce refresh requests without losing requests inside the rate limit.
 */
#pragma once

#include <chrono>
#include <optional>

namespace nvenc {
  /**
   * @brief Retain one pending refresh until the encoder's rate limit permits it.
   *
   * Owned by the encoder thread. Capture continues producing frames while a
   * request waits, including on an idle desktop. No timer thread is needed.
   */
  class idr_scheduler {
  public:
    using clock = std::chrono::steady_clock;

    /** @brief Coalesce a refresh request without postponing its deadline. */
    void request() {
      pending = true;
    }

    /**
     * @brief Consume a pending request when submitting the next encoded frame.
     * @param now Monotonic submission time.
     * @return Whether this frame must be an IDR.
     */
    bool take_request(clock::time_point now) {
      if (!pending || (next_allowed && now < *next_allowed)) {
        return false;
      }
      pending = false;
      next_allowed = now + std::chrono::seconds {1};
      return true;
    }

  private:
    bool pending = false;
    std::optional<clock::time_point> next_allowed;
  };
}
