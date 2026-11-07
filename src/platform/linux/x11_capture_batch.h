/**
 * @file src/platform/linux/x11_capture_batch.h
 * @brief One ordered XCB batch for geometry validation and depth-30 capture.
 */
#pragma once

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <xcb/shm.h>

namespace platf::x11 {
  /** @brief Dynamically loaded checked requests and their matching replies. */
  struct capture_api_t {
    decltype(&xcb_get_geometry) geometry {};
    decltype(&xcb_get_geometry_reply) geometry_reply {};
    decltype(&xcb_shm_get_image) image {};
    decltype(&xcb_shm_get_image_reply) image_reply {};
    decltype(&xcb_change_window_attributes_checked) watch {};
    decltype(&xcb_request_check) check {};
    decltype(&xcb_poll_for_event) poll {};
  };

  /** @brief Validated capture result and CPU waits, in microseconds. */
  struct capture_batch_result_t {
    bool valid {};
    double geometry_wait_us {};
    double image_wait_us {};
  };

  /**
   * @brief Queue all three requests before waiting; consume every reply on error.
   *
   * XCB flushes outstanding requests when waiting for a reply. Capture remains
   * synchronous with SHM reuse, but no Xlib round trips precede the image request.
   * Checked requests deliver errors here rather than accumulating an event queue.
   * Geometry wait includes the initial batch dispatch; image wait is the remaining
   * wait after geometry replies, not an independent GPU execution measurement.
   * @param api Loaded XCB function table.
   * @param connection SHM owner's connection, serialized by the caller.
   * @param root Root window whose dimensions define the environment.
   * @param overlay Verified depth-30 XComposite drawable.
   * @param x Horizontal source offset.
   * @param y Vertical source offset.
   * @param width Capture width.
   * @param height Capture height.
   * @param environment_width Expected root and overlay width.
   * @param environment_height Expected root and overlay height.
   * @param segment Writable SHM image segment; remains owned until all replies.
   * @param validate_geometry False only after event-backed geometry validation.
   */
  inline capture_batch_result_t capture_batch(const capture_api_t &api, xcb_connection_t *connection, xcb_window_t root, xcb_drawable_t overlay, int16_t x, int16_t y, uint16_t width, uint16_t height, uint16_t environment_width, uint16_t environment_height, xcb_shm_seg_t segment, bool validate_geometry = true) {
    const auto start = std::chrono::steady_clock::now();
    const auto root_cookie = validate_geometry ? api.geometry(connection, root) : xcb_get_geometry_cookie_t {};
    const auto overlay_cookie = validate_geometry ? api.geometry(connection, overlay) : xcb_get_geometry_cookie_t {};
    const auto image_cookie = api.image(connection, overlay, x, y, width, height, ~0U, XCB_IMAGE_FORMAT_Z_PIXMAP, segment, 0);
    const auto free_reply = [](auto *reply) {
      std::free(reply);
    };
    xcb_generic_error_t *error = nullptr;
    std::unique_ptr<xcb_get_geometry_reply_t, decltype(free_reply)> root_reply {
      validate_geometry ? api.geometry_reply(connection, root_cookie, &error) : nullptr,
      free_reply
    };
    bool failed = error != nullptr;
    std::free(error);
    error = nullptr;
    std::unique_ptr<xcb_get_geometry_reply_t, decltype(free_reply)> overlay_reply {
      validate_geometry ? api.geometry_reply(connection, overlay_cookie, &error) : nullptr,
      free_reply
    };
    failed |= error != nullptr;
    std::free(error);
    error = nullptr;
    const auto geometry_done = std::chrono::steady_clock::now();
    std::unique_ptr<xcb_shm_get_image_reply_t, decltype(free_reply)> image_reply {
      api.image_reply(connection, image_cookie, &error),
      free_reply
    };
    failed |= error != nullptr;
    std::free(error);
    const auto done = std::chrono::steady_clock::now();
    const auto matches = [&](const auto &reply) {
      return reply && reply->width == environment_width &&
             reply->height == environment_height && reply->depth == 30;
    };
    const bool valid = !failed && (!validate_geometry || (matches(root_reply) && matches(overlay_reply))) &&
                       image_reply && image_reply->depth == 30 &&
                       image_reply->size == static_cast<std::uint64_t>(width) * height * 4U;
    return {valid, std::chrono::duration<double, std::micro>(geometry_done - start).count(), std::chrono::duration<double, std::micro>(done - geometry_done).count()};
  }

  /** @brief Geometry stays valid only while the watched drawables remain unchanged.
   * Subscribe before initial validation; drain events after the image reply as
   * well, rejecting a frame if a structure change raced capture. Depth and exact
   * SHM image size are still checked on every frame. No content/damage events
   * are filtered, so animated/native 10-bit application frames are never skipped.
   */
  struct capture_geometry_t {
    bool valid = false;  ///< Validated dimensions for this subscribed connection.

    /** @brief Subscribe both capture drawables before their first validation.
     * @param api Checked XCB API.
     * @param connection Capture-owned connection.
     * @param root Root drawable.
     * @param overlay Depth-30 composite drawable.
     * @return Whether both subscriptions succeeded.
     */
    bool subscribe(const capture_api_t &api, xcb_connection_t *connection, xcb_window_t root, xcb_window_t overlay) {
      valid = false;
      if (!api.watch || !api.check || !api.poll) {
        return false;
      }
      const std::uint32_t mask = XCB_EVENT_MASK_STRUCTURE_NOTIFY;
      const auto first = api.watch(connection, root, XCB_CW_EVENT_MASK, &mask);
      const auto second = api.watch(connection, overlay, XCB_CW_EVENT_MASK, &mask);
      auto *first_error = api.check(connection, first);
      auto *second_error = api.check(connection, second);
      const bool accepted = !first_error && !second_error;
      std::free(first_error);
      std::free(second_error);
      return accepted;
    }

    /** @brief Invalidate cached geometry on any event from this dedicated connection.
     * @param api Checked XCB API.
     * @param connection Subscribed capture connection.
     * @return Whether a structure change or asynchronous error was observed.
     */
    bool changed(const capture_api_t &api, xcb_connection_t *connection) {
      bool changed = false;
      while (auto *event = api.poll(connection)) {
        changed = true;
        std::free(event);
      }
      if (changed) {
        valid = false;
      }
      return changed;
    }

    /** @brief Capture with event-backed geometry and per-frame image validation.
     * @param api Checked XCB API.
     * @param connection Subscribed capture connection.
     * @param root Root drawable.
     * @param overlay Composite drawable.
     * @param x Source X offset.
     * @param y Source Y offset.
     * @param width Source width.
     * @param height Source height.
     * @param environment_width Expected root/overlay width.
     * @param environment_height Expected root/overlay height.
     * @param segment Owned writable SHM image segment.
     * @return Validated frame and timing measurements.
     */
    capture_batch_result_t snapshot(const capture_api_t &api, xcb_connection_t *connection, xcb_window_t root, xcb_drawable_t overlay, int16_t x, int16_t y, uint16_t width, uint16_t height, uint16_t environment_width, uint16_t environment_height, xcb_shm_seg_t segment) {
      if (!api.poll) {
        return {};
      }
      changed(api, connection);
      auto result = capture_batch(api, connection, root, overlay, x, y, width, height, environment_width, environment_height, segment, !valid);
      if (changed(api, connection)) {
        result.valid = false;
      }
      valid = result.valid;
      return result;
    }
  };
}  // namespace platf::x11
