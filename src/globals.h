/**
 * @file globals.h
 * @brief Declarations for globally accessible variables and functions.
 */
#pragma once

// local includes
#include "entry_handler.h"
#include "thread_pool.h"

/**
 * @brief A thread pool for processing tasks.
 */
extern thread_pool_util::ThreadPool task_pool;


/**
 * @brief Handles process-wide communication.
 */
namespace mail {
/**
 * @def MAIL(x)
 * @brief Macro for MAIL.
 */
#define MAIL(x) \
  constexpr auto x = std::string_view { \
    #x \
  }

  /**
   * @brief A process-wide communication mechanism.
   */
  extern safe::mail_t man;

  // Global mail
  MAIL(shutdown);  ///< Shutdown.
  MAIL(broadcast_shutdown);  ///< Broadcast shutdown.
  MAIL(video_packets);  ///< Video packets.
  MAIL(audio_packets);  ///< Audio packets.
  MAIL(switch_display);  ///< Switch display.
  MAIL(desktop_reattach);  ///< Rebind capture to a new local graphical session.

  // Local mail
  MAIL(touch_port);  ///< Touch port.
  MAIL(idr);  ///< IDR.
  MAIL(invalidate_ref_frames);  ///< Invalidate ref frames.
  MAIL(raw_hid_feedback);  ///< Raw HID tablet control requests.
  MAIL(video_bitrate);  ///< Active-session video bitrate requests in kilobits per second.
  MAIL(cursor_shape);  ///< Complete local-cursor wire chunks awaiting reliable transmission.
  MAIL(cursor_position);  ///< Latest host-authoritative cursor position awaiting transmission.
  MAIL(clipboard_offer);  ///< Complete clipboard offer wire chunks awaiting reliable transmission.
  MAIL(hdr);  ///< HDR.
#undef MAIL

}  // namespace mail
