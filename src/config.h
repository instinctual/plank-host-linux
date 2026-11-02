/**
 * @file src/config.h
 * @brief Declarations for the configuration of Sunshine.
 */
#pragma once

// standard includes
#include <bitset>
#include <chrono>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

// local includes
#include "nvenc/nvenc_config.h"

namespace config {
  // Valid range for the packetsize limit
  constexpr int PACKETSIZE_MIN = 200;  ///< Lowest accepted configured packet size in bytes.
  constexpr int PACKETSIZE_MAX = 65535;  ///< Highest accepted configured packet size in bytes.
  constexpr int PACKETSIZE_SMALL = 500;  ///< Conservative packet size used for low-MTU links.
  constexpr int PACKETSIZE_LARGE = 1456;  ///< Default large packet size that avoids common MTU fragmentation.

  // track modified config options
  inline std::unordered_map<std::string, std::string> modified_config_settings;  ///< Configuration keys changed during the current parse or UI update.

  // sensitive values that should be redacted from logging
  /**
   * @brief Configuration keys whose values must be hidden in logs.
   */
  inline constexpr std::array<std::string_view, 0> redacted_config {};

  /**
   * @brief Log configuration entries and optionally mark them for persistence.
   *
   * @param vars Parsed configuration entries to log.
   * @param save Whether modified configuration values should be written back to disk.
   */
  void log_config_settings(const std::unordered_map<std::string, std::string> &vars, bool save);

  /**
   * @brief Video encoder tuning and color settings loaded from configuration.
   */
  struct video_t {
    // ffmpeg params
    /**
     * @brief Quantization parameter used by encoders where higher values trade quality for compression.
     */
    int qp;  // higher == more compression and less quality

    int min_threads;  ///< Minimum number of threads or slices for CPU encoding.

    struct {
      std::string sw_preset;
      std::string sw_tune;
      std::optional<int> svtav1_preset;
      int vbv_maxrate_percentage;  ///< Peak software-encoder rate as a percentage of the requested average rate.
      int vbv_buffer_frames;  ///< Software-encoder VBV reservoir in average-rate frame units; zero keeps legacy sizing.
      int scenecut;  ///< x264 scene-change threshold; zero disables adaptive keyframes.
    } sw;  ///< Software encoder options.

    nvenc::nvenc_config nv;  ///< NVIDIA NVENC encoder settings.
    bool nv_realtime_hags;  ///< Enable the NVIDIA realtime HAGS workaround.
    bool nv_opengl_vulkan_on_dxgi;  ///< Prefer NVIDIA OpenGL/Vulkan-on-DXGI interop.
    bool nv_sunshine_high_power_mode;  ///< Request NVIDIA high-power mode for Sunshine.

    struct {
      std::optional<int> qsv_preset;
      std::optional<int> qsv_cavlc;
      bool qsv_slow_hevc;
    } qsv;  ///< Intel Quick Sync encoder options.

    struct {
      std::optional<int> amd_usage_h264;
      std::optional<int> amd_usage_hevc;
      std::optional<int> amd_usage_av1;
      std::optional<int> amd_rc_h264;
      std::optional<int> amd_rc_hevc;
      std::optional<int> amd_rc_av1;
      std::optional<int> amd_enforce_hrd;
      std::optional<int> amd_quality_h264;
      std::optional<int> amd_quality_hevc;
      std::optional<int> amd_quality_av1;
      std::optional<int> amd_preanalysis;
      std::optional<int> amd_vbaq;
      int amd_coder;
    } amd;  ///< AMD AMF encoder options.

    std::string adapter_name;  ///< Display adapter selected in configuration.

    double minimum_fps_target;  ///< Lowest framerate that will be used when streaming. Range 0-1000, 0 = half of client's requested framerate.
  };

  /**
   * @brief Audio capture and encoder settings loaded from configuration.
   */
  struct audio_t {
    std::string sink;  ///< Audio output device/sink to use for audio capture
    std::string virtual_sink;  ///< Virtual audio sink for audio routing
    bool stream;  ///< Enable audio streaming to clients
    bool install_steam_drivers;  ///< Install Steam audio drivers for enhanced compatibility
  };

  /**
   * @brief Network stream settings shared by audio, video, and control channels.
   */
  struct stream_t {
    std::chrono::milliseconds ping_timeout;  ///< Timeout used when waiting for client ping responses.
  };

  /**
   * @brief HTTP and HTTPS settings used by PLANK negotiation.
   */
  struct nvhttp_t {
    std::string pkey;  ///< Private key PEM string or path.
    std::string cert;  ///< Certificate PEM string or path.

    std::string host_name;  ///< Host name advertised to Moonlight clients.

    std::string file_state;  ///< Path to the persisted Sunshine state file.

    bool publish_session_user;  ///< Publish the desktop account name on unauthenticated serverinfo.
  };

  /**
   * @brief Input emulation settings loaded from configuration.
   */
  struct input_t {
    std::unordered_map<int, int> keybindings;  ///< Client keycode to platform keycode bindings.

    std::chrono::milliseconds key_repeat_delay;  ///< Delay before repeating a held keyboard key.
    std::chrono::duration<double> key_repeat_period;  ///< Interval between repeated keyboard key events.

#ifdef _WIN32
    std::string gamepad;  ///< Legacy Windows-only virtual controller backend selection.
    bool ds4_back_as_touchpad_click;  ///< Legacy Windows-only Back/Select mapping.
    bool motion_as_ds4;  ///< Legacy Windows-only motion-controller mapping.
    bool touchpad_as_ds4;  ///< Legacy Windows-only touch-controller mapping.
    bool virtualhid_randomize_mac;  ///< Legacy Windows-only virtual controller identity policy.
#endif

    bool keyboard;  ///< Enable keyboard input from clients.
    bool key_rightalt_to_key_win;  ///< Map the client Right Alt key to the Windows key.
    bool mouse;  ///< Enable mouse input from clients.
    bool always_send_scancodes;  ///< Always send keyboard scancodes when available.

    bool high_resolution_scrolling;  ///< Enable high-resolution mouse-wheel events.
  };

  namespace flag {
    /**
     * @brief Enumerates supported flag options.
     */
    enum flag_e : std::size_t {
      FORCE_VIDEO_HEADER_REPLACE = 0,  ///< force replacing headers inside video data
      FLAG_SIZE  ///< Number of flags
    };
  }  // namespace flag

  /**
   * @brief Top-level Sunshine configuration and credential state.
   */
  struct sunshine_t {
    int min_log_level;  ///< Minimum severity level written to the configured log sink.
    std::bitset<flag::FLAG_SIZE> flags;  ///< Runtime flags parsed from command-line options.
    std::string config_file;  ///< Path to the active Sunshine configuration file.

    /**
     * @brief Command-line options parsed before configuration loading.
     */
    struct cmd_t {
      std::string name;  ///< Executable name from the command line.
      int argc;  ///< Number of command-line arguments.
      char **argv;  ///< Command-line argument vector.
    } cmd;  ///< Command line used to launch the application.

    std::uint16_t port;  ///< TCP port used by Sunshine services.

    std::string log_file;  ///< Path to the configured log file.
    bool mdns_discovery;  ///< Advertise this PLANK host with mDNS.
    std::string startup_layout;  ///< PLANK physical or virtual display policy applied before the display manager starts.
  };

  extern video_t video;
  extern audio_t audio;
  extern stream_t stream;
  extern nvhttp_t nvhttp;
  extern input_t input;
  extern sunshine_t sunshine;

  /**
   * @brief Parse serialized text into the corresponding runtime representation.
   *
   * @param argc Number of command-line arguments.
   * @param argv Command-line argument vector.
   * @return 0 on success; nonzero when command-line or configuration parsing fails.
   */
  int parse(int argc, char *argv[]);
  /**
   * @brief Parse INI-style PLANK configuration text into key-value entries.
   *
   * @param file_content Raw configuration file contents to parse.
   * @return Parsed globally scoped key-value entries; section headers are organizational.
   */
  std::unordered_map<std::string, std::string> parse_config(const std::string_view &file_content);
}  // namespace config
