/** @file src/platform/linux/pulse_wait.h
 * @brief Bounded PulseAudio dispatch on the calling thread; no detached work.
 */
#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <pulse/pulseaudio.h>

namespace platf::pulse {
  /**
   * @brief Dispatch until completion, cancellation, context failure or deadline.
   * @param loop Mainloop exclusively owned by the caller.
   * @param context Context whose failure cancels the operation.
   * @param ready Completion predicate, evaluated after every dispatch.
   * @param stop Cancellation predicate, polled at most ten milliseconds apart.
   * @param deadline Absolute monotonic operation deadline.
   * @return Whether completion won before failure/cancellation.
   */
  template<class Ready, class Stop>
  bool wait(pa_mainloop *loop, pa_context *context, Ready ready, Stop stop, std::chrono::steady_clock::time_point deadline) {
    while (!stop()) {
      const auto state = pa_context_get_state(context);
      if (state == PA_CONTEXT_FAILED || state == PA_CONTEXT_TERMINATED) {
        return false;
      }
      if (ready()) {
        return true;
      }
      const auto remaining = std::chrono::duration_cast<std::chrono::microseconds>(deadline - std::chrono::steady_clock::now());
      if (remaining.count() <= 0) {
        return false;
      }
      if (pa_mainloop_prepare(loop, static_cast<int>(std::min<std::int64_t>(remaining.count(), 10000))) < 0 ||
          pa_mainloop_poll(loop) < 0 || pa_mainloop_dispatch(loop) < 0) {
        return false;
      }
    }
    return false;
  }
}  // namespace platf::pulse
