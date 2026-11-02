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
   */
  inline capture_batch_result_t capture_batch(const capture_api_t &api,
      xcb_connection_t *connection, xcb_window_t root, xcb_drawable_t overlay,
      int16_t x, int16_t y, uint16_t width, uint16_t height,
      uint16_t environment_width, uint16_t environment_height, xcb_shm_seg_t segment) {
    const auto start = std::chrono::steady_clock::now();
    const auto root_cookie = api.geometry(connection, root);
    const auto overlay_cookie = api.geometry(connection, overlay);
    const auto image_cookie = api.image(connection, overlay, x, y, width, height,
                                       ~0U, XCB_IMAGE_FORMAT_Z_PIXMAP, segment, 0);
    const auto free_reply = [](auto *reply) { std::free(reply); };
    xcb_generic_error_t *error = nullptr;
    std::unique_ptr<xcb_get_geometry_reply_t, decltype(free_reply)> root_reply {
      api.geometry_reply(connection, root_cookie, &error), free_reply};
    bool failed = error != nullptr;
    std::free(error); error = nullptr;
    std::unique_ptr<xcb_get_geometry_reply_t, decltype(free_reply)> overlay_reply {
      api.geometry_reply(connection, overlay_cookie, &error), free_reply};
    failed |= error != nullptr;
    std::free(error); error = nullptr;
    const auto geometry_done = std::chrono::steady_clock::now();
    std::unique_ptr<xcb_shm_get_image_reply_t, decltype(free_reply)> image_reply {
      api.image_reply(connection, image_cookie, &error), free_reply};
    failed |= error != nullptr;
    std::free(error);
    const auto done = std::chrono::steady_clock::now();
    const auto matches = [&](const auto &reply) {
      return reply && reply->width == environment_width &&
             reply->height == environment_height && reply->depth == 30;
    };
    const bool valid = !failed && matches(root_reply) && matches(overlay_reply) &&
      image_reply && image_reply->depth == 30 &&
      image_reply->size == static_cast<std::uint64_t>(width) * height * 4U;
    return {valid,
      std::chrono::duration<double, std::micro>(geometry_done - start).count(),
      std::chrono::duration<double, std::micro>(done - geometry_done).count()};
  }
}
