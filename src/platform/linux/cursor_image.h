/**
 * @file src/platform/linux/cursor_image.h
 * @brief X11 cursor pixels and hotspot metadata, independent of capture.
 */
#pragma once
#include "src/platform/common.h"

#include <cstdint>
#include <vector>

namespace platf {
  /**
   * @brief Cursor image and hotspot metadata captured from the window system.
   */
  class cursor_image_t: public img_t {
  public:
    int x;  ///< Cursor hotspot or surface X coordinate.
    int y;  ///< Cursor hotspot or surface Y coordinate.
    int src_w;  ///< Cursor source image width in pixels.
    int src_h;  ///< Cursor source image height in pixels.
    int hotspot_x;  ///< Cursor hotspot X coordinate within the source image.
    int hotspot_y;  ///< Cursor hotspot Y coordinate within the source image.
    int desktop_width;  ///< Width of the X root desktop containing the cursor.
    int desktop_height;  ///< Height of the X root desktop containing the cursor.
    bool visible;  ///< Whether any cursor pixel is visible.

    unsigned long serial;  ///< X11 cursor serial used to detect cursor image changes.

    std::vector<std::uint8_t> buffer;  ///< Cursor image pixels.
  };

}  // namespace platf
