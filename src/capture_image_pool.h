/**
 * @file src/capture_image_pool.h
 * @brief Bounded capture-image ownership and idle retention.
 */
#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <list>
#include <memory>
#include <optional>

namespace video {
  /**
   * @brief Reuse capture images without reclaiming buffers still owned by an encoder.
   *
   * Only the capture thread accesses the pool. Consumers may release their shared
   * references concurrently. Retention keeps already allocated storage available;
   * it neither allocates ahead nor changes the number of queued frames. Image must
   * expose an immutable capture_pool_preferred flag for expensive registered memory.
   * @tparam Image Shared capture image type.
   * @tparam Clock Monotonic clock, replaceable by deterministic tests.
   */
  template<class Image, class Clock = std::chrono::steady_clock>
  class capture_image_pool_t {
  public:
    static constexpr std::size_t capacity = 12;  ///< Existing maximum capture images.

    /** @brief Compute a lazy retention floor without overflowing byte-count multiplication. */
    static std::size_t retention_for_bytes(std::size_t frame_bytes, std::size_t budget) {
      return frame_bytes != 0 ? std::min(capacity, budget / frame_bytes) : 0;
    }

    /** @brief Initialize an empty pool with a bounded lazy-retention floor. */
    explicit capture_image_pool_t(std::size_t retained = 0):
        retained_ {std::min(retained, capacity)},
        images_(capacity) {
    }

    /**
     * @brief Claim a reusable image or allocate an empty slot; never block here.
     * @param allocate Factory called only when no allocated image is available.
     * @return Claimed image, or null on allocation failure or a fully owned pool.
     */
    template<class Allocate>
    std::shared_ptr<Image> acquire(Allocate &&allocate) {
      auto available = images_.end();
      if (retained_ != 0) {
        available = std::find_if(images_.begin(), images_.end(), [](const auto &image) {
          return image && image.use_count() == 1 && image->capture_pool_preferred;
        });
      }
      if (available == images_.end()) {
        available = std::find_if(images_.begin(), images_.end(), [](const auto &image) {
          return image && image.use_count() == 1;
        });
      }
      if (available == images_.end()) {
        available = std::find(images_.begin(), images_.end(), nullptr);
        if (available == images_.end()) {
          ++unavailable_count;
          return nullptr;
        }
        *available = allocate();
        if (!*available) {
          ++allocation_failures;
          return nullptr;
        }
        ++allocation_count;
      }

      auto image = *available;
      // Splice reorders existing nodes without a per-acquisition list allocation.
      images_.splice(images_.begin(), images_, available);
      trim(Clock::now());
      return image;
    }

    /**
     * @brief Release pool ownership before display replacement, keeping consumers valid.
     *
     * Images held by the encoder remain alive until that encoder releases them.
     * Usage history and counters belong to one display generation only.
     */
    void clear() {
      for (auto &image : images_) {
        image.reset();
      }
      used_timestamps_ = {};
      allocation_count = retired_count = unavailable_count = allocation_failures = 0;
      peak_allocated = 0;
    }

    /** @brief Update the retention floor after the old display pool has been cleared. */
    void set_retention(std::size_t retained) {
      retained_ = std::min(retained, capacity);
    }

    /** @brief Return the number of allocated buffers protected from idle trimming. */
    std::size_t retention() const {
      return retained_;
    }

    std::uint64_t allocation_count = 0;  ///< Successful factory calls this generation.
    std::uint64_t retired_count = 0;  ///< Idle images released by trimming.
    std::uint64_t unavailable_count = 0;  ///< Acquisition calls with every slot owned.
    std::uint64_t allocation_failures = 0;  ///< Failed factory calls.
    std::size_t peak_allocated = 0;  ///< Maximum allocated slots this generation.

  private:
    /** @brief Preserve recent in-use capacity and the backend's bounded warm buffers. */
    void trim(typename Clock::time_point now) {
      std::size_t allocated = 0;
      std::size_t used = 0;
      std::size_t preferred = 0;
      std::size_t used_nonpreferred = 0;
      for (const auto &image : images_) {
        if (image) {
          const bool in_use = image.use_count() > 1;
          ++allocated;
          used += in_use;
          preferred += image->capture_pool_preferred;
          used_nonpreferred += in_use && !image->capture_pool_preferred;
        }
      }
      peak_allocated = std::max(peak_allocated, allocated);
      used_timestamps_[used] = now;

      std::size_t target = used;
      for (std::size_t i = used; i < used_timestamps_.size(); ++i) {
        if (used_timestamps_[i] && now - *used_timestamps_[i] < std::chrono::seconds(3)) {
          target = i;
        }
      }
      target = std::max(target, retained_);
      // An overflow image still owned by the encoder must not displace an
      // otherwise reusable registered buffer. Once it is released, normal
      // trimming returns the pool to the byte-budgeted retention floor.
      target = std::max(target, std::min(preferred, retained_) + used_nonpreferred);
      if (allocated <= target) {
        return;
      }

      auto to_trim = allocated - target;
      const auto release_unused = [&](bool preserve_preferred) {
        for (auto it = images_.rbegin(); it != images_.rend() && to_trim != 0; ++it) {
          auto &image = *it;
          if (image && image.use_count() == 1 &&
              (!preserve_preferred || !image->capture_pool_preferred)) {
            image.reset();
            --to_trim;
            ++retired_count;
          }
        }
      };
      // A temporary burst may exceed the CUDA pin budget. Retire pageable
      // overflow before pinned images, even if the overflow was used more recently.
      if (retained_ != 0) {
        release_unused(true);
      }
      release_unused(false);
      for (std::size_t i = target + 1; i < used_timestamps_.size(); ++i) {
        used_timestamps_[i].reset();
      }
    }

    std::size_t retained_;  ///< Lazy-retention floor, bounded by pool capacity.
    std::list<std::shared_ptr<Image>> images_;  ///< Fixed slots in reuse order.
    std::array<std::optional<typename Clock::time_point>, capacity + 1> used_timestamps_ {};  ///< Bounded three-second usage history.
  };
}
