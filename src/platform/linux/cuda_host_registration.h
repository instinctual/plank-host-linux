/**
 * @file src/platform/linux/cuda_host_registration.h
 * @brief Bounded, scoped CUDA registration of reusable X11 shared-memory images.
 */
#pragma once

#include <atomic>
#include <cstddef>
#include <memory>
#include <ffnvcodec/dynlink_cuda.h>

namespace cuda {
  /** @brief Driver operations; the caller retains their library until teardown. */
  struct host_registration_api_t {
    tcuDeviceGet *device_get;
    tcuDevicePrimaryCtxRetain *retain;
    tcuDevicePrimaryCtxRelease *release;
    tcuCtxPushCurrent_v2 *push;
    tcuCtxPopCurrent_v2 *pop;
    CUresult (CUDAAPI *register_memory)(void *, std::size_t, unsigned int);
    CUresult (CUDAAPI *unregister_memory)(void *);
    void (*report_error)(CUresult, const char *);
  };

  /**
   * @brief Own a portable registration and its primary context, not the memory.
   *
   * Destroy only after the upload stream has drained and before shmdt(). Failed
   * registration rolls back every acquired resource and leaves pageable upload
   * available. Budget is shared across capture generations, including images
   * retained by an old encoder during reinitialization.
   */
  class host_memory_registration_t {
  public:
    static std::unique_ptr<host_memory_registration_t> make(
        const host_registration_api_t &api, void *data, std::size_t size,
        std::atomic<std::size_t> &used, std::size_t limit) {
      if (!data || !size || size > limit) return nullptr;
      auto owner = std::unique_ptr<host_memory_registration_t>(
        new host_memory_registration_t(api, used));
      auto previous = used.load(std::memory_order_relaxed);
      do {
        if (previous > limit - size) return nullptr;
      } while (!used.compare_exchange_weak(previous, previous + size, std::memory_order_relaxed));
      owner->size_ = size;
      CUcontext retained_context = nullptr;
      if (api.device_get(&owner->device_, 0) != CUDA_SUCCESS ||
          api.retain(&retained_context, owner->device_) != CUDA_SUCCESS) return nullptr;
      // Failed driver calls need not preserve output parameters. Only adopt a
      // successfully retained context, so rollback cannot release another owner.
      owner->context_ = retained_context;
      if (api.push(owner->context_) != CUDA_SUCCESS) return nullptr;
      constexpr unsigned portable = 1;  // CU_MEMHOSTREGISTER_PORTABLE
      const auto registered = api.register_memory(data, size, portable);
      if (registered == CUDA_SUCCESS) owner->data_ = data;
      CUcontext popped = nullptr;
      const auto restored = api.pop(&popped);
      if (restored != CUDA_SUCCESS) api.report_error(restored, "Couldn't restore SHM registration context: ");
      if (registered != CUDA_SUCCESS || restored != CUDA_SUCCESS) return nullptr;
      return owner;
    }

    ~host_memory_registration_t() {
      if (data_) {
        const auto pushed = api_.push(context_);
        if (pushed == CUDA_SUCCESS) {
          report(api_.unregister_memory(data_), "Couldn't unregister X11 SHM: ");
          CUcontext popped = nullptr;
          report(api_.pop(&popped), "Couldn't restore context after SHM unregister: ");
        } else {
          report(pushed, "Couldn't select SHM unregistration context: ");
        }
      }
      if (context_) report(api_.release(device_), "Couldn't release SHM context: ");
      used_.fetch_sub(size_, std::memory_order_relaxed);
    }

    host_memory_registration_t(const host_memory_registration_t &) = delete;
    host_memory_registration_t &operator=(const host_memory_registration_t &) = delete;

  private:
    host_memory_registration_t(const host_registration_api_t &api, std::atomic<std::size_t> &used):
        api_(api), used_(used) {}
    void report(CUresult status, const char *message) {
      if (status != CUDA_SUCCESS) api_.report_error(status, message);
    }
    host_registration_api_t api_;
    std::atomic<std::size_t> &used_;
    std::size_t size_ = 0;
    CUdevice device_ = 0;
    CUcontext context_ = nullptr;
    void *data_ = nullptr;
  };
}
