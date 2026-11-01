/**
 * @file src/stream.h
 * @brief Declarations for the streaming protocols.
 */
#pragma once

// standard includes
#include <cstdint>
#include <utility>

// lib includes
#include <boost/asio.hpp>

// local includes
#include "audio.h"
#include "video.h"

namespace stream {
  constexpr auto VIDEO_STREAM_PORT = 9;  ///< GameStream base-port offset used for the video UDP stream.
  constexpr auto CONTROL_PORT = 10;  ///< GameStream base-port offset used for the control channel.
  constexpr auto AUDIO_STREAM_PORT = 11;  ///< GameStream base-port offset used for the audio UDP stream.

  struct session_t;

  /**
   * @brief Stream configuration shared by capture and network senders.
   */
  struct config_t {
    audio::config_t audio;  ///< Audio capture configuration for the stream.
    video::config_t monitor;  ///< Video capture and encoder configuration for the selected monitor.

    int packetsize;  ///< Maximum payload size for network packets.
    int minRequiredFecPackets;  ///< Minimum recovery packets required before FEC is emitted.
    int mlFeatureFlags;  ///< Moonlight feature flags negotiated for this session.
    int controlProtocolType;  ///< GameStream control protocol variant selected by the client.
    int audioQosType;  ///< Audio QoS type.
    int videoQosType;  ///< Video QoS type.

  };

  namespace session {
    /**
     * @brief Enumerates supported state options.
     */
    enum class state_e : int {
      STOPPED,  ///< The session is stopped
      STOPPING,  ///< The session is stopping
      STARTING,  ///< The session is starting
      RUNNING,  ///< The session is running
    };

    /**
     * @brief Allocate and initialize platform input state for a stream.
     *
     * @param config Configuration values to apply.
     * @param launch_session Launch session.
     * @return Allocated object or identifier, or an error value on failure.
     */
    std::shared_ptr<session_t> alloc(config_t &config, session_stream::launch_session_t &launch_session);
    /**
     * @brief Start a streaming session for the supplied peer address.
     *
     * @param session Active streaming or pairing session for the request.
     * @param addr_string Addr string.
     * @return Start status.
     */
    int start(session_t &session, const std::string &addr_string);
    /**
     * @brief Stop a streaming session and prevent more packets from being queued.
     *
     * @param session Active streaming or pairing session for the request.
     */
    void stop(session_t &session);
    /**
     * @brief Stop a session after notifying its client of the Host reason.
     *
     * Input processing is stopped before the reason is queued so a displaced
     * client cannot deliver another input event during session handoff.
     *
     * @param session Active streaming session.
     * @param termination_reason PLANK Host termination reason.
     */
    void stop(session_t &session, std::uint32_t termination_reason);
    void notify_desktop_handoff(session_t &session);
    /**
     * @brief Wait for worker threads owned by the session to exit.
     *
     * @param session Active streaming or pairing session for the request.
     */
    void join(session_t &session);
    /**
     * @brief Platform handle returned from stream setup.
     *
     * @param session Active streaming or pairing session for the request.
     * @return Current lifecycle state for the stream session.
     */
    state_e state(session_t &session);
  }  // namespace session
}  // namespace stream
