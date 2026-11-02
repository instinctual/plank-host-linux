/**
 * @file src/platform/linux/x11grab.h
 * @brief Declarations for x11 capture.
 */
#pragma once

// standard includes
#include <cstddef>
#include <cstdint>
#include <optional>

// local includes
#include "src/platform/common.h"
#include "x11_rgb10.h"
#include "src/utility.h"

// X11 Display
extern "C" struct _XDisplay;

namespace egl {
  class cursor_t;
}

namespace platf::x11 {
  /**
   * @brief Host-authoritative root-pointer position.
   */
  struct cursor_position_t {
    int x;  ///< Root-window X coordinate of the pointer hotspot.
    int y;  ///< Root-window Y coordinate of the pointer hotspot.
    int desktop_width;  ///< Current root-window width.
    int desktop_height;  ///< Current root-window height.
  };

  /**
   * @brief Scale packed depth-30 X11 RGB directly into planar identity GBR.
   *
   * Center-aligned bilinear sampling preserves the full 10-bit component
   * values and writes FFmpeg's native 0-1023 planar representation without an
   * intermediate RGB or 8-bit surface.
   *
   * @param source Packed X11 image base.
   * @param source_pitch Packed source row stride in bytes.
   * @param source_width Source width in pixels.
   * @param source_height Source height in pixels.
   * @param green FFmpeg plane 0 destination.
   * @param green_pitch Green-plane row stride in bytes.
   * @param blue FFmpeg plane 1 destination.
   * @param blue_pitch Blue-plane row stride in bytes.
   * @param red FFmpeg plane 2 destination.
   * @param red_pitch Red-plane row stride in bytes.
   * @param destination_width Destination width in pixels.
   * @param destination_height Destination height in pixels.
   */
  void scale_xrgb10_to_gbr10(const std::uint8_t *source,
                             std::size_t source_pitch,
                             int source_width,
                             int source_height,
                             std::uint16_t *green,
                             std::size_t green_pitch,
                             std::uint16_t *blue,
                             std::size_t blue_pitch,
                             std::uint16_t *red,
                             std::size_t red_pitch,
                             int destination_width,
                             int destination_height);

  struct cursor_ctx_raw_t;
  /**
   * @brief Release cursor context resources.
   *
   * @param ctx Native context object used by the operation or callback.
   */
  void freeCursorCtx(cursor_ctx_raw_t *ctx);
  /**
   * @brief Release display resources.
   *
   * @param xdisplay X11 display connection.
   */
  void freeDisplay(_XDisplay *xdisplay);

  /**
   * @brief XFixes cursor image pointer released with `XFree`.
   */
  using cursor_ctx_t = util::safe_ptr<cursor_ctx_raw_t, freeCursorCtx>;
  /**
   * @brief X11 display pointer released with `XCloseDisplay`.
   */
  using xdisplay_t = util::safe_ptr<_XDisplay, freeDisplay>;

  /**
   * @brief X11 cursor image and positioning state used during capture.
   */
  class cursor_t {
  public:
    /**
     * @brief Allocate the underlying object and wrap it in the owning handle.
     *
     * @return Created backend object, or null when creation fails.
     */
    static std::optional<cursor_t> make();

    /**
     * @brief Run the capture loop for this backend.
     *
     * @param img Image or frame object to read from or populate.
     */
    bool capture(egl::cursor_t &img);

    /**
     * @brief Subscribe this connection to XFixes cursor-shape changes.
     *
     * This also captures the root geometry used by position queries.
     *
     * @return True when local cursor monitoring is available.
     */
    bool subscribe_shape_events();

    /**
     * @brief Query the current root-pointer position through XQueryPointer.
     *
     * @param position Position populated on success.
     * @return True when the query succeeds.
     */
    bool query_position(cursor_position_t &position);

    /**
     * @brief Drain queued X11 events and report a cursor-shape change.
     *
     * @return 1 when one or more shape changes were coalesced, 0 when none
     *         were pending, or -1 before shape monitoring is initialized.
     */
    int consume_shape_change();

    cursor_ctx_t ctx;  ///< X11 cursor context used to track and blend cursor images.

  private:
    int xfixes_event_base_ {-1};  ///< Base event number for XFixes notifications.
    int desktop_width_ {0};  ///< Root-window width captured at subscription time.
    int desktop_height_ {0};  ///< Root-window height captured at subscription time.
  };

  /**
   * @brief Open and initialize the display connection used for capture.
   *
   * @return Constructed display object.
   */
  xdisplay_t make_display();
}  // namespace platf::x11
