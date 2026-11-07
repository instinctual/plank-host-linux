/**
 * @file src/platform/linux/audio.cpp
 * @brief Definitions for audio control on Linux.
 */
// standard includes
#include <bitset>
#include <sstream>

// lib includes
#include "pulse_wait.h"

#include <boost/regex.hpp>
#include <pulse/error.h>
#include <pulse/pulseaudio.h>

// local includes
#include "src/config.h"
#include "src/logging.h"
#include "src/platform/common.h"
#include "src/thread_safe.h"

namespace platf {
  using namespace std::literals;

  /**
   * @brief Position mapping.
   */
  constexpr pa_channel_position_t position_mapping[] {
    PA_CHANNEL_POSITION_FRONT_LEFT,
    PA_CHANNEL_POSITION_FRONT_RIGHT,
    PA_CHANNEL_POSITION_FRONT_CENTER,
    PA_CHANNEL_POSITION_LFE,
    PA_CHANNEL_POSITION_REAR_LEFT,
    PA_CHANNEL_POSITION_REAR_RIGHT,
    PA_CHANNEL_POSITION_SIDE_LEFT,
    PA_CHANNEL_POSITION_SIDE_RIGHT,
  };

  /**
   * @brief Convert a PulseAudio operation result to a log string.
   *
   * @param name Human-readable name to assign.
   * @param mapping Opus channel mapping table for the requested layout.
   * @param channels Number of audio channels in the stream.
   * @return Value converted to string.
   */
  std::string to_string(const char *name, const std::uint8_t *mapping, int channels) {
    std::stringstream ss;

    ss << "rate=48000 sink_name="sv << name << " format=float channels="sv << channels << " channel_map="sv;
    std::for_each_n(mapping, channels - 1, [&ss](std::uint8_t pos) {
      ss << pa_channel_position_to_string(position_mapping[pos]) << ',';
    });

    ss << pa_channel_position_to_string(position_mapping[mapping[channels - 1]]);

    ss << " sink_properties=device.description="sv << name;
    auto result = ss.str();

    BOOST_LOG(debug) << "null-sink args: "sv << result;
    return result;
  }

  /**
   * @brief PulseAudio recording stream and channel metadata.
   */
  struct mic_attr_t: public mic_t {
    util::safe_ptr<pa_mainloop, pa_mainloop_free> loop {pa_mainloop_new()};  ///< Capture-owned event loop.
    util::safe_ptr<pa_context, pa_context_unref> context;  ///< Capture connection.
    util::safe_ptr<pa_stream, pa_stream_unref> stream;  ///< Asynchronous recording stream.
    const void *chunk = nullptr;  ///< Borrowed until pa_stream_drop().
    std::size_t chunk_size = 0, chunk_offset = 0;  ///< Unconsumed capture bytes.
    std::vector<float> pending;  ///< One partial output block, retained across timeouts.
    std::size_t filled = 0;  ///< Filled bytes in pending.

    /** @brief Disconnect without waiting for an audio-server acknowledgement. */
    ~mic_attr_t() override {
      if (stream) {
        pa_stream_disconnect(stream.get());
      }
      stream.reset();
      if (context) {
        pa_context_disconnect(context.get());
      }
    }

    /** @brief Capture one block with bounded waits and explicit shutdown cancellation.
     * @param output Destination PCM block.
     * @param stop Owner cancellation predicate.
     * @return Capture outcome.
     */
    capture_e sample(std::vector<float> &output, const std::function<bool()> &stop) override {
      if (pending.size() != output.size()) {
        pending.resize(output.size());
        filled = 0;
      }
      const auto total = pending.size() * sizeof(float);
      const auto deadline = std::chrono::steady_clock::now() + 1s;
      while (filled < total) {
        if (stop()) {
          return capture_e::interrupted;
        }
        if (chunk_offset == chunk_size) {
          if (chunk_size && pa_stream_drop(stream.get()) < 0) {
            return capture_e::reinit;
          }
          chunk_size = chunk_offset = 0;
          if (!pulse::wait(loop.get(), context.get(), [&] {
                return pa_stream_get_state(stream.get()) != PA_STREAM_READY || pa_stream_readable_size(stream.get()) > 0;
              },
                           stop,
                           deadline)) {
            if (stop()) {
              return capture_e::interrupted;
            }
            return pa_context_get_state(context.get()) == PA_CONTEXT_READY ? capture_e::timeout : capture_e::reinit;
          }
          if (pa_stream_get_state(stream.get()) != PA_STREAM_READY ||
              pa_stream_peek(stream.get(), &chunk, &chunk_size) < 0) {
            return capture_e::reinit;
          }
          if (!chunk_size) {
            continue;
          }
        }
        const auto size = std::min(total - filled, chunk_size - chunk_offset);
        auto *destination = reinterpret_cast<std::uint8_t *>(pending.data()) + filled;
        if (chunk) {
          std::memcpy(destination, static_cast<const std::uint8_t *>(chunk) + chunk_offset, size);
        } else {
          std::memset(destination, 0, size);  // PulseAudio holes represent silence.
        }
        chunk_offset += size;
        filled += size;
      }
      output.swap(pending);
      filled = 0;
      return capture_e::ok;
    }
  };

  /** @brief Open a bounded asynchronous recording connection.
   * @param mapping Channel positions.
   * @param channels Channel count.
   * @param sample_rate Sample rate in Hz.
   * @param frame_size Frames per capture block.
   * @param source_name Monitor source to capture.
   * @return Initialized capture or null on failure/deadline.
   */
  std::unique_ptr<mic_t> microphone(const std::uint8_t *mapping, int channels, std::uint32_t sample_rate, std::uint32_t frame_size, std::string source_name) {
    auto mic = std::make_unique<mic_attr_t>();
    if (!mic->loop) {
      return nullptr;
    }
    mic->context.reset(pa_context_new(pa_mainloop_get_api(mic->loop.get()), "PLANK Host"));
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    if (!mic->context || pa_context_connect(mic->context.get(), nullptr, PA_CONTEXT_NOFLAGS, nullptr) < 0 ||
        !pulse::wait(mic->loop.get(), mic->context.get(), [&] {
          return pa_context_get_state(mic->context.get()) == PA_CONTEXT_READY;
        },
                     [] {
                       return false;
                     },
                     deadline)) {
      return nullptr;
    }
    pa_sample_spec spec {PA_SAMPLE_FLOAT32, sample_rate, static_cast<std::uint8_t>(channels)};
    pa_channel_map map {};
    map.channels = channels;
    for (int i = 0; i < channels; ++i) {
      map.map[i] = position_mapping[mapping[i]];
    }
    pa_buffer_attr attr {uint32_t(-1), uint32_t(-1), uint32_t(-1), uint32_t(-1), frame_size * channels * uint32_t(sizeof(float))};
    mic->stream.reset(pa_stream_new(mic->context.get(), "PLANK recording", &spec, &map));
    if (!mic->stream || pa_stream_connect_record(mic->stream.get(), source_name.c_str(), &attr, PA_STREAM_ADJUST_LATENCY) < 0 ||
        !pulse::wait(mic->loop.get(), mic->context.get(), [&] {
          return pa_stream_get_state(mic->stream.get()) != PA_STREAM_CREATING;
        },
                     [] {
                       return false;
                     },
                     deadline) ||
        pa_stream_get_state(mic->stream.get()) != PA_STREAM_READY) {
      return nullptr;
    }
    return mic;
  }

  namespace pa {
    template<bool B, class T>
    struct add_const_helper;

    /**
     * @brief Template helper that preserves constness for const inputs.
     */
    template<class T>
    struct add_const_helper<true, T> {
      /**
       * @brief PulseAudio object type passed to the safe pointer wrapper.
       */
      using type = const std::remove_pointer_t<T> *;
    };

    /**
     * @brief Template helper that leaves non-const inputs mutable.
     */
    template<class T>
    struct add_const_helper<false, T> {
      /**
       * @brief PulseAudio object type passed to the safe pointer wrapper.
       */
      using type = const T *;
    };

    /**
     * @brief PulseAudio callback info type with pointer constness normalized.
     */
    template<class T>
    using add_const_t = typename add_const_helper<std::is_pointer_v<T>, T>::type;

    /**
     * @brief Release memory allocated by PulseAudio.
     *
     * @param p Pointer allocated by PulseAudio and released with `pa_xfree`.
     */
    template<class T>
    void pa_free(T *p) {
      pa_xfree(p);
    }

    /**
     * @brief Owning pointer for a PulseAudio context.
     */
    using ctx_t = util::safe_ptr<pa_context, pa_context_unref>;
    /**
     * @brief Owning pointer for a PulseAudio mainloop.
     */
    using loop_t = util::safe_ptr<pa_mainloop, pa_mainloop_free>;
    /**
     * @brief Owning pointer for a PulseAudio asynchronous operation.
     */
    using op_t = util::safe_ptr<pa_operation, pa_operation_unref>;
    /**
     * @brief Owning pointer for PulseAudio strings allocated with `pa_xmalloc`.
     */
    using string_t = util::safe_ptr<char, pa_free<char>>;

    /**
     * @brief Callback wrapper for PulseAudio introspection results without an end marker.
     */
    template<class T>
    using cb_simple_t = std::function<void(ctx_t::pointer, add_const_t<T> i)>;

    /**
     * @brief Handle PulseAudio sink-input introspection results.
     *
     * @param ctx Native context object used by the operation or callback.
     * @param i PulseAudio introspection info supplied to the callback.
     * @param userdata Caller-provided pointer passed through the callback.
     */
    template<class T>
    void cb(ctx_t::pointer ctx, add_const_t<T> i, void *userdata) {
      auto &f = *(cb_simple_t<T> *) userdata;

      // Cannot similarly filter on eol here. Unless reported otherwise assume
      // we have no need for special filtering like cb?
      f(ctx, i);
    }

    /**
     * @brief Callback wrapper for PulseAudio introspection results with an end marker.
     */
    template<class T>
    using cb_t = std::function<void(ctx_t::pointer, add_const_t<T> i, int eol)>;

    /**
     * @brief Handle PulseAudio source introspection results.
     *
     * @param ctx Native context object used by the operation or callback.
     * @param i PulseAudio introspection info supplied to the callback.
     * @param eol PulseAudio end-of-list marker.
     * @param userdata Caller-provided pointer passed through the callback.
     */
    template<class T>
    void cb(ctx_t::pointer ctx, add_const_t<T> i, int eol, void *userdata) {
      auto &f = *(cb_t<T> *) userdata;

      // For some reason, pulseaudio calls this callback after disconnecting
      if (i && eol) {
        return;
      }

      f(ctx, i, eol);
    }

    /**
     * @brief Forward a PulseAudio integer callback value into a Sunshine alarm.
     *
     * @param ctx PulseAudio context that emitted the callback.
     * @param i Integer value returned by the PulseAudio operation.
     * @param userdata Caller-provided pointer passed through the callback.
     */
    void cb_i(ctx_t::pointer ctx, std::uint32_t i, void *userdata) {
      auto alarm = (safe::alarm_raw_t<int> *) userdata;

      alarm->ring(i);
    }

    /**
     * @brief Record completion of a PulseAudio asynchronous operation.
     *
     * @param ctx Native context object used by the operation or callback.
     * @param status Native status code returned by the platform API.
     * @param userdata Caller-provided pointer passed through the callback.
     */
    void success_cb(ctx_t::pointer ctx, int status, void *userdata) {
      assert(userdata != nullptr);

      auto alarm = (safe::alarm_raw_t<int> *) userdata;
      alarm->ring(status ? 0 : 1);
    }

    /**
     * @brief PulseAudio server controller that creates and removes Sunshine sinks.
     */
    class server_t: public audio_control_t {
    public:
      loop_t loop;  ///< PulseAudio caller-owned mainloop instance.
      ctx_t ctx;  ///< PulseAudio caller-owned mainloop context.
      std::string requested_sink;  ///< Requested sink.

      struct {
        std::uint32_t stereo = PA_INVALID_INDEX;
        std::uint32_t surround51 = PA_INVALID_INDEX;
        std::uint32_t surround71 = PA_INVALID_INDEX;
      } index;  ///< PulseAudio module indexes for Sunshine-created null sinks.

      std::recursive_mutex operation_mutex;  ///< Serialize all context API calls and callbacks.
      std::chrono::steady_clock::time_point shutdown_deadline = std::chrono::steady_clock::time_point::max();  ///< Shared cleanup budget.

      /** @brief Initialize a context with a two-second deadline. @return Zero on success. */
      int init() {
        loop.reset(pa_mainloop_new());
        if (!loop) {
          return -1;
        }
        ctx.reset(pa_context_new(pa_mainloop_get_api(loop.get()), "PLANK Host"));
        if (!ctx || pa_context_connect(ctx.get(), nullptr, PA_CONTEXT_NOFLAGS, nullptr) < 0) {
          return -1;
        }
        return pulse::wait(loop.get(), ctx.get(), [&] {
          return pa_context_get_state(ctx.get()) == PA_CONTEXT_READY;
        },
                           [] {
                             return false;
                           },
                           std::chrono::steady_clock::now() + 2s) ?
                 0 :
                 -1;
      }

      /** @brief Complete/cancel an operation before its callback storage retires.
       * @param op Owned PulseAudio operation; null means immediate API failure.
       * @param alarm Callback result.
       * @return True if the operation produced a result before its deadline.
       */
      template<class Alarm>
      bool await(op_t &op, const Alarm &alarm) {
        if (!op) {
          return false;
        }
        const bool done = pulse::wait(loop.get(), ctx.get(), [&] {
          return pa_operation_get_state(op.get()) != PA_OPERATION_RUNNING;
        },
                                      [] {
                                        return false;
                                      },
                                      std::min(shutdown_deadline, std::chrono::steady_clock::now() + 2s));
        if (!done || !alarm->status()) {
          pa_operation_cancel(op.get());
          return false;
        }
        return true;
      }

      /**
       * @brief Create a PulseAudio null sink for one channel layout.
       *
       * @param name Human-readable name to assign.
       * @param channel_mapping Channel mapping.
       * @param channels Number of audio channels in the stream.
       * @return PulseAudio module index for the new sink, or PA_INVALID_INDEX on failure.
       */
      int load_null(const char *name, const std::uint8_t *channel_mapping, int channels) {
        std::lock_guard lock {operation_mutex};
        auto alarm = safe::make_alarm<int>();

        op_t op {
          pa_context_load_module(
            ctx.get(),
            "module-null-sink",
            to_string(name, channel_mapping, channels).c_str(),
            cb_i,
            alarm.get()
          ),
        };

        if (!await(op, alarm)) {
          return PA_INVALID_INDEX;
        }
        return *alarm->status();
      }

      /**
       * @brief Unload a Sunshine-created PulseAudio null sink.
       *
       * @param i PulseAudio introspection info supplied to the callback.
       * @return 0 when the sink is absent or unloaded; nonzero on PulseAudio failure.
       */
      int unload_null(std::uint32_t i) {
        std::lock_guard lock {operation_mutex};
        if (i == PA_INVALID_INDEX) {
          return 0;
        }

        auto alarm = safe::make_alarm<int>();

        op_t op {
          pa_context_unload_module(ctx.get(), i, success_cb, alarm.get())
        };

        if (!await(op, alarm)) {
          return -1;
        }

        if (*alarm->status()) {
          BOOST_LOG(error) << "Couldn't unload null-sink with index ["sv << i << "]: "sv << pa_strerror(pa_context_errno(ctx.get()));
          return -1;
        }

        return 0;
      }

      /**
       * @brief Query host and virtual sink names available to Sunshine.
       *
       * @return Host and virtual sink names when the backend can report them.
       */
      std::optional<sink_t> sink_info() override {
        std::lock_guard lock {operation_mutex};
        constexpr auto stereo = "sink-plank-stereo";
        constexpr auto surround51 = "sink-plank-surround51";
        constexpr auto surround71 = "sink-plank-surround71";

        auto alarm = safe::make_alarm<int>();

        sink_t sink;

        // Count of all virtual sinks that are created by us
        int nullcount = 0;

        cb_t<pa_sink_info *> f = [&](ctx_t::pointer ctx, const pa_sink_info *sink_info, int eol) {
          if (!sink_info) {
            if (eol <= 0) {
              BOOST_LOG(error) << "Couldn't get pulseaudio sink info: "sv << pa_strerror(pa_context_errno(ctx));

              alarm->ring(-1);
            } else {
              alarm->ring(0);
            }
            return;
          }

          // Ensure Sunshine won't create a sink that already exists.
          if (!std::strcmp(sink_info->name, stereo)) {
            index.stereo = sink_info->owner_module;

            ++nullcount;
          } else if (!std::strcmp(sink_info->name, surround51)) {
            index.surround51 = sink_info->owner_module;

            ++nullcount;
          } else if (!std::strcmp(sink_info->name, surround71)) {
            index.surround71 = sink_info->owner_module;

            ++nullcount;
          }
        };

        op_t op {pa_context_get_sink_info_list(ctx.get(), cb<pa_sink_info *>, &f)};

        if (!op) {
          BOOST_LOG(error) << "Couldn't create card info operation: "sv << pa_strerror(pa_context_errno(ctx.get()));

          return std::nullopt;
        }

        if (!await(op, alarm)) {
          return std::nullopt;
        }

        if (*alarm->status()) {
          return std::nullopt;
        }

        auto sink_name = get_default_sink_name();
        sink.host = sink_name;

        if (index.stereo == PA_INVALID_INDEX) {
          index.stereo = load_null(stereo, speaker::map_stereo.data(), static_cast<int>(speaker::map_stereo.size()));
          if (index.stereo == PA_INVALID_INDEX) {
            BOOST_LOG(warning) << "Couldn't create virtual sink for stereo: "sv << pa_strerror(pa_context_errno(ctx.get()));
          } else {
            ++nullcount;
          }
        }

        if (index.surround51 == PA_INVALID_INDEX) {
          index.surround51 = load_null(surround51, speaker::map_surround51.data(), static_cast<int>(speaker::map_surround51.size()));
          if (index.surround51 == PA_INVALID_INDEX) {
            BOOST_LOG(warning) << "Couldn't create virtual sink for surround-51: "sv << pa_strerror(pa_context_errno(ctx.get()));
          } else {
            ++nullcount;
          }
        }

        if (index.surround71 == PA_INVALID_INDEX) {
          index.surround71 = load_null(surround71, speaker::map_surround71.data(), static_cast<int>(speaker::map_surround71.size()));
          if (index.surround71 == PA_INVALID_INDEX) {
            BOOST_LOG(warning) << "Couldn't create virtual sink for surround-71: "sv << pa_strerror(pa_context_errno(ctx.get()));
          } else {
            ++nullcount;
          }
        }

        if (sink_name.empty()) {
          BOOST_LOG(warning) << "Couldn't find an active default sink. Continuing with virtual audio only."sv;
        }

        if (nullcount == 3) {
          sink.null = std::make_optional(sink_t::null_t {stereo, surround51, surround71});
        }

        return std::make_optional(std::move(sink));
      }

      /**
       * @brief Get default sink name.
       *
       * @return PulseAudio name of the current default sink, or an empty string.
       */
      std::string get_default_sink_name() {
        std::lock_guard lock {operation_mutex};
        std::string sink_name;
        auto alarm = safe::make_alarm<int>();

        cb_simple_t<pa_server_info *> server_f = [&](ctx_t::pointer ctx, const pa_server_info *server_info) {
          if (!server_info) {
            BOOST_LOG(error) << "Couldn't get pulseaudio server info: "sv << pa_strerror(pa_context_errno(ctx));
            alarm->ring(-1);
            return;
          }

          if (server_info->default_sink_name) {
            sink_name = server_info->default_sink_name;
          }
          alarm->ring(0);
        };

        op_t server_op {pa_context_get_server_info(ctx.get(), cb<pa_server_info *>, &server_f)};
        if (!await(server_op, alarm)) {
          return {};
        }
        // No need to check status. If it failed just return default name.
        return sink_name;
      }

      /**
       * @brief Get monitor name.
       *
       * @param sink_name Sink name.
       * @return PulseAudio monitor source name for the supplied sink, or an empty string.
       */
      std::string get_monitor_name(const std::string &sink_name) {
        std::lock_guard lock {operation_mutex};
        std::string monitor_name;
        auto alarm = safe::make_alarm<int>();

        if (sink_name.empty()) {
          return monitor_name;
        }

        cb_t<pa_sink_info *> sink_f = [&](ctx_t::pointer ctx, const pa_sink_info *sink_info, int eol) {
          if (!sink_info) {
            if (eol <= 0) {
              BOOST_LOG(error) << "Couldn't get pulseaudio sink info for ["sv << sink_name
                               << "]: "sv << pa_strerror(pa_context_errno(ctx));
              alarm->ring(-1);
            } else {
              alarm->ring(0);
            }
            return;
          }

          monitor_name = sink_info->monitor_source_name;
        };

        op_t sink_op {pa_context_get_sink_info_by_name(ctx.get(), sink_name.c_str(), cb<pa_sink_info *>, &sink_f)};

        if (!await(sink_op, alarm)) {
          return {};
        }
        // No need to check status. If it failed just return default name.
        BOOST_LOG(info) << "Found default monitor by name: "sv << monitor_name;
        return monitor_name;
      }

      /**
       * @brief Create a microphone capture stream for the requested layout.
       *
       * @param mapping Opus channel mapping table for the requested layout.
       * @param channels Number of audio channels in the stream.
       * @param sample_rate Audio sample rate in hertz.
       * @param frame_size Number of samples captured per audio frame.
       * @param continuous_audio Continuous audio.
       * @param host_audio_enabled Whether host playback should remain enabled during capture.
       * @return Microphone capture object for the requested audio layout.
       */
      std::unique_ptr<mic_t> microphone(const std::uint8_t *mapping, int channels, std::uint32_t sample_rate, std::uint32_t frame_size, bool continuous_audio, [[maybe_unused]] bool host_audio_enabled) override {
        std::lock_guard lock {operation_mutex};
        // Sink choice priority:
        // 1. Config sink
        // 2. Last sink swapped to (Usually virtual in this case)
        // 3. Default Sink
        // An attempt was made to always use default to match the switching mechanic,
        // but this happens right after the swap so the default returned by PA was not
        // the new one just set!
        auto sink_name = config::audio.sink;
        if (sink_name.empty()) {
          sink_name = requested_sink;
        }
        if (sink_name.empty()) {
          sink_name = get_default_sink_name();
        }

        return ::platf::microphone(mapping, channels, sample_rate, frame_size, get_monitor_name(sink_name));
      }

      bool is_sink_available(const std::string &sink) override {
        return !get_monitor_name(sink).empty();
      }

      /**
       * @brief Update the sink value on the backend.
       *
       * @param sink Audio sink name to route or capture.
       * @return Status from updating sink.
       */
      int set_sink(const std::string &sink) override {
        std::lock_guard lock {operation_mutex};
        auto alarm = safe::make_alarm<int>();

        BOOST_LOG(info) << "Setting default sink to: ["sv << sink << "]"sv;
        op_t op {
          pa_context_set_default_sink(
            ctx.get(),
            sink.c_str(),
            success_cb,
            alarm.get()
          ),
        };

        if (!op) {
          BOOST_LOG(error) << "Couldn't create set default-sink operation: "sv << pa_strerror(pa_context_errno(ctx.get()));
          return -1;
        }

        if (!await(op, alarm)) {
          return -1;
        }
        if (*alarm->status()) {
          BOOST_LOG(error) << "Couldn't set default-sink ["sv << sink << "]: "sv << pa_strerror(pa_context_errno(ctx.get()));

          return -1;
        }

        requested_sink = sink;

        return 0;
      }

      ~server_t() override {
        std::lock_guard lock {operation_mutex};
        shutdown_deadline = std::chrono::steady_clock::now() + 500ms;
        if (ctx && pa_context_get_state(ctx.get()) == PA_CONTEXT_READY) {
          unload_null(index.stereo);
          unload_null(index.surround51);
          unload_null(index.surround71);
        }
        if (ctx) {
          pa_context_disconnect(ctx.get());
        }
      }
    };
  }  // namespace pa

  /**
   * @brief Create the platform audio controller.
   */
  std::unique_ptr<audio_control_t> audio_control() {
    auto audio = std::make_unique<pa::server_t>();

    if (audio->init()) {
      return nullptr;
    }

    return audio;
  }
}  // namespace platf
