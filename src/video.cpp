/**
 * @file src/video.cpp
 * @brief Definitions for video.
 */
// standard includes
#include <algorithm>
#include <array>
#include <atomic>
#include <bitset>
#include <cerrno>
#include <charconv>
#include <cmath>
#include <cstring>
#include <list>
#include <thread>
#include <utility>

#ifdef __linux__
  #include <sched.h>
  #include <unistd.h>
#endif

// lib includes
#include <boost/pointer_cast.hpp>

extern "C" {
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
}

// local includes
#include "cbs.h"
#include "config.h"
#include "frame_timing.h"
#include "globals.h"
#include "input.h"
#include "logging.h"
#include "nvenc/nvenc_encoder.h"
#include "platform/common.h"
#include "sync.h"
#include "video.h"
#ifdef __linux__
  #include "session/session_context.h"
#endif

using namespace std::literals;

namespace video {

  software_rate_control_t software_rate_control(
    std::int64_t average_rate,
    int framerate,
    int peak_percentage,
    int buffer_frames
  ) {
    return {
      average_rate,
      average_rate * peak_percentage / 100,
      buffer_frames > 0 && framerate > 0 ? average_rate * buffer_frames / framerate : 0,
    };
  }

  namespace {
#ifdef __linux__
    /**
     * @brief Allow x264 workers to use every CPU available to Sunshine.
     *
     * NvFBC initialization can narrow the calling thread's inherited affinity.
     * Expand it after capture setup and immediately before x264 creates workers.
     */
    void expand_software_encoder_cpu_affinity() {
      const long processor_count = sysconf(_SC_NPROCESSORS_CONF);
      if (processor_count <= 0 || processor_count > CPU_SETSIZE) {
        BOOST_LOG(warning) << "Unable to determine CPUs for software encoder affinity"sv;
        return;
      }

      cpu_set_t affinity;
      CPU_ZERO(&affinity);
      for (long cpu = 0; cpu < processor_count; ++cpu) {
        CPU_SET(cpu, &affinity);
      }

      if (sched_setaffinity(0, sizeof(affinity), &affinity) != 0) {
        BOOST_LOG(warning) << "Unable to expand software encoder CPU affinity: "sv << std::strerror(errno);
        return;
      }

      cpu_set_t effective_affinity;
      CPU_ZERO(&effective_affinity);
      if (sched_getaffinity(0, sizeof(effective_affinity), &effective_affinity) == 0) {
        BOOST_LOG(info) << "Software encoder thread CPU affinity allows "sv << CPU_COUNT(&effective_affinity) << " logical CPUs"sv;
      }
    }
#endif

    /**
     * @brief Collect bounded display-to-packet timing for either maintained encoder.
     */
    struct integrated_timing_samples_t {
      /**
       * @brief Emit the completed stream's aggregate timing distributions.
       */
      explicit integrated_timing_samples_t(std::string_view conversion_name):
          conversion_name(conversion_name) {}

      ~integrated_timing_samples_t() {
        log_frame_timing("display_to_convert_start", display_to_convert_start_us);
        log_frame_timing(
          conversion_name,
          conversion_us
        );
        log_frame_timing("encode_and_packetize", encode_and_packetize_us);
        log_frame_timing("display_to_packet_ready", display_to_packet_ready_us);
        log_frame_timing("packet_ready_cadence", packet_ready_cadence_us);
      }

      timing_samples_t display_to_convert_start_us;
      timing_samples_t conversion_us;
      timing_samples_t encode_and_packetize_us;
      timing_samples_t display_to_packet_ready_us;
      timing_samples_t packet_ready_cadence_us;
      std::optional<std::chrono::steady_clock::time_point> last_packet_ready;
      std::string_view conversion_name;
    };
  }  // namespace

  void log_frame_timing(std::string_view name, const timing_samples_t &samples) {
    if (samples.size() < 60) {
      return;
    }
    const auto summary = samples.summary();
    BOOST_LOG(info) << "PLANK timing " << name
                    << ": samples=" << summary.count
                    << " percentile_window_samples=" << summary.window_samples
                    << " mean_ms=" << summary.mean / 1000.0
                    << " p50_ms=" << summary.p50 / 1000.0
                    << " p95_ms=" << summary.p95 / 1000.0
                    << " p99_ms=" << summary.p99 / 1000.0
                    << " max_ms=" << summary.maximum / 1000.0
                    << " over_16_67ms=" << summary.over_budget;
  }

  /**
   * @brief Release context resources.
   */
  void free_ctx(AVCodecContext *ctx) {
    avcodec_free_context(&ctx);
  }

  /**
   * @brief Release an FFmpeg frame allocated by the capture or conversion backend.
   */
  void free_frame(AVFrame *frame) {
    av_frame_free(&frame);
  }

  /**
   * @brief Select the x264 profile for the requested chroma sampling.
   * @param config Exact negotiated video format.
   * @return FFmpeg H.264 profile identifier.
   */
  int select_h264_profile(const config_t &config) {
    if (config.chromaSamplingType == 1) {
      return AV_PROFILE_H264_HIGH_444_PREDICTIVE;
    }
    if (config.chromaSamplingType == 2) {
      return AV_PROFILE_H264_HIGH_422;
    }

    return AV_PROFILE_H264_HIGH;
  }

  /**
   * @brief Enumerates supported flag options.
   */
  enum flag_e : uint32_t {
    DEFAULT = 0,  ///< Default flags
    H264_ONLY = 1 << 2,  ///< When HEVC is too heavy
    REF_FRAMES_INVALIDATION = 1 << 8,  ///< Support reference frames invalidation
    ALWAYS_REPROBE = 1 << 9,  ///< This is an encoder of last resort and we want to aggressively probe for a better one
    YUV444_SUPPORT = 1 << 10,  ///< Encoder may support 4:4:4 chroma sampling depending on hardware
    ASYNC_TEARDOWN = 1 << 11,  ///< Encoder supports async teardown on a different thread
    YUV422_SUPPORT = 1 << 13,  ///< Encoder may support 4:2:2 chroma sampling depending on hardware
  };

  /**
   * @brief FFmpeg AVCodec encode session and parameter-set rewriting state.
   */
  class avcodec_encode_session_t: public encode_session_t {
  public:
    avcodec_encode_session_t() = default;

    /**
     * @brief Initialize an FFmpeg encode session and its hardware encode device.
     *
     * @param avcodec_ctx Open FFmpeg codec context for the selected encoder.
     * @param encode_device Platform encode device that supplies frames to FFmpeg.
     * @param inject Whether SPS/VPS replacement data should be injected.
     * @param warmup Whether to discard one startup encode before publishing packets.
     */
    avcodec_encode_session_t(avcodec_ctx_t &&avcodec_ctx, std::unique_ptr<platf::avcodec_encode_device_t> encode_device, int inject, bool warmup):
        avcodec_ctx {std::move(avcodec_ctx)},
        device {std::move(encode_device)},
        inject {inject},
        warmup_pending {warmup},
        measure_timing {warmup} {
    }

    /**
     * @brief Move an FFmpeg encode session without duplicating codec/device ownership.
     *
     * @param other Source object whose state is copied or moved into this object.
     */
    avcodec_encode_session_t(avcodec_encode_session_t &&other) noexcept = default;

    ~avcodec_encode_session_t() {
      log_frame_timing("x264_encode_completion", encode_completion_us);

      // Flush any remaining frames in the encoder if the encoder started up (frame num > 0)
      if (avcodec_ctx->frame_num > 0 && avcodec_send_frame(avcodec_ctx.get(), nullptr) == 0) {
        packet_raw_avcodec pkt;
        while (avcodec_receive_packet(avcodec_ctx.get(), pkt.av_packet) == 0);
      }

      // Order matters here because the context relies on the hwdevice still being valid
      avcodec_ctx.reset();
      device.reset();
    }

    // Ensure objects are destroyed in the correct order
    /**
     * @brief Assign state from another instance while preserving ownership semantics.
     *
     * @param other Source object whose state is copied or moved into this object.
     * @return Reference or value produced by the operator.
     */
    avcodec_encode_session_t &operator=(avcodec_encode_session_t &&other) {
      device = std::move(other.device);
      avcodec_ctx = std::move(other.avcodec_ctx);
      replacements = std::move(other.replacements);
      sps = std::move(other.sps);
      vps = std::move(other.vps);

      inject = other.inject;
      warmup_pending = other.warmup_pending;
      measure_timing = other.measure_timing;
      encode_completion_us = std::move(other.encode_completion_us);

      return *this;
    }

    /**
     * @brief Encode one frame with FFmpeg AVCodec and prepare packet replacements.
     *
     * @param img Image or frame object to read from or populate.
     * @return Conversion status.
     */
    int convert(platf::img_t &img) override {
      if (!device) {
        return -1;
      }
      return device->convert(img);
    }

    /**
     * @brief Mark the frame as a request for an IDR frame.
     */
    void request_idr_frame() override {
      if (device && device->frame) {
        auto &frame = device->frame;
        frame->pict_type = AV_PICTURE_TYPE_I;
        frame->flags |= AV_FRAME_FLAG_KEY;
      }
    }

    /**
     * @brief Mark the frame as a request for a normal inter frame.
     */
    void request_normal_frame() override {
      if (device && device->frame) {
        auto &frame = device->frame;
        frame->pict_type = AV_PICTURE_TYPE_NONE;
        frame->flags &= ~AV_FRAME_FLAG_KEY;
      }
    }

    /**
     * @brief Mark the frame range whose references must be invalidated.
     *
     * @param first_frame First frame.
     * @param last_frame Last frame.
     */
    void invalidate_ref_frames(int64_t first_frame, int64_t last_frame) override {
      BOOST_LOG(error) << "Encoder doesn't support reference frame invalidation";
      request_idr_frame();
    }

    avcodec_ctx_t avcodec_ctx;  ///< FFmpeg codec context owned by the encode session.
    std::unique_ptr<platf::avcodec_encode_device_t> device;  ///< Platform device used by the FFmpeg hardware encoder.

    std::vector<packet_raw_t::replace_t> replacements;  ///< NAL-unit byte ranges that must be replaced before packet send.

    cbs::nal_t sps;  ///< Original and rewritten sequence parameter set for IDR injection.
    cbs::nal_t vps;  ///< Original and rewritten HEVC video parameter set for IDR injection.

    // inject sps/vps data into idr pictures
    int inject;  ///< Number of upcoming IDR frames that should receive rewritten parameter sets.
    bool warmup_pending;  ///< Whether the first encode should be discarded to absorb x264 startup allocation.
    bool measure_timing;  ///< Whether this PLANK x264 session should emit timing statistics.
    timing_samples_t encode_completion_us;  ///< Frame submission to first encoded packet availability.
  };

  /**
   * @brief NVENC encode session and device state for hardware encoding.
   */
  class nvenc_encode_session_t: public encode_session_t {
  public:
    /**
     * @brief Initialize an NVENC encode session and take ownership of its device.
     *
     * @param encode_device Encode device.
     */
    nvenc_encode_session_t(std::unique_ptr<platf::nvenc_encode_device_t> encode_device):
        device(std::move(encode_device)) {
    }

    /**
     * @brief Encode one frame with NVENC and return the packet payload.
     *
     * @param img Image or frame object to read from or populate.
     * @return Conversion status.
     */
    int convert(platf::img_t &img) override {
      if (!device) {
        return -1;
      }
      return device->convert(img);
    }

    /**
     * @brief Mark the frame as a request for an IDR frame.
     */
    void request_idr_frame() override {
      constexpr auto min_idr_interval = std::chrono::milliseconds {1000};
      const auto now = std::chrono::steady_clock::now();
      if (last_idr_request_time.time_since_epoch().count() != 0 &&
          now - last_idr_request_time < min_idr_interval) {
        BOOST_LOG(debug) << "NvEnc: rate-limiting repeated IDR request"sv;
        return;
      }
      force_idr = true;
      last_idr_request_time = now;
    }

    /**
     * @brief Mark the frame as a request for a normal inter frame.
     */
    void request_normal_frame() override {
      force_idr = false;
    }

    /**
     * @brief Mark the frame range whose references must be invalidated.
     *
     * @param first_frame First frame.
     * @param last_frame Last frame.
     */
    void invalidate_ref_frames(int64_t first_frame, int64_t last_frame) override {
      if (!device || !device->nvenc) {
        return;
      }

      if (!device->nvenc->invalidate_ref_frames(first_frame, last_frame)) {
        request_idr_frame();
      }
    }

    /**
     * @brief Submit the next frame to NVENC and return the encoded payload.
     *
     * @param frame_index Monotonic frame index assigned by the video pipeline.
     * @return Encoded NVENC frame payload and frame metadata.
     */
    nvenc::nvenc_encoded_frame encode_frame(uint64_t frame_index) {
      if (!device || !device->nvenc) {
        return {};
      }

      auto result = device->nvenc->encode_frame(frame_index, force_idr);
      force_idr = false;
      return result;
    }

  private:
    std::unique_ptr<platf::nvenc_encode_device_t> device;
    bool force_idr = false;
    std::chrono::steady_clock::time_point last_idr_request_time;
  };

  /**
   * @brief Capture thread context shared with the encoder session.
   */
  struct capture_ctx_t {
    img_event_t images;  ///< Queue of captured images waiting for encode.
    config_t config;  ///< Stream or encoder configuration captured for the worker.
  };

  /**
   * @brief Asynchronous capture thread state.
   */
  struct capture_thread_async_ctx_t {
    std::shared_ptr<safe::queue_t<capture_ctx_t>> capture_ctx_queue;  ///< Capture ctx queue.
    std::jthread capture_thread;  ///< Capture thread.

    safe::signal_t reinit_event;  ///< Reinit event.
    const encoder_t *encoder_p;  ///< Encoder p.
    sync_util::sync_t<std::weak_ptr<platf::display_t>> display_wp;  ///< Display wp.
  };

  /**
   * @brief Start the asynchronous capture thread.
   *
   * @param ctx Native context object used by the operation or callback.
   * @return 0 when the capture thread is started; nonzero on setup failure.
   */
  int start_capture_async(capture_thread_async_ctx_t &ctx);
  /**
   * @brief Stop capture async processing.
   *
   * @param ctx Native context object used by the operation or callback.
   */
  void end_capture_async(capture_thread_async_ctx_t &ctx);

  // Keep a reference counter to ensure the capture thread only runs when other threads have a reference to the capture thread
  auto capture_thread_async = safe::make_shared<capture_thread_async_ctx_t>(start_capture_async, end_capture_async);  ///< Capture thread async.

#if defined(__linux__) && defined(SUNSHINE_BUILD_CUDA)
  /**
   * @brief Direct CUDA NVENC backend qualified for each PLANK hardware profile.
   */
  encoder_t nvenc_direct {
    "nvenc-direct"sv,
    std::make_unique<encoder_platform_formats_nvenc>(
      platf::mem_type_e::cuda,
      platf::pix_fmt_e::nv12,
      platf::pix_fmt_e::p010,
      platf::pix_fmt_e::yuv444p,
      platf::pix_fmt_e::yuv444p16
    ),
    {"hevc_nvenc"s},
    {"h264_nvenc"s},
    REF_FRAMES_INVALIDATION | YUV444_SUPPORT | ASYNC_TEARDOWN
  };
#endif

#if defined(__linux__) && defined(SUNSHINE_BUILD_CUDA)
  /**
   * @brief NvFBC/CUDA capture with x264 software encoding.
   */
  encoder_t software_cuda {
    "software-cuda"sv,
    std::make_unique<encoder_platform_formats_avcodec>(
      AV_PIX_FMT_NONE,
      AV_PIX_FMT_NONE,
      AV_PIX_FMT_YUV444P,
      AV_PIX_FMT_YUV444P10,
      platf::mem_type_e::cuda,
      AV_PIX_FMT_YUV422P,
      AV_PIX_FMT_YUV422P10
    ),
    {},
    {"libx264"s},
    H264_ONLY | ALWAYS_REPROBE | YUV444_SUPPORT | YUV422_SUPPORT
  };
#endif

  static encoder_t *chosen_encoder;
  bool last_encoder_probe_supported_ref_frames_invalidation = false;  ///< Whether the last probe found reference-frame invalidation support.
  std::array<bool, 2> last_encoder_probe_supported_yuv444_for_codec = {};  ///< YUV444 support discovered for each probed codec.
  bool last_encoder_probe_supported_h264_10bit_444 = false;  ///< H.264 10-bit 4:4:4 support discovered for the selected encoder.
  bool last_encoder_probe_supported_h264_8bit_422 = false;  ///< H.264 8-bit 4:2:2 support discovered for the selected encoder.
  bool last_encoder_probe_supported_h264_10bit_422 = false;  ///< H.264 10-bit 4:2:2 support discovered for the selected encoder.
#if defined(__linux__) && defined(SUNSHINE_BUILD_CUDA)
  static bool nvenc_direct_qualified = false;
#endif

  bool nvenc_direct_supports_h264_444_8bit() {
#if defined(__linux__) && defined(SUNSHINE_BUILD_CUDA)
    return nvenc_direct_qualified && nvenc_direct.h264[encoder_t::PASSED] &&
           nvenc_direct.h264[encoder_t::YUV444];
#else
    return false;
#endif
  }

  bool nvenc_direct_supports_hevc_444_8bit() {
#if defined(__linux__) && defined(SUNSHINE_BUILD_CUDA)
    return nvenc_direct_qualified && nvenc_direct.hevc[encoder_t::PASSED] &&
           nvenc_direct.hevc[encoder_t::YUV444];
#else
    return false;
#endif
  }

  bool nvenc_direct_supports_hevc_444_10bit() {
#if defined(__linux__) && defined(SUNSHINE_BUILD_CUDA)
    return nvenc_direct_qualified && nvenc_direct.hevc[encoder_t::PASSED] &&
           nvenc_direct.hevc[encoder_t::TEN_BIT_YUV444];
#else
    return false;
#endif
  }

  bool encoder_backend_available(std::string_view backend) {
#if defined(__linux__) && defined(SUNSHINE_BUILD_CUDA)
    if (backend == "nvenc-direct"sv) {
      return nvenc_direct_supports_h264_444_8bit() ||
             nvenc_direct_supports_hevc_444_8bit() ||
             nvenc_direct_supports_hevc_444_10bit();
    }
    if (backend == "software-cuda"sv) {
      return software_cuda.h264[encoder_t::PASSED] &&
             software_cuda.h264[encoder_t::YUV444];
    }
#endif
    return false;
  }

  bool encoding_mode_available(std::string_view mode) {
#if defined(__linux__) && defined(SUNSHINE_BUILD_CUDA)
    const bool software_h264 = software_cuda.h264[encoder_t::PASSED];
    if (mode == "h264-8-422-software"sv) {
      return software_h264 && software_cuda.h264[encoder_t::YUV422];
    }
    if (mode == "h264-8-444-software"sv) {
      return software_h264 && software_cuda.h264[encoder_t::YUV444];
    }
    if (mode == "h264-10-422-software"sv) {
      return software_h264 && software_cuda.h264[encoder_t::TEN_BIT_YUV422];
    }
    if (mode == "h264-10-444-software"sv) {
      return software_h264 && software_cuda.h264[encoder_t::TEN_BIT_YUV444];
    }
    if (mode == "h264-8-444-nvenc"sv) {
      return nvenc_direct_supports_h264_444_8bit();
    }
    if (mode == "hevc-8-444-nvenc"sv) {
      return nvenc_direct_supports_hevc_444_8bit();
    }
    if (mode == "hevc-10-444-nvenc"sv) {
      return nvenc_direct_supports_hevc_444_10bit();
    }
#else
    (void) mode;
#endif
    return false;
  }

  bool capture_source_available(std::string_view source) {
#if defined(__linux__)
    return platf::plank_capture_source_available(source);
#else
    return false;
#endif
  }

  bool select_encoder_backend_for_session(std::string_view backend) {
    if (!encoder_backend_available(backend)) {
      return false;
    }
#if defined(__linux__) && defined(SUNSHINE_BUILD_CUDA)
    chosen_encoder = backend == "nvenc-direct"sv ? &nvenc_direct : &software_cuda;
    BOOST_LOG(info) << "PLANK selected per-session encoder backend ["sv
                    << chosen_encoder->name << ']';
    return true;
#else
    return false;
#endif
  }

  /**
   * @brief Recreate a display capture object after a capture failure.
   *
   * @param disp Display connection or display handle.
   * @param type Protocol, message, or resource type selector.
   * @param display_name Display name.
   * @param config Configuration values to apply.
   */
  void reset_display(std::shared_ptr<platf::display_t> &disp, const platf::mem_type_e &type, const std::string &display_name, const config_t &config) {
    // We try this twice, in case we still get an error on reinitialization
    for (int x = 0; x < 2; ++x) {
      disp.reset();
      disp = platf::display(type, display_name, config);
      if (disp) {
        break;
      }

      // The capture code depends on us to sleep between failures
      std::this_thread::sleep_for(200ms);
    }
  }

  /**
   * @brief Update the list of display names before or during a stream.
   * @details This will attempt to keep `current_display_index` pointing at the same display.
   * @param dev_type The encoder device type used for display lookup.
   * @param display_names The list of display names to repopulate.
   * @param current_display_index The current display index or -1 if not yet known.
   */
  void refresh_displays(platf::mem_type_e dev_type, const std::string &preferred_output_name, bool span_desktop, std::vector<std::string> &display_names, int &current_display_index) {
    if (span_desktop) {
      display_names.assign(1, std::string {});
      current_display_index = 0;
      return;
    }

    // It is possible that the output name may be empty even if it wasn't before (device disconnected) or vice-versa
    const auto output_name {preferred_output_name};
    std::string current_display_name;

    // If we have a current display index, let's start with that
    if (current_display_index >= 0 && current_display_index < display_names.size()) {
      current_display_name = display_names.at(current_display_index);
    }

    // Refresh the display names
    auto old_display_names = std::move(display_names);
    display_names = platf::display_names(dev_type);

    // If we now have no displays, let's put the old display array back and fail
    if (display_names.empty() && !old_display_names.empty()) {
      BOOST_LOG(error) << "No displays were found after reenumeration!"sv;
      display_names = std::move(old_display_names);
      return;
    } else if (display_names.empty()) {
      display_names.emplace_back(output_name);
    }

    // We now have a new display name list, so reset the index back to 0
    current_display_index = 0;

    // If we had a name previously, let's try to find it in the new list
    if (!current_display_name.empty()) {
      for (int x = 0; x < display_names.size(); ++x) {
        if (display_names[x] == current_display_name) {
          current_display_index = x;
          return;
        }
      }

      // The old display was removed, so we'll start back at the first display again
      BOOST_LOG(warning) << "Previous active display ["sv << current_display_name << "] is no longer present"sv;
    } else {
      for (int x = 0; x < display_names.size(); ++x) {
        if (display_names[x] == output_name) {
          current_display_index = x;
          return;
        }
      }

      int legacy_index = -1;
      const auto [end, error] = std::from_chars(
        output_name.data(),
        output_name.data() + output_name.size(),
        legacy_index
      );
      if (error == std::errc {} && end == output_name.data() + output_name.size() &&
          legacy_index >= 0 && legacy_index < display_names.size()) {
        BOOST_LOG(info) << "Resolved legacy numeric output ["sv << output_name
                        << "] to stable display ["sv << display_names[legacy_index] << ']';
        current_display_index = legacy_index;
      }
    }
  }

  /**
   * @brief Run the shared display capture thread for asynchronous encoding.
   *
   * @param capture_ctx_queue Capture context queue.
   * @param display_wp Weak pointer holder for the active display.
   * @param reinit_event Signal raised while the display is being reinitialized.
   * @param encoder Selected encoder.
   */
  void captureThread(
    std::shared_ptr<safe::queue_t<capture_ctx_t>> capture_ctx_queue,
    sync_util::sync_t<std::weak_ptr<platf::display_t>> &display_wp,
    safe::signal_t &reinit_event,
    const encoder_t &encoder
  ) {
    std::vector<capture_ctx_t> capture_ctxs;

    auto fg = util::fail_guard([&]() {
      capture_ctx_queue->stop();

      // Stop all sessions listening to this thread
      for (auto &capture_ctx : capture_ctxs) {
        capture_ctx.images->stop();
      }
      for (auto &capture_ctx : capture_ctx_queue->unsafe()) {
        capture_ctx.images->stop();
      }
    });

    auto switch_display_event = mail::man->event<int>(mail::switch_display);
    auto desktop_reattach_event =
      mail::man->event<std::uint64_t>(mail::desktop_reattach);
#ifdef __linux__
    auto attached_generation = plank::session::desktop_generation();
#endif

    // Wait for the initial capture context or a request to stop the queue
    auto initial_capture_ctx = capture_ctx_queue->pop();
    if (!initial_capture_ctx) {
      return;
    }
    capture_ctxs.emplace_back(std::move(*initial_capture_ctx));

    // Get all the monitor names now, rather than at boot, to
    // get the most up-to-date list available monitors
    std::vector<std::string> display_names;
    int display_p = -1;
    refresh_displays(encoder.platform_formats->dev_type, capture_ctxs.front().config.output_name, capture_ctxs.front().config.span_desktop, display_names, display_p);
    auto disp = platf::display(encoder.platform_formats->dev_type, display_names[display_p], capture_ctxs.front().config);
    if (!disp) {
      return;
    }
    display_wp = disp;

    constexpr auto capture_buffer_size = 12;
    std::list<std::shared_ptr<platf::img_t>> imgs(capture_buffer_size);

    std::vector<std::optional<std::chrono::steady_clock::time_point>> imgs_used_timestamps;
    const std::chrono::seconds trim_timeot = 3s;
    auto trim_imgs = [&]() {
      // count allocated and used within current pool
      size_t allocated_count = 0;
      size_t used_count = 0;
      for (const auto &img : imgs) {
        if (img) {
          allocated_count += 1;
          if (img.use_count() > 1) {
            used_count += 1;
          }
        }
      }

      // remember the timestamp of currently used count
      const auto now = std::chrono::steady_clock::now();
      if (imgs_used_timestamps.size() <= used_count) {
        imgs_used_timestamps.resize(used_count + 1);
      }
      imgs_used_timestamps[used_count] = now;

      // decide whether to trim allocated unused above the currently used count
      // based on last used timestamp and universal timeout
      size_t trim_target = used_count;
      for (size_t i = used_count; i < imgs_used_timestamps.size(); i++) {
        if (imgs_used_timestamps[i] && now - *imgs_used_timestamps[i] < trim_timeot) {
          trim_target = i;
        }
      }

      // trim allocated unused above the newly decided trim target
      if (allocated_count > trim_target) {
        size_t to_trim = allocated_count - trim_target;
        // prioritize trimming least recently used
        for (auto it = imgs.rbegin(); it != imgs.rend(); it++) {
          auto &img = *it;
          if (img && img.use_count() == 1) {
            img.reset();
            to_trim -= 1;
            if (to_trim == 0) {
              break;
            }
          }
        }
        // forget timestamps that no longer relevant
        imgs_used_timestamps.resize(trim_target + 1);
      }
    };

    auto pull_free_image_callback = [&](std::shared_ptr<platf::img_t> &img_out) -> bool {
      img_out.reset();
      while (capture_ctx_queue->running()) {
        // pick first allocated but unused
        for (auto it = imgs.begin(); it != imgs.end(); it++) {
          if (*it && it->use_count() == 1) {
            img_out = *it;
            if (it != imgs.begin()) {
              // move image to the front of the list to prioritize its reusal
              imgs.erase(it);
              imgs.push_front(img_out);
            }
            break;
          }
        }
        // otherwise pick first unallocated
        if (!img_out) {
          for (auto it = imgs.begin(); it != imgs.end(); it++) {
            if (!*it) {
              // allocate image
              *it = disp->alloc_img();
              img_out = *it;
              if (it != imgs.begin()) {
                // move image to the front of the list to prioritize its reusal
                imgs.erase(it);
                imgs.push_front(img_out);
              }
              break;
            }
          }
        }
        if (img_out) {
          // trim allocated but unused portion of the pool based on timeouts
          trim_imgs();
          img_out->frame_timestamp.reset();
          return true;
        } else {
          // sleep and retry if image pool is full
          std::this_thread::sleep_for(1ms);
        }
      }
      return false;
    };

    // Capture takes place on this thread
    platf::set_thread_name("video::capture");
    platf::adjust_thread_priority(platf::thread_priority_e::critical);
    while (capture_ctx_queue->running()) {
      bool artificial_reinit = false;

      auto push_captured_image_callback = [&](std::shared_ptr<platf::img_t> &&img, bool frame_captured) -> bool {
        KITTY_WHILE_LOOP(auto capture_ctx = std::begin(capture_ctxs), capture_ctx != std::end(capture_ctxs), {
          if (!capture_ctx->images->running()) {
            capture_ctx = capture_ctxs.erase(capture_ctx);

            continue;
          }

          if (frame_captured) {
            capture_ctx->images->raise(img);
          }

          ++capture_ctx;
        })

        if (capture_ctxs.empty()) {
          return false;
        }

        if (!capture_ctx_queue->running()) {
          return false;
        }

        while (capture_ctx_queue->peek()) {
          capture_ctxs.emplace_back(std::move(*capture_ctx_queue->pop()));
        }

        if (switch_display_event->peek() || desktop_reattach_event->peek()) {
          artificial_reinit = true;
          return false;
        }

        return true;
      };

      // PLANK always transports cursor shape separately. It is never
      // composited into the captured video frame.
      bool capture_cursor = false;
      auto status = disp->capture(push_captured_image_callback, pull_free_image_callback, &capture_cursor);

#ifdef __linux__
      if (status == platf::capture_e::error &&
          getenv("PLANK_SESSION_CONTROL_FD") != nullptr) {
        const auto deadline = std::chrono::steady_clock::now() + 10s;
        while (capture_ctx_queue->running() &&
               plank::session::desktop_generation() == attached_generation &&
               std::chrono::steady_clock::now() < deadline) {
          std::this_thread::sleep_for(50ms);
        }
        if (plank::session::desktop_generation() != attached_generation) {
          status = platf::capture_e::reinit;
        }
      }
#endif

      if (artificial_reinit && status != platf::capture_e::error) {
        status = platf::capture_e::reinit;

        artificial_reinit = false;
      }

      switch (status) {
        case platf::capture_e::reinit:
          {
            while (desktop_reattach_event->peek()) {
              const auto generation = desktop_reattach_event->pop();
              if (generation) {
                BOOST_LOG(info) << "Reinitializing video for desktop generation "sv
                                << *generation;
              }
            }
#ifdef __linux__
            attached_generation = plank::session::desktop_generation();
#endif
            reinit_event.raise(true);

            // Some classes of images contain references to the display --> display won't delete unless img is deleted
            for (auto &img : imgs) {
              img.reset();
            }

            // display_wp is modified in this thread only
            // Wait for the other shared_ptr's of display to be destroyed.
            // New displays will only be created in this thread.
            while (display_wp->use_count() != 1) {
              // Free images that weren't consumed by the encoders. These can reference the display and prevent
              // the ref count from reaching 1. We do this here rather than on the encoder thread to avoid race
              // conditions where the encoding loop might free a good frame after reinitializing if we capture
              // a new frame here before the encoder has finished reinitializing.
              KITTY_WHILE_LOOP(auto capture_ctx = std::begin(capture_ctxs), capture_ctx != std::end(capture_ctxs), {
                if (!capture_ctx->images->running()) {
                  capture_ctx = capture_ctxs.erase(capture_ctx);
                  continue;
                }

                while (capture_ctx->images->try_pop()) {
                }

                ++capture_ctx;
              });

              std::this_thread::sleep_for(20ms);
            }

            while (capture_ctx_queue->running()) {
              // Release the display before reenumerating displays, since some capture backends
              // only support a single display session per device/application.
              disp.reset();

              // Refresh display names since a display removal might have caused the reinitialization
              refresh_displays(encoder.platform_formats->dev_type, capture_ctxs.front().config.output_name, capture_ctxs.front().config.span_desktop, display_names, display_p);

              // Process any pending display switch with the new list of displays
              if (switch_display_event->peek()) {
                display_p = std::clamp(*switch_display_event->pop(), 0, static_cast<int>(display_names.size()) - 1);
              }

              // reset_display() will sleep between retries
              reset_display(disp, encoder.platform_formats->dev_type, display_names[display_p], capture_ctxs.front().config);
              if (disp) {
                break;
              }
            }
            if (!disp) {
              return;
            }

            display_wp = disp;

            reinit_event.reset();
            continue;
          }
        case platf::capture_e::error:
        case platf::capture_e::ok:
        case platf::capture_e::timeout:
        case platf::capture_e::interrupted:
          return;
        default:
          BOOST_LOG(error) << "Unrecognized capture status ["sv << std::to_underlying(status) << ']';
          return;
      }
    }
  }

  /**
   * @brief Drain encoded packets from an FFmpeg encoder session.
   *
   * @param frame_nr Monotonic frame index assigned by the video pipeline.
   * @param session Active FFmpeg encoder session.
   * @param packets Output queue that receives encoded packets.
   * @param channel_data Platform or protocol state attached to each packet.
   * @param frame_timestamp Capture timestamp associated with the encoded frame.
   * @return 0 when packets are queued; nonzero when encoding or packetization fails.
   */
  int encode_avcodec(int64_t frame_nr, avcodec_encode_session_t &session, safe::mail_raw_t::queue_t<packet_t> &packets, void *channel_data, std::optional<std::chrono::steady_clock::time_point> frame_timestamp) {
    auto &frame = session.device->frame;
    auto &ctx = session.avcodec_ctx;

    if (session.warmup_pending) {
      frame->pts = -1;
      frame->pict_type = AV_PICTURE_TYPE_I;
      frame->flags |= AV_FRAME_FLAG_KEY;
      auto warmup_status = avcodec_send_frame(ctx.get(), frame);
      if (warmup_status < 0) {
        return warmup_status;
      }
      packet_raw_avcodec warmup_packet;
      while ((warmup_status = avcodec_receive_packet(ctx.get(), warmup_packet.av_packet)) >= 0) {
        av_packet_unref(warmup_packet.av_packet);
      }
      if (warmup_status != AVERROR(EAGAIN) && warmup_status != AVERROR_EOF) {
        return warmup_status;
      }
      session.warmup_pending = false;
    }

    frame->pts = frame_nr;

    auto &sps = session.sps;
    auto &vps = session.vps;
    const auto encode_start = std::chrono::steady_clock::now();
    bool completion_recorded = false;

    // send the frame to the encoder
    auto ret = avcodec_send_frame(ctx.get(), frame);
    if (ret < 0) {
      char err_str[AV_ERROR_MAX_STRING_SIZE] {0};
      BOOST_LOG(error) << "Could not send a frame for encoding: "sv << av_make_error_string(err_str, AV_ERROR_MAX_STRING_SIZE, ret);

      return -1;
    }

    while (ret >= 0) {
      auto packet = std::make_unique<packet_raw_avcodec>();
      auto av_packet = packet.get()->av_packet;

      ret = avcodec_receive_packet(ctx.get(), av_packet);
      if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
        return 0;
      } else if (ret < 0) {
        return ret;
      }

      if (session.measure_timing && !completion_recorded) {
        session.encode_completion_us.push_back(
          std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - encode_start).count()
        );
        completion_recorded = true;
      }

      if (av_packet->flags & AV_PKT_FLAG_KEY) {
        BOOST_LOG(debug) << "Frame "sv << frame_nr << ": IDR Keyframe (AV_FRAME_FLAG_KEY)"sv;
      }

      if ((frame->flags & AV_FRAME_FLAG_KEY) && !(av_packet->flags & AV_PKT_FLAG_KEY)) {
        BOOST_LOG(error) << "Encoder did not produce IDR frame when requested!"sv;
      }

      if (session.inject) {
        if (session.inject == 1) {
          auto h264 = cbs::make_sps_h264(ctx.get(), av_packet);

          sps = std::move(h264.sps);
        } else {
          auto hevc = cbs::make_sps_hevc(ctx.get(), av_packet);

          sps = std::move(hevc.sps);
          vps = std::move(hevc.vps);

          session.replacements.emplace_back(
            std::string_view(reinterpret_cast<const char *>(std::begin(vps.old)), vps.old.size()),
            std::string_view(reinterpret_cast<const char *>(std::begin(vps._new)), vps._new.size())
          );
        }

        session.inject = 0;

        session.replacements.emplace_back(
          std::string_view(reinterpret_cast<const char *>(std::begin(sps.old)), sps.old.size()),
          std::string_view(reinterpret_cast<const char *>(std::begin(sps._new)), sps._new.size())
        );
      }

      if (av_packet && av_packet->pts == frame_nr) {
        packet->frame_timestamp = frame_timestamp;
      }

      packet->replacements = &session.replacements;
      packet->channel_data = channel_data;
      packets->raise(std::move(packet));
    }

    return 0;
  }

  /**
   * @brief Encode one frame through NVENC and queue the resulting packet.
   *
   * @param frame_nr Monotonic frame index assigned by the video pipeline.
   * @param session Active NVENC encoder session.
   * @param packets Output queue that receives the encoded packet.
   * @param channel_data Platform or protocol state attached to the packet.
   * @param frame_timestamp Capture timestamp associated with the encoded frame.
   * @return 0 when packets are queued; nonzero when NVENC encoding fails.
   */
  int encode_nvenc(int64_t frame_nr, nvenc_encode_session_t &session, safe::mail_raw_t::queue_t<packet_t> &packets, void *channel_data, std::optional<std::chrono::steady_clock::time_point> frame_timestamp) {
    auto encoded_frame = session.encode_frame(frame_nr);
    if (encoded_frame.data.empty()) {
      BOOST_LOG(error) << "NvENC returned empty packet";
      return -1;
    }

    if (frame_nr != encoded_frame.frame_index) {
      BOOST_LOG(error) << "NvENC frame index mismatch " << frame_nr << " " << encoded_frame.frame_index;
    }

    auto packet = std::make_unique<packet_raw_generic>(std::move(encoded_frame.data), encoded_frame.frame_index, encoded_frame.idr);
    packet->channel_data = channel_data;
    packet->after_ref_frame_invalidation = encoded_frame.after_ref_frame_invalidation;
    packet->frame_timestamp = frame_timestamp;
    packets->raise(std::move(packet));

    return 0;
  }

  /**
   * @brief Encode one captured frame and queue packets for transmission.
   *
   * @param frame_nr Frame nr.
   * @param session Active streaming or pairing session for the request.
   * @param packets Packets queued or emitted by the stream.
   * @param channel_data Channel data.
   * @param frame_timestamp Frame timestamp.
   * @return 0 when the frame is encoded and queued; nonzero on encoder failure.
   */
  int encode(int64_t frame_nr, encode_session_t &session, safe::mail_raw_t::queue_t<packet_t> &packets, void *channel_data, std::optional<std::chrono::steady_clock::time_point> frame_timestamp) {
    if (auto avcodec_session = dynamic_cast<avcodec_encode_session_t *>(&session)) {
      return encode_avcodec(frame_nr, *avcodec_session, packets, channel_data, frame_timestamp);
    } else if (auto nvenc_session = dynamic_cast<nvenc_encode_session_t *>(&session)) {
      return encode_nvenc(frame_nr, *nvenc_session, packets, channel_data, frame_timestamp);
    }

    return -1;
  }

  /**
   * @brief Create an AVCodec encode session.
   *
   * @param disp Display being encoded.
   * @param encoder Selected encoder.
   * @param config Video configuration.
   * @param encode_device AVCodec encode device.
   * @return AVCodec encode session, or nullptr on failure.
   */
  std::unique_ptr<avcodec_encode_session_t> make_avcodec_encode_session(
    platf::display_t *disp,
    const encoder_t &encoder,
    const config_t &config,
    std::unique_ptr<platf::avcodec_encode_device_t> encode_device
  ) {
    auto platform_formats = dynamic_cast<const encoder_platform_formats_avcodec *>(encoder.platform_formats.get());
    if (!platform_formats) {
      return nullptr;
    }

    auto &video_format = encoder.codec_from_config(config);
    if (!video_format[encoder_t::PASSED] || !disp->is_codec_supported(video_format.name, config)) {
      BOOST_LOG(error) << encoder.name << ": "sv << video_format.name << " mode not supported"sv;
      return nullptr;
    }

    if (config.chromaSamplingType == 1) {
      if (config.dynamicRange) {
        if (!video_format[encoder_t::TEN_BIT_YUV444]) {
          BOOST_LOG(error) << video_format.name << ": 10-bit YUV 4:4:4 not supported"sv;
          return nullptr;
        }
      } else if (!video_format[encoder_t::YUV444]) {
        BOOST_LOG(error) << video_format.name << ": 8-bit YUV 4:4:4 not supported"sv;
        return nullptr;
      }

    } else if (config.chromaSamplingType == 2) {
      if (!(encoder.flags & YUV422_SUPPORT) || !video_format[encoder_t::YUV422]) {
        BOOST_LOG(error) << video_format.name << ": YUV 4:2:2 not supported"sv;
        return nullptr;
      }
      if (config.dynamicRange && !video_format[encoder_t::TEN_BIT_YUV422]) {
        BOOST_LOG(error) << video_format.name << ": 10-bit YUV 4:2:2 not supported"sv;
        return nullptr;
      }
    } else {
      if (config.dynamicRange && !video_format[encoder_t::TEN_BIT]) {
        BOOST_LOG(error) << video_format.name << ": dynamic range not supported"sv;
        return nullptr;
      }
    }

    auto colorspace = encode_device->colorspace;
    const std::string &codec_name = video_format.name;
    auto codec = avcodec_find_encoder_by_name(codec_name.c_str());
    if (!codec) {
      BOOST_LOG(error) << "Couldn't open ["sv << codec_name << ']';

      return nullptr;
    }

#ifdef __linux__
    if (encoder.name == "software-cuda") {
      expand_software_encoder_cpu_affinity();
    }
#endif

    auto sw_fmt = (colorspace.bit_depth == 8 && config.chromaSamplingType == 0)  ? platform_formats->avcodec_pix_fmt_8bit :
                  (colorspace.bit_depth == 8 && config.chromaSamplingType == 1)  ? platform_formats->avcodec_pix_fmt_yuv444_8bit :
                  (colorspace.bit_depth == 10 && config.chromaSamplingType == 0) ? platform_formats->avcodec_pix_fmt_10bit :
                  (colorspace.bit_depth == 10 && config.chromaSamplingType == 1) ? platform_formats->avcodec_pix_fmt_yuv444_10bit :
                  (colorspace.bit_depth == 8 && config.chromaSamplingType == 2)  ? platform_formats->avcodec_pix_fmt_yuv422_8bit :
                  (colorspace.bit_depth == 10 && config.chromaSamplingType == 2) ? platform_formats->avcodec_pix_fmt_yuv422_10bit :
                                                                                   AV_PIX_FMT_NONE;

    avcodec_ctx_t ctx {avcodec_alloc_context3(codec)};
    if (!ctx) {
      return nullptr;
    }
    {
      ctx->width = config.width;
      ctx->height = config.height;
      const AVRational fps = video::framerate_to_rational(config);
      ctx->framerate = fps;
      ctx->time_base = AVRational {fps.den, fps.num};

      ctx->profile = select_h264_profile(config);

      // B-frames delay decoder output, so never use them
      ctx->max_b_frames = 0;

      // Use an infinite GOP length since I-frames are generated on demand
      ctx->gop_size = std::numeric_limits<int>::max();
      ctx->keyint_min = std::numeric_limits<int>::max();

      // x264 otherwise inherits the effectively infinite minimum keyframe
      // interval above, which prevents its scene-cut detector from emitting an
      // adaptive I-frame. Keep the infinite maximum GOP while allowing a hard
      // cut to reset prediction when explicitly requested.
      if (config::video.sw.scenecut > 0) {
        ctx->keyint_min = 1;
      }

      // Some client decoders have limits on the number of reference frames
      if (config.numRefFrames) {
        if (video_format[encoder_t::REF_FRAMES_RESTRICT]) {
          ctx->refs = config.numRefFrames;
        } else {
          BOOST_LOG(warning) << "Client requested reference frame limit, but encoder doesn't support it!"sv;
        }
      }

      // We forcefully reset the flags to avoid clash on reuse of AVCodecContext
      ctx->flags = 0;
      ctx->flags |= AV_CODEC_FLAG_CLOSED_GOP | AV_CODEC_FLAG_LOW_DELAY;

      ctx->flags2 |= AV_CODEC_FLAG2_FAST;

      auto avcodec_colorspace = avcodec_colorspace_from_sunshine_colorspace(colorspace);

      ctx->color_range = avcodec_colorspace.range;
      ctx->color_primaries = avcodec_colorspace.primaries;
      ctx->color_trc = avcodec_colorspace.transfer_function;
      ctx->colorspace = avcodec_colorspace.matrix;

      // Used by cbs::make_sps_hevc
      ctx->sw_pix_fmt = sw_fmt;

      ctx->pix_fmt = sw_fmt;
      ctx->slices = std::max(config.slicesPerFrame, config::video.min_threads);

      ctx->thread_type = FF_THREAD_SLICE;
      ctx->thread_count = ctx->slices;

      AVDictionary *options {nullptr};
      if (!config::video.sw.sw_preset.empty()) {
        av_dict_set(&options, "preset", config::video.sw.sw_preset.c_str(), 0);
      }
      if (!config::video.sw.sw_tune.empty()) {
        av_dict_set(&options, "tune", config::video.sw.sw_tune.c_str(), 0);
      }
      av_dict_set_int(&options, "sc_threshold", config::video.sw.scenecut, 0);
      auto release_options = util::fail_guard([&]() {
        av_dict_free(&options);
      });

      const std::int64_t bitrate = config.bitrate * 1000LL;
      BOOST_LOG(info) << "Streaming bitrate is " << bitrate;
      const auto sw_rate_control = software_rate_control(
        bitrate,
        config.framerate,
        config::video.sw.vbv_maxrate_percentage,
        config::video.sw.vbv_buffer_frames
      );
      ctx->rc_max_rate = sw_rate_control.peak_rate;
      ctx->bit_rate = bitrate;

      if (ctx->rc_max_rate == ctx->bit_rate) {
        ctx->rc_min_rate = bitrate;
      }
      {
        if (config::video.sw.vbv_buffer_frames > 0) {
          // Express the reservoir in frame units at the requested average
          // rate. This permits a short peak after a hard cut without raising
          // the stream's sustained bandwidth target.
          ctx->rc_buffer_size = sw_rate_control.buffer_size;
        } else if (ctx->slices > 1) {
          // Use a larger rc_buffer_size for software encoding when slices are enabled,
          // because libx264 can severely degrade quality if the buffer is too small.
          ctx->rc_buffer_size = bitrate / ((config.framerate * 10) / 15);
        } else {
          ctx->rc_buffer_size = bitrate / config.framerate;
        }
      }

      {
        BOOST_LOG(info)
          << "Software rate control: average=" << ctx->bit_rate
          << ", peak=" << ctx->rc_max_rate
          << ", vbv=" << ctx->rc_buffer_size
          << ", slices=" << ctx->slices
          << ", scenecut=" << config::video.sw.scenecut;
      }

      if (auto status = avcodec_open2(ctx.get(), codec, &options)) {
        char error[AV_ERROR_MAX_STRING_SIZE] {};
        BOOST_LOG(error) << "Could not open codec [" << codec_name << "]: "
                         << av_make_error_string(error, sizeof(error), status);
        return nullptr;
      }
    }

    avcodec_frame_t frame {av_frame_alloc()};
    if (!frame) {
      return nullptr;
    }
    frame->format = ctx->pix_fmt;
    frame->width = ctx->width;
    frame->height = ctx->height;
    frame->color_range = ctx->color_range;
    frame->color_primaries = ctx->color_primaries;
    frame->color_trc = ctx->color_trc;
    frame->colorspace = ctx->colorspace;
    frame->chroma_location = ctx->chroma_sample_location;

    if (encode_device->set_frame(frame.release())) {
      return nullptr;
    }

    encode_device->apply_colorspace();

    auto session = std::make_unique<avcodec_encode_session_t>(
      std::move(ctx),
      std::move(encode_device),

      // 0 ==> don't inject, 1 ==> inject for h264, 2 ==> inject for hevc
      config.videoFormat <= 1 ? (1 - static_cast<int>(video_format[encoder_t::VUI_PARAMETERS])) * (1 + config.videoFormat) : 0,
      encoder.name == "software-cuda"sv
    );

    return session;
  }

  /**
   * @brief Create NVENC encode session.
   *
   * @param client_config Client stream configuration negotiated for this session.
   * @param encode_device Encode device.
   * @return Constructed NVENC encode session object.
   */
  std::unique_ptr<nvenc_encode_session_t> make_nvenc_encode_session(const config_t &client_config, std::unique_ptr<platf::nvenc_encode_device_t> encode_device) {
    if (!encode_device->init_encoder(client_config, encode_device->colorspace)) {
      return nullptr;
    }

    return std::make_unique<nvenc_encode_session_t>(std::move(encode_device));
  }

  /**
   * @brief Create encode session.
   *
   * @param disp Display connection or display handle.
   * @param encoder Encoder configuration or encoder instance.
   * @param config Configuration values to apply.
   * @param encode_device Encode device.
   * @return Constructed encode session object.
   */
  std::unique_ptr<encode_session_t> make_encode_session(platf::display_t *disp, const encoder_t &encoder, const config_t &config, std::unique_ptr<platf::encode_device_t> encode_device) {
    if (dynamic_cast<platf::avcodec_encode_device_t *>(encode_device.get())) {
      auto avcodec_encode_device = boost::dynamic_pointer_cast<platf::avcodec_encode_device_t>(std::move(encode_device));
      return make_avcodec_encode_session(disp, encoder, config, std::move(avcodec_encode_device));
    } else if (dynamic_cast<platf::nvenc_encode_device_t *>(encode_device.get())) {
      auto nvenc_encode_device = boost::dynamic_pointer_cast<platf::nvenc_encode_device_t>(std::move(encode_device));
      return make_nvenc_encode_session(config, std::move(nvenc_encode_device));
    }

    return nullptr;
  }

  /**
   * @brief Run one encode loop for a display capture stream.
   *
   * @param frame_nr Frame counter updated as frames are encoded.
   * @param mail Session mail bus.
   * @param images Captured image event source.
   * @param config Video configuration.
   * @param disp Display being encoded.
   * @param encode_device Platform encode device.
   * @param reinit_event Signal raised while the encoder/display is reinitializing.
   * @param encoder Selected encoder.
   * @param channel_data Opaque channel data passed to packets.
   */
  void encode_run(
    int &frame_nr,  // Store progress of the frame number
    safe::mail_t mail,
    img_event_t images,
    config_t &config,
    std::shared_ptr<platf::display_t> disp,
    std::unique_ptr<platf::encode_device_t> encode_device,
    safe::signal_t &reinit_event,
    const encoder_t &encoder,
    void *channel_data
  ) {
    auto session = make_encode_session(disp.get(), encoder, config, std::move(encode_device));
    if (!session) {
      return;
    }
    integrated_timing_samples_t timing {
      encoder.name == "nvenc-direct"sv ? "nvenc_upload_and_conversion"sv :
      config.capture_source == capture_source_e::x11_native10 ?
                                         "native10_rgb_to_gbr_conversion"sv :
                                         "cuda_to_cpu_conversion"sv
    };

    // As a workaround for NVENC hangs and to generally speed up encoder reinit,
    // we will complete the encoder teardown in a separate thread if supported.
    // This will move expensive processing off the encoder thread to allow us
    // to restart encoding as soon as possible. For cases where the NVENC driver
    // hang occurs, this thread may probably never exit, but it will allow
    // streaming to continue without requiring a full restart of Sunshine.
    auto fail_guard = util::fail_guard([&encoder, &session] {
      if (encoder.flags & ASYNC_TEARDOWN) {
        std::jthread encoder_teardown_thread {[session = std::move(session)]() mutable {
          BOOST_LOG(info) << "Starting async encoder teardown";
          session.reset();
          BOOST_LOG(info) << "Async encoder teardown complete";
        }};
        encoder_teardown_thread.detach();
      }
    });

    // set max frame time based on client-requested target framerate.
    double minimum_fps_target = (config::video.minimum_fps_target > 0.0) ? config::video.minimum_fps_target : (config.framerate / 2);
    std::chrono::duration<double, std::milli> max_frametime {1000.0 / minimum_fps_target};
    BOOST_LOG(info) << "Minimum FPS target set to ~"sv << minimum_fps_target << "fps ("sv << max_frametime.count() << "ms)"sv;

    auto shutdown_event = mail->event<bool>(mail::shutdown);
    auto packets = mail::man->queue<packet_t>(mail::video_packets);
    auto idr_events = mail->event<bool>(mail::idr);
    auto invalidate_ref_frames_events = mail->event<std::pair<int64_t, int64_t>>(mail::invalidate_ref_frames);
    auto bitrate_events = mail->event<int>(mail::video_bitrate);

    {
      // Load a dummy image into the AVFrame to ensure we have something to encode
      // even if we timeout waiting on the first frame. This is a relatively large
      // allocation which can be freed immediately after convert(), so we do this
      // in a separate scope.
      auto dummy_img = disp->alloc_img();
      if (!dummy_img || disp->dummy_img(dummy_img.get()) || session->convert(*dummy_img)) {
        return;
      }
    }

    while (true) {
      bool requested_idr_frame = false;

      std::optional<int> requested_bitrate_kbps;
      while (bitrate_events->peek()) {
        requested_bitrate_kbps = bitrate_events->pop(0ms);
      }
      if (requested_bitrate_kbps && *requested_bitrate_kbps != config.bitrate) {
        // x264 only supports changing an active bitrate when its ABR engine is
        // operating as CBR. PLANK's qualified bounded-ABR profile
        // deliberately uses a raised VBV peak, so changing AVCodecContext fields
        // would update public parameters while leaving x264's real sustained
        // target unchanged. Return to capture_async(), which preserves the
        // stream and display while creating a fresh encoder at the new target.
        BOOST_LOG(info) << "Recreating active PLANK ABR encoder: old="sv
                        << config.bitrate << " Kbps, new="sv
                        << *requested_bitrate_kbps << " Kbps"sv;
        config.bitrate = *requested_bitrate_kbps;
        return;
      }

      while (invalidate_ref_frames_events->peek()) {
        if (auto frames = invalidate_ref_frames_events->pop(0ms)) {
          session->invalidate_ref_frames(frames->first, frames->second);
        }
      }

      if (idr_events->peek()) {
        requested_idr_frame = true;
        idr_events->pop();
      }

      if (requested_idr_frame) {
        session->request_idr_frame();
      }

      std::optional<std::chrono::steady_clock::time_point> frame_timestamp;

      // Encode at a minimum FPS to avoid image quality issues with static content
      if (!requested_idr_frame || images->peek()) {
        if (auto img = images->pop(max_frametime)) {
          frame_timestamp = img->frame_timestamp;
          const auto conversion_start = std::chrono::steady_clock::now();
          if (frame_timestamp) {
            timing.display_to_convert_start_us.push_back(
              std::chrono::duration<double, std::micro>(conversion_start - *frame_timestamp).count()
            );
          }
          if (session->convert(*img)) {
            BOOST_LOG(error) << "Could not convert image"sv;
            return;
          }
          if (frame_timestamp) {
            timing.conversion_us.push_back(
              std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - conversion_start).count()
            );
          }
        } else if (!images->running()) {
          break;
        }
      }

      // Break out of the encoding loop if any of the following are true:
      // a) The stream is ending
      // b) Sunshine is quitting
      // c) The capture side is waiting to reinit and we've encoded at least one frame
      //
      // If we have to reinit before we have received any captured frames, we will encode
      // the blank dummy frame just to let Moonlight know that we're alive.
      //
      // Ensure that this check occurs as close as possible to the encode call to prevent packets
      // in flight after encoder teardown.
      if (shutdown_event->peek() || !images->running() || (reinit_event.peek() && frame_nr > 1)) {
        break;
      }

      const auto encode_start = std::chrono::steady_clock::now();
      if (encode(frame_nr++, *session, packets, channel_data, frame_timestamp)) {
        BOOST_LOG(error) << "Could not encode video packet"sv;
        return;
      }
      const auto packet_ready = std::chrono::steady_clock::now();
      if (frame_timestamp) {
        timing.encode_and_packetize_us.push_back(
          std::chrono::duration<double, std::micro>(packet_ready - encode_start).count()
        );
        timing.display_to_packet_ready_us.push_back(
          std::chrono::duration<double, std::micro>(packet_ready - *frame_timestamp).count()
        );
        if (timing.last_packet_ready) {
          timing.packet_ready_cadence_us.push_back(
            std::chrono::duration<double, std::micro>(packet_ready - *timing.last_packet_ready).count()
          );
        }
        timing.last_packet_ready = packet_ready;
      }

      session->request_normal_frame();

      // While streaming check to see if the mouse is present and enable Mouse Keys to force the cursor to appear
      // This is useful for KVM switch scenarios where mouse may disappear during streaming
      platf::enable_mouse_keys();
    }
  }

  /**
   * @brief Create a port object or message.
   *
   * @param display Display object or identifier associated with the operation.
   * @param config Configuration values to apply.
   * @return Constructed port object.
   */
  input::touch_port_t make_port(platf::display_t *display, const config_t &config) {
    float wd = display->width;
    float hd = display->height;

    float wt = config.width;
    float ht = config.height;

    auto scalar = std::fminf(wt / wd, ht / hd);

    // we initialize scalar_tpcoords and logical dimensions to default values in case they are not set (non-KMS)
    float scalar_tpcoords = 1.0f;
    int display_env_logical_width = 0;
    int display_env_logical_height = 0;
    if (display->logical_width > 0 && display->logical_height > 0 && display->env_logical_width > 0 && display->env_logical_height > 0) {
      float lwd = display->logical_width;
      float lhd = display->logical_height;
      scalar_tpcoords = std::fminf(wd / lwd, hd / lhd);
      display_env_logical_width = display->env_logical_width;
      display_env_logical_height = display->env_logical_height;
    }

    auto w2 = scalar * wd;
    auto h2 = scalar * hd;

    auto offsetX = (config.width - w2) * 0.5f;
    auto offsetY = (config.height - h2) * 0.5f;

    return input::touch_port_t {
      {
        display->offset_x,
        display->offset_y,
        config.width,
        config.height,
      },
      display->env_width,
      display->env_height,
      offsetX,
      offsetY,
      1.0f / scalar,
      scalar_tpcoords,
      display_env_logical_width,
      display_env_logical_height
    };
  }

  /**
   * @brief Create encode device.
   *
   * @param disp Display connection or display handle.
   * @param encoder Encoder configuration or encoder instance.
   * @param config Configuration values to apply.
   * @return Constructed encode device object.
   */
  std::unique_ptr<platf::encode_device_t> make_encode_device(platf::display_t &disp, const encoder_t &encoder, const config_t &config) {
    std::unique_ptr<platf::encode_device_t> result;

    auto colorspace = colorspace_from_client_config(config);

    platf::pix_fmt_e pix_fmt;
    if (config.chromaSamplingType == 1) {
      // YUV 4:4:4
      if (!(encoder.flags & YUV444_SUPPORT)) {
        // Encoder can't support YUV 4:4:4 regardless of hardware capabilities
        return {};
      }
      pix_fmt = (colorspace.bit_depth == 10) ?
                  encoder.platform_formats->pix_fmt_yuv444_10bit :
                  encoder.platform_formats->pix_fmt_yuv444_8bit;
    } else if (config.chromaSamplingType == 2) {
      if (!(encoder.flags & YUV422_SUPPORT)) {
        return {};
      }
      pix_fmt = (colorspace.bit_depth == 10) ?
                  encoder.platform_formats->pix_fmt_yuv422_10bit :
                  encoder.platform_formats->pix_fmt_yuv422_8bit;
    } else {
      // YUV 4:2:0
      pix_fmt = (colorspace.bit_depth == 10) ?
                  encoder.platform_formats->pix_fmt_10bit :
                  encoder.platform_formats->pix_fmt_8bit;
    }

    {
      auto encoder_name = encoder.codec_from_config(config).name;

      BOOST_LOG(info) << "Creating encoder " << logging::bracket(encoder_name);

      const bool native_x11_10bit =
        config.capture_source == capture_source_e::x11_native10;
      auto color_coding = colorspace.colorspace == colorspace_e::rec601                                    ? "SDR (Rec. 601)" :
                          colorspace.colorspace == colorspace_e::rec709                                    ? "SDR (Rec. 709)" :
                          colorspace.colorspace == colorspace_e::bt2020sdr                                 ? "SDR (Rec. 2020)" :
                          colorspace.colorspace == colorspace_e::identity_gbr && colorspace.bit_depth == 8 ? "SDR (identity GBR, 8-bit source)" :
                          colorspace.colorspace == colorspace_e::identity_gbr && native_x11_10bit          ? "SDR (identity GBR, native 10-bit source)" :
                          colorspace.colorspace == colorspace_e::identity_gbr                              ? "SDR (identity GBR, 8-bit source/up-converted)" :
                                                                                                             "unknown";

      BOOST_LOG(info) << "Color coding: " << color_coding;
      BOOST_LOG(info) << "Color depth: " << colorspace.bit_depth << "-bit";
      BOOST_LOG(info) << "Color range: " << (colorspace.full_range ? "PC" : "TV");
    }

    if (dynamic_cast<const encoder_platform_formats_avcodec *>(encoder.platform_formats.get())) {
      result = disp.make_avcodec_software_encode_device(pix_fmt);
    } else if (dynamic_cast<const encoder_platform_formats_nvenc *>(encoder.platform_formats.get())) {
      result = disp.make_nvenc_encode_device(pix_fmt);
    }

    if (result) {
      result->colorspace = colorspace;
    }

    return result;
  }

  /**
   * @brief Capture and encode video using the asynchronous capture thread.
   *
   * @param mail Session mail bus.
   * @param config Video configuration.
   * @param channel_data Opaque channel data passed to packets.
   */
  void capture_async(
    safe::mail_t mail,
    config_t &config,
    void *channel_data
  ) {
    auto shutdown_event = mail->event<bool>(mail::shutdown);

    auto images = std::make_shared<img_event_t::element_type>();
    auto lg = util::fail_guard([&]() {
      images->stop();
      shutdown_event->raise(true);
    });

    auto ref = capture_thread_async.ref();
    if (!ref) {
      return;
    }

    ref->capture_ctx_queue->raise(capture_ctx_t {images, config});

    if (!ref->capture_ctx_queue->running()) {
      return;
    }

    int frame_nr = 1;

    auto touch_port_event = mail->event<input::touch_port_t>(mail::touch_port);

    // Encoding takes place on this thread
    platf::adjust_thread_priority(platf::thread_priority_e::high);

    while (!shutdown_event->peek() && images->running()) {
      // Wait for the main capture event when the display is being reinitialized
      if (ref->reinit_event.peek()) {
        std::this_thread::sleep_for(20ms);
        continue;
      }
      // Wait for the display to be ready
      std::shared_ptr<platf::display_t> display;
      {
        auto lg = ref->display_wp.lock();
        if (ref->display_wp->expired()) {
          continue;
        }

        display = ref->display_wp->lock();
      }

      auto &encoder = *ref->encoder_p;

      auto encode_device = make_encode_device(*display, encoder, config);
      if (!encode_device) {
        return;
      }

      // absolute mouse coordinates require that the dimensions of the screen are known
      touch_port_event->raise(make_port(display.get(), config));

      encode_run(
        frame_nr,
        mail,
        images,
        config,
        display,
        std::move(encode_device),
        ref->reinit_event,
        *ref->encoder_p,
        channel_data
      );
    }
  }

  /**
   * @brief Capture and encode video for a streaming session.
   *
   * @param mail Session mail bus.
   * @param config Video configuration.
   * @param channel_data Opaque channel data passed to packets.
   */
  void capture(
    safe::mail_t mail,
    config_t config,
    void *channel_data
  ) {
    auto idr_events = mail->event<bool>(mail::idr);

    idr_events->raise(true);
    if (!select_encoder_backend_for_session(config.encoder_backend)) {
      BOOST_LOG(error) << "Unable to activate requested PLANK encoder backend ["sv
                       << config.encoder_backend << ']';
      mail->event<bool>(mail::shutdown)->raise(true);
      return;
    }
    capture_async(std::move(mail), config, channel_data);
  }

  std::vector<platf::display_info_t> output_topology() {
    if (!chosen_encoder || !chosen_encoder->platform_formats) {
      return {};
    }
    return platf::display_infos(chosen_encoder->platform_formats->dev_type);
  }

  std::string output_topology_generation(const std::vector<platf::display_info_t> &outputs) {
    auto canonical = outputs;
    std::ranges::sort(canonical, {}, &platf::display_info_t::id);
    std::string fingerprint;
    for (const auto &output : canonical) {
      fingerprint += std::format("{}:{}:{}:{}:{}:{}:{}:{};", output.id, output.x, output.y, output.width, output.height, output.rotation, output.refresh_millihz, output.primary);
    }
    // std::hash is not required to be stable across implementations or builds.
    // Use a fixed FNV-1a fingerprint so a generation remains reproducible.
    std::uint64_t hash = 14695981039346656037ULL;
    for (const auto byte : fingerprint) {
      hash ^= static_cast<unsigned char>(byte);
      hash *= 1099511628211ULL;
    }
    return std::format("x11:{:016x}", hash);
  }

  std::optional<std::string> resolve_output_capture_name(
    const std::vector<platf::display_info_t> &outputs,
    std::string_view output_id
  ) {
    const auto output = std::find_if(outputs.begin(), outputs.end(), [output_id](const auto &candidate) {
      return candidate.id == output_id;
    });
    if (output == outputs.end()) {
      return std::nullopt;
    }
    return output->capture_name;
  }

  /**
   * @brief Enumerates supported validate flag options.
   */
  enum validate_flag_e {
    VUI_PARAMS = 0x01,  ///< VUI parameters
  };

  /**
   * @brief Validate config before it is used.
   *
   * @param disp Display connection or display handle.
   * @param encoder Encoder configuration or encoder instance.
   * @param config Configuration values to apply.
   * @return 0 when the selected encoder/device accepts the configuration; nonzero otherwise.
   */
  int validate_config(std::shared_ptr<platf::display_t> disp, const encoder_t &encoder, const config_t &config) {
    auto encode_device = make_encode_device(*disp, encoder, config);
    if (!encode_device) {
      return -1;
    }

    auto session = make_encode_session(disp.get(), encoder, config, std::move(encode_device));
    if (!session) {
      return -1;
    }

    {
      // Image buffers are large, so we use a separate scope to free it immediately after convert()
      auto img = disp->alloc_img();
      if (!img || disp->dummy_img(img.get()) || session->convert(*img)) {
        return -1;
      }
    }

    session->request_idr_frame();

    auto packets = mail::man->queue<packet_t>(mail::video_packets);
    while (!packets->peek()) {
      if (encode(1, *session, packets, nullptr, {})) {
        return -1;
      }
    }

    auto packet = packets->pop();
    if (!packet->is_idr()) {
      BOOST_LOG(error) << "First packet type is not an IDR frame"sv;

      return -1;
    }

    int flag = 0;

    // This check only applies for H.264 and HEVC
    if (config.videoFormat <= 1) {
      if (auto packet_avcodec = dynamic_cast<packet_raw_avcodec *>(packet.get())) {
        const bool exact_h264_high10_identity =
          config.videoFormat == 0 && config.dynamicRange == 1 &&
          config.chromaSamplingType == 1 &&
          (config.encoderCscMode >> 1) == COLORSPACE_IDENTITY_GBR &&
          (config.encoderCscMode & 1) != 0;
        const bool sps_valid = exact_h264_high10_identity ?
                                 cbs::validate_h264_high10_444_identity(packet_avcodec->av_packet) :
                                 cbs::validate_sps(
                                   packet_avcodec->av_packet,
                                   config.videoFormat ? AV_CODEC_ID_H265 : AV_CODEC_ID_H264
                                 );
        if (sps_valid) {
          flag |= VUI_PARAMS;
        } else if (exact_h264_high10_identity) {
          BOOST_LOG(error)
            << "H.264 encoder probe rejected a stream that was not exact High 10 4:4:4 identity GBR"sv;
        }
      } else {
        // Don't check it for non-avcodec encoders.
        flag |= VUI_PARAMS;
      }
    }

    return flag;
  }

  /**
   * @brief Validate encoder before it is used.
   */
  bool validate_encoder(encoder_t &encoder, bool expect_failure, bool test_hevc) {
    const std::string output_name;
    std::shared_ptr<platf::display_t> disp;

    BOOST_LOG(info) << "Trying encoder ["sv << encoder.name << ']';
    auto fg = util::fail_guard([&]() {
      BOOST_LOG(info) << "Encoder ["sv << encoder.name << "] failed"sv;
    });

    encoder.h264.capabilities.set();
    encoder.hevc.capabilities.set();

    // First, test encoder viability
    config_t config_max_ref_frames {"", 1920, 1080, 60, 6000, 1000, 1, 1, 1, 0, 0, 0};
    config_t config_autoselect {"", 1920, 1080, 60, 6000, 1000, 1, 0, 1, 0, 0, 0};
#if defined(__linux__) && defined(SUNSHINE_BUILD_CUDA)
    if (encoder.platform_formats->dev_type == platf::mem_type_e::cuda) {
      config_max_ref_frames.capture_source = capture_source_e::nvfbc_8bit;
      config_autoselect.capture_source = capture_source_e::nvfbc_8bit;
    }
    if (encoder.name == "software-cuda"sv) {
      config_max_ref_frames.encoderCscMode = (COLORSPACE_IDENTITY_GBR << 1) | 1;
      config_max_ref_frames.chromaSamplingType = 1;
      config_autoselect.encoderCscMode = config_max_ref_frames.encoderCscMode;
      config_autoselect.chromaSamplingType = 1;
    }
#endif

    // If the encoder isn't supported at all (not even H.264), bail early
    reset_display(disp, encoder.platform_formats->dev_type, output_name, config_autoselect);
    if (!disp) {
      return false;
    }
    if (!disp->is_codec_supported(encoder.h264.name, config_autoselect)) {
      fg.disable();
      BOOST_LOG(info) << "Encoder ["sv << encoder.name << "] is not supported on this GPU"sv;
      return false;
    }

    // If we're expecting failure, use the autoselect ref config first since that will always succeed
    // if the encoder is available.
    auto max_ref_frames_h264 = expect_failure ? -1 : validate_config(disp, encoder, config_max_ref_frames);
    auto autoselect_h264 = max_ref_frames_h264 >= 0 ? max_ref_frames_h264 : validate_config(disp, encoder, config_autoselect);
    if (autoselect_h264 < 0) {
      return false;
    } else if (expect_failure) {
      // We expected failure, but actually succeeded. Do the max_ref_frames probe we skipped.
      max_ref_frames_h264 = validate_config(disp, encoder, config_max_ref_frames);
    }

    std::vector<std::pair<validate_flag_e, encoder_t::flag_e>> packet_deficiencies {
      {VUI_PARAMS, encoder_t::VUI_PARAMETERS},
    };

    for (auto [validate_flag, encoder_flag] : packet_deficiencies) {
      encoder.h264[encoder_flag] = (max_ref_frames_h264 & validate_flag && autoselect_h264 & validate_flag);
    }

    encoder.h264[encoder_t::REF_FRAMES_RESTRICT] = max_ref_frames_h264 >= 0;
    encoder.h264[encoder_t::PASSED] = true;

    if (test_hevc) {
      config_max_ref_frames.videoFormat = 1;
      config_autoselect.videoFormat = 1;

      if (disp->is_codec_supported(encoder.hevc.name, config_autoselect)) {
        auto max_ref_frames_hevc = validate_config(disp, encoder, config_max_ref_frames);

        // If H.264 succeeded with max ref frames specified, assume that we can count on
        // HEVC to also succeed with max ref frames specified if HEVC is supported.
        auto autoselect_hevc = (max_ref_frames_hevc >= 0 || max_ref_frames_h264 >= 0) ?
                                 max_ref_frames_hevc :
                                 validate_config(disp, encoder, config_autoselect);

        for (auto [validate_flag, encoder_flag] : packet_deficiencies) {
          encoder.hevc[encoder_flag] = (max_ref_frames_hevc & validate_flag && autoselect_hevc & validate_flag);
        }

        encoder.hevc[encoder_t::REF_FRAMES_RESTRICT] = max_ref_frames_hevc >= 0;
        encoder.hevc[encoder_t::PASSED] = max_ref_frames_hevc >= 0 || autoselect_hevc >= 0;
      } else {
        BOOST_LOG(info) << "Encoder ["sv << encoder.hevc.name << "] is not supported on this GPU"sv;
        encoder.hevc.capabilities.reset();
      }
    } else {
      // Clear all cap bits for HEVC if we didn't probe it
      encoder.hevc.capabilities.reset();
    }

    // Test ten-bit and YUV444 support
    {
      auto test_yuv444 = [&](auto &flag_map, auto video_format) {
        // Reject ineligible probes before opening another capture context.
        // Supported profiles still require their real test encode below.
        if (!flag_map[encoder_t::PASSED]) {
          return;
        }
        if (!(encoder.flags & YUV444_SUPPORT)) {
          flag_map[encoder_t::YUV444] = false;
          return;
        }
        config_t config = {"", 1920, 1080, 60, 6000, 1000, 1, 0, 1, video_format, 0, 1};
        if (video_format == 0) {
          config.encoderCscMode = (COLORSPACE_IDENTITY_GBR << 1) | 1;
        }

        reset_display(disp, encoder.platform_formats->dev_type, output_name, config);
        if (!disp) {
          return;
        }

        auto encoder_codec_name = encoder.codec_from_config(config).name;

        if (disp->is_codec_supported(encoder_codec_name, config) && validate_config(disp, encoder, config) >= 0) {
          flag_map[encoder_t::YUV444] = true;
        } else {
          flag_map[encoder_t::YUV444] = false;
        }
      };

      auto test_yuv420_10bit = [&](auto &flag_map, auto video_format) {
        if (!flag_map[encoder_t::PASSED]) {
          return;
        }
        const config_t config = {"", 1920, 1080, 60, 6000, 1000, 1, 0, 3, video_format, 1, 0};

        reset_display(disp, encoder.platform_formats->dev_type, output_name, config);
        if (!disp) {
          return;
        }

        auto encoder_codec_name = encoder.codec_from_config(config).name;

        if (disp->is_codec_supported(encoder_codec_name, config) && validate_config(disp, encoder, config) >= 0) {
          flag_map[encoder_t::TEN_BIT] = true;
        } else {
          flag_map[encoder_t::TEN_BIT] = false;
        }
      };

      auto test_yuv444_10bit = [&](auto &flag_map, auto video_format) {
        if (!flag_map[encoder_t::PASSED]) {
          return;
        }
        if (!(encoder.flags & YUV444_SUPPORT)) {
          flag_map[encoder_t::TEN_BIT_YUV444] = false;
          return;
        }
        config_t config = {"", 1920, 1080, 60, 6000, 1000, 1, 0, 3, video_format, 1, 1};
        if (video_format == 0) {
          config.encoderCscMode = (COLORSPACE_IDENTITY_GBR << 1) | 1;
        }

        reset_display(disp, encoder.platform_formats->dev_type, output_name, config);
        if (!disp) {
          return;
        }

        auto encoder_codec_name = encoder.codec_from_config(config).name;

        if (disp->is_codec_supported(encoder_codec_name, config) && validate_config(disp, encoder, config) >= 0) {
          flag_map[encoder_t::TEN_BIT_YUV444] = true;
        } else {
          flag_map[encoder_t::TEN_BIT_YUV444] = false;
        }
      };

      auto test_h264_yuv422 = [&](bool ten_bit) {
        if (!encoder.h264[encoder_t::PASSED] || !(encoder.flags & YUV422_SUPPORT)) {
          return false;
        }
        config_t config = {"", 1920, 1080, 60, 6000, 1000, 1, 0, 3, 0, ten_bit ? 1 : 0, 2};
        reset_display(disp, encoder.platform_formats->dev_type, output_name, config);
        if (!disp) {
          return false;
        }

        const auto encoder_codec_name = encoder.codec_from_config(config).name;
        return disp->is_codec_supported(encoder_codec_name, config) &&
               validate_config(disp, encoder, config) >= 0;
      };

      test_yuv444(encoder.h264, 0);
      // H.264 dynamic range denotes the PLANK 10-bit 4:4:4 mode.
      // It remains SDR identity GBR rather than HDR.
      encoder.h264[encoder_t::TEN_BIT] = false;
      test_yuv444_10bit(encoder.h264, 0);
      encoder.h264[encoder_t::YUV422] = test_h264_yuv422(false);
      encoder.h264[encoder_t::TEN_BIT_YUV422] = test_h264_yuv422(true);
      encoder.hevc[encoder_t::YUV422] = false;
      encoder.hevc[encoder_t::TEN_BIT_YUV422] = false;

      test_yuv444(encoder.hevc, 1);
      test_yuv420_10bit(encoder.hevc, 1);
      test_yuv444_10bit(encoder.hevc, 1);
    }

    encoder.h264[encoder_t::VUI_PARAMETERS] = encoder.h264[encoder_t::VUI_PARAMETERS] && !config::sunshine.flags[config::flag::FORCE_VIDEO_HEADER_REPLACE];
    encoder.hevc[encoder_t::VUI_PARAMETERS] = encoder.hevc[encoder_t::VUI_PARAMETERS] && !config::sunshine.flags[config::flag::FORCE_VIDEO_HEADER_REPLACE];

    if (!encoder.h264[encoder_t::VUI_PARAMETERS]) {
      BOOST_LOG(warning) << encoder.name << ": h264 missing sps->vui parameters"sv;
    }
    if (encoder.hevc[encoder_t::PASSED] && !encoder.hevc[encoder_t::VUI_PARAMETERS]) {
      BOOST_LOG(warning) << encoder.name << ": hevc missing sps->vui parameters"sv;
    }

    fg.disable();
    return true;
  }

  int probe_encoders() {
    // If we already have a good encoder, check to see if another probe is required
    if (chosen_encoder && !(chosen_encoder->flags & ALWAYS_REPROBE) && !platf::needs_encoder_reenumeration()) {
      return 0;
    }

    // Restart encoder selection
    auto previous_encoder = chosen_encoder;
    chosen_encoder = nullptr;
    last_encoder_probe_supported_ref_frames_invalidation = false;
    last_encoder_probe_supported_h264_8bit_422 = false;
    last_encoder_probe_supported_h264_10bit_422 = false;

    BOOST_LOG(info) << "// Testing for available encoders, this may generate errors. You can safely ignore those errors. //"sv;

#if defined(__linux__) && defined(SUNSHINE_BUILD_CUDA)
    // PLANK profiles select their backend per session. Qualify both
    // exact product backends independently instead of using Sunshine's
    // inherited global encoder selector or generic fallback order.
    const bool software_cuda_qualified =
      validate_encoder(software_cuda, previous_encoder && previous_encoder != &software_cuda, false);
    nvenc_direct_qualified =
      validate_encoder(nvenc_direct, previous_encoder && previous_encoder != &nvenc_direct, true);
    if (!software_cuda_qualified && !nvenc_direct_qualified) {
      BOOST_LOG(fatal) << "No exact PLANK encoder backend is available."sv;
      return -1;
    }
    chosen_encoder = software_cuda_qualified ? &software_cuda : &nvenc_direct;
#else
    BOOST_LOG(fatal) << "The PLANK Linux Host requires its CUDA-enabled product backends."sv;
    return -1;
#endif

    if (chosen_encoder == nullptr) {
      BOOST_LOG(fatal) << "Unable to find display or encoder during startup."sv;
      if (!config::video.adapter_name.empty()) {
        BOOST_LOG(fatal) << "Please ensure your manually chosen GPU is connected and available."sv;
      } else {
        BOOST_LOG(fatal) << "Please check that a display is connected and powered on."sv;
      }
      return -1;
    }

    BOOST_LOG(info);
    BOOST_LOG(info) << "// Ignore any errors mentioned above, they are not relevant. //"sv;
    BOOST_LOG(info);

    auto &encoder = *chosen_encoder;

    last_encoder_probe_supported_ref_frames_invalidation = (encoder.flags & REF_FRAMES_INVALIDATION);
    last_encoder_probe_supported_yuv444_for_codec[0] = encoder.h264[encoder_t::PASSED] &&
                                                       encoder.h264[encoder_t::YUV444];
    last_encoder_probe_supported_h264_10bit_444 =
      encoder.h264[encoder_t::PASSED] && encoder.h264[encoder_t::TEN_BIT_YUV444];
    last_encoder_probe_supported_h264_8bit_422 =
      encoder.h264[encoder_t::PASSED] && encoder.h264[encoder_t::YUV422];
    last_encoder_probe_supported_h264_10bit_422 =
      encoder.h264[encoder_t::PASSED] && encoder.h264[encoder_t::TEN_BIT_YUV422];
    last_encoder_probe_supported_yuv444_for_codec[1] = encoder.hevc[encoder_t::PASSED] &&
                                                       encoder.hevc[encoder_t::YUV444];

    BOOST_LOG(debug) << "------  h264 ------"sv;
    for (int x = 0; x < encoder_t::MAX_FLAGS; ++x) {
      auto flag = static_cast<encoder_t::flag_e>(x);
      BOOST_LOG(debug) << encoder_t::from_flag(flag) << (encoder.h264[flag] ? ": supported"sv : ": unsupported"sv);
    }
    BOOST_LOG(debug) << "-------------------"sv;
    BOOST_LOG(info) << "Found H.264 encoder: "sv << encoder.h264.name << " ["sv << encoder.name << ']';

    if (encoder.hevc[encoder_t::PASSED]) {
      BOOST_LOG(debug) << "------  hevc ------"sv;
      for (int x = 0; x < encoder_t::MAX_FLAGS; ++x) {
        auto flag = static_cast<encoder_t::flag_e>(x);
        BOOST_LOG(debug) << encoder_t::from_flag(flag) << (encoder.hevc[flag] ? ": supported"sv : ": unsupported"sv);
      }
      BOOST_LOG(debug) << "-------------------"sv;

      BOOST_LOG(info) << "Found HEVC encoder: "sv << encoder.hevc.name << " ["sv << encoder.name << ']';
    }

#if defined(__linux__) && defined(SUNSHINE_BUILD_CUDA)
    // Advertise the union of exact per-session capabilities rather than the
    // capabilities of whichever backend was used for display enumeration.
    last_encoder_probe_supported_ref_frames_invalidation =
      (software_cuda_qualified && (software_cuda.flags & REF_FRAMES_INVALIDATION)) ||
      (nvenc_direct_qualified && (nvenc_direct.flags & REF_FRAMES_INVALIDATION));
    last_encoder_probe_supported_yuv444_for_codec[0] =
      (software_cuda.h264[encoder_t::PASSED] && software_cuda.h264[encoder_t::YUV444]) ||
      (nvenc_direct.h264[encoder_t::PASSED] && nvenc_direct.h264[encoder_t::YUV444]);
    last_encoder_probe_supported_h264_10bit_444 =
      (software_cuda.h264[encoder_t::PASSED] && software_cuda.h264[encoder_t::TEN_BIT_YUV444]) ||
      (nvenc_direct.h264[encoder_t::PASSED] && nvenc_direct.h264[encoder_t::TEN_BIT_YUV444]);
    last_encoder_probe_supported_h264_8bit_422 =
      software_cuda.h264[encoder_t::PASSED] && software_cuda.h264[encoder_t::YUV422];
    last_encoder_probe_supported_h264_10bit_422 =
      software_cuda.h264[encoder_t::PASSED] && software_cuda.h264[encoder_t::TEN_BIT_YUV422];
    last_encoder_probe_supported_yuv444_for_codec[1] =
      nvenc_direct.hevc[encoder_t::PASSED] && nvenc_direct.hevc[encoder_t::YUV444];
    BOOST_LOG(info) << "PLANK nvenc-direct modes: h264-444-8="sv
                    << nvenc_direct_supports_h264_444_8bit()
                    << " hevc-444-8="sv << nvenc_direct_supports_hevc_444_8bit()
                    << " hevc-444-10="sv << nvenc_direct_supports_hevc_444_10bit();
#endif

    return 0;
  }

  /**
   * @brief Start capture async.
   */
  int start_capture_async(capture_thread_async_ctx_t &capture_thread_ctx) {
    capture_thread_ctx.encoder_p = chosen_encoder;
    capture_thread_ctx.reinit_event.reset();

    capture_thread_ctx.capture_ctx_queue = std::make_shared<safe::queue_t<capture_ctx_t>>(30);

    capture_thread_ctx.capture_thread = std::jthread {
      captureThread,
      capture_thread_ctx.capture_ctx_queue,
      std::ref(capture_thread_ctx.display_wp),
      std::ref(capture_thread_ctx.reinit_event),
      std::ref(*capture_thread_ctx.encoder_p)
    };

    return 0;
  }

  /**
   * @brief Stop capture async processing.
   */
  void end_capture_async(capture_thread_async_ctx_t &capture_thread_ctx) {
    capture_thread_ctx.capture_ctx_queue->stop();

    capture_thread_ctx.capture_thread.join();
  }

  /**
   * @brief Map pix fmt values.
   */
  platf::pix_fmt_e map_pix_fmt(AVPixelFormat fmt) {
    switch (fmt) {
      case AV_PIX_FMT_VUYX:
        return platf::pix_fmt_e::ayuv;
      case AV_PIX_FMT_XV30:
        return platf::pix_fmt_e::y410;
      case AV_PIX_FMT_YUV420P10:
        return platf::pix_fmt_e::yuv420p10;
      case AV_PIX_FMT_YUV420P:
        return platf::pix_fmt_e::yuv420p;
      case AV_PIX_FMT_YUV422P:
        return platf::pix_fmt_e::yuv422p;
      case AV_PIX_FMT_YUV422P10:
        return platf::pix_fmt_e::yuv422p10;
      case AV_PIX_FMT_NV12:
        return platf::pix_fmt_e::nv12;
      case AV_PIX_FMT_P010:
        return platf::pix_fmt_e::p010;
      case AV_PIX_FMT_YUV444P:
        return platf::pix_fmt_e::yuv444p;
      case AV_PIX_FMT_YUV444P10:
      case AV_PIX_FMT_YUV444P16:
        return platf::pix_fmt_e::yuv444p16;
      default:
        return platf::pix_fmt_e::unknown;
    }

    return platf::pix_fmt_e::unknown;
  }

}  // namespace video
