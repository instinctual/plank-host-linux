/**
 * @file src/platform/linux/cuda.cpp
 * @brief Definitions for CUDA encoding.
 */
// standard includes
#include <atomic>
#include <bitset>
#include <charconv>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <dlfcn.h>
#include <fcntl.h>
#include <mutex>
#include <thread>

// lib includes
#include <ffnvcodec/dynlink_loader.h>
#include <NvFBC.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
}

// local includes
#include "cuda.h"
#include "cuda_host_registration.h"
#include "src/config.h"
#include "src/frame_timing.h"
#include "src/logging.h"
#include "src/nvenc/nvenc_cuda_factory.h"
#include "src/utility.h"
#include "src/video.h"

/**
 * @def SUNSHINE_STRINGVIEW_HELPER(x)
 * @brief Macro for SUNSHINE STRINGVIEW HELPER.
 */
#define SUNSHINE_STRINGVIEW_HELPER(x) x##sv
/**
 * @def SUNSHINE_STRINGVIEW(x)
 * @brief Macro for SUNSHINE STRINGVIEW.
 */
#define SUNSHINE_STRINGVIEW(x) SUNSHINE_STRINGVIEW_HELPER(x)

/**
 * @def CU_CHECK(x, y)
 * @brief Macro for CU CHECK.
 */
#define CU_CHECK(x, y) \
  if (check((x), SUNSHINE_STRINGVIEW(y ": "))) \
  return -1

/**
 * @def CU_CHECK_IGNORE(x, y)
 * @brief Macro for CU CHECK IGNORE.
 */
#define CU_CHECK_IGNORE(x, y) \
  check((x), SUNSHINE_STRINGVIEW(y ": "))

using namespace std::literals;

namespace cuda {
  constexpr auto cudaDevAttrMaxThreadsPerBlock = (CUdevice_attribute) 1;  ///< CUDA dev attr max threads per block.
  constexpr auto cudaDevAttrMaxThreadsPerMultiProcessor = (CUdevice_attribute) 39;  ///< CUDA dev attr max threads per multi processor.

  /**
   * @brief Convert a CUDA result code into Sunshine's capture status.
   *
   * @param sv String view containing the text to inspect.
   * @param name Human-readable name to assign.
   * @param description Human-readable description used in log output.
   */
  void pass_error(const std::string_view &sv, const char *name, const char *description) {
    BOOST_LOG(error) << sv << name << ':' << description;
  }

  /**
   * @brief Release a Core Foundation object when the wrapper is destroyed.
   *
   * @param cf Core Foundation object passed to the scoped releaser.
   */
  void cff(CudaFunctions *cf) {
    cuda_free_functions(&cf);
  }

  /**
   * @brief Handle to a CUDA dynamic-library function table.
   */
  using cdf_t = util::safe_ptr<CudaFunctions, cff>;

  static cdf_t cdf;

  inline static int check(CUresult result, const std::string_view &sv) {
    if (result != CUDA_SUCCESS) {
      const char *name;
      const char *description;

      cdf->cuGetErrorName(result, &name);
      cdf->cuGetErrorString(result, &description);

      BOOST_LOG(error) << sv << name << ':' << description;
      return -1;
    }

    return 0;
  }

  namespace {
    constexpr std::size_t native10_pin_budget = 256U * 1024U * 1024U;
    std::atomic<std::size_t> native10_pinned_bytes {0};

    /** @brief Keep driver symbols alive until the SHM registration is released. */
    class host_registration_t final: public platf::deinit_t {
    public:
      void *library = nullptr;  ///< Driver DSO retained with the registration.
      std::unique_ptr<host_memory_registration_t> memory;

      ~host_registration_t() override {
        memory.reset();
        if (library) {
          dlclose(library);
        }
      }
    };
  }  // namespace

  std::unique_ptr<platf::deinit_t> register_host_memory(void *data, std::size_t size) {
    if (!cdf || !data || !size || size > native10_pin_budget) {
      return nullptr;
    }
    auto owner = std::make_unique<host_registration_t>();
    owner->library = dlopen("libcuda.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!owner->library) {
      return nullptr;
    }
    host_registration_api_t api {
      cdf->cuDeviceGet,
      cdf->cuDevicePrimaryCtxRetain,
      cdf->cuDevicePrimaryCtxRelease,
      cdf->cuCtxPushCurrent,
      cdf->cuCtxPopCurrent,
      nullptr,
      nullptr,
      [](CUresult result, const char *message) {
        check(result, message);
      }
    };
    api.register_memory = reinterpret_cast<decltype(api.register_memory)>(
      dlsym(owner->library, "cuMemHostRegister_v2")
    );
    api.unregister_memory = reinterpret_cast<decltype(api.unregister_memory)>(
      dlsym(owner->library, "cuMemHostUnregister")
    );
    if (!api.register_memory || !api.unregister_memory) {
      return nullptr;
    }
    owner->memory = host_memory_registration_t::make(api, data, size, native10_pinned_bytes, native10_pin_budget);
    if (!owner->memory) {
      return nullptr;
    }
    return owner;
  }

  /**
   * @brief Release stream resources.
   *
   * @param stream CUDA stream or PipeWire stream involved in the operation.
   */
  void freeStream(CUstream stream) {
    CU_CHECK_IGNORE(cdf->cuStreamDestroy(stream), "Couldn't destroy cuda stream");
  }

  /**
   * @brief CUDA image wrapper that owns mapped graphics resources for one frame.
   */
  class img_t: public platf::img_t {
  public:
    tex_t tex;  ///< CUDA texture object used as the conversion source.
  };

  /**
   * @brief Map CUDA graphics resources for use as an image.
   *
   * @return 0 on success; nonzero or negative platform status on failure.
   */
  int init() {
    auto status = cuda_load_functions(&cdf, nullptr);
    if (status) {
      BOOST_LOG(error) << "Couldn't load cuda: "sv << status;

      return -1;
    }

    CU_CHECK(cdf->cuInit(0), "Couldn't initialize cuda");

    return 0;
  }

  /**
   * @brief NvFBC CUDA readback device for latency-bounded x264 encoding.
   *
   * The qualified 8-bit and 10-bit x264 paths first create compact planar
   * identity GBR on CUDA and read those planes into pinned memory. The 8-bit
   * frame uses that readback directly; the 10-bit frame expands it to full-range
   * 10-bit samples on CPU. This avoids transferring the 49.8 MB 16-bit carrier
   * produced by direct CUDA 10-bit conversion at 4K.
   */
  class cuda_software_t final: public platf::avcodec_encode_device_t {
  public:
    /**
     * @brief Release CUDA resources and stop depth-expansion workers.
     */
    ~cuda_software_t() override {
      video::log_frame_timing("cuda_dispatch", dispatch_us);
      video::log_frame_timing("gpu_readback_sync", readback_sync_us);
      video::log_frame_timing("cpu_depth_expansion", depth_expansion_us);
      video::log_frame_timing("cuda_software_convert_total", total_conversion_us);

      for (auto &worker : workers) {
        worker.request_stop();
      }
      work_ready.notify_all();
      workers.clear();

      if (device_compact && cdf) {
        CU_CHECK_IGNORE(cdf->cuMemFree(device_compact), "Couldn't free compact identity planes");
      }
      if (host_readback && host_free) {
        CU_CHECK_IGNORE(host_free(host_readback), "Couldn't free pinned software-encoder readback");
      }
      if (cuda_driver) {
        dlclose(cuda_driver);
      }
    }

    /**
     * @brief Initialize capture dimensions and CUDA stream state.
     *
     * @param in_width Captured frame width.
     * @param in_height Captured frame height.
     * @param requested_format Software format selected during encoder negotiation.
     * @return 0 on success; -1 on invalid state.
     */
    int init(int in_width, int in_height, platf::pix_fmt_e requested_format) {
      if (!cdf || (requested_format != platf::pix_fmt_e::yuv444p &&
                   requested_format != platf::pix_fmt_e::yuv444p16 &&
                   requested_format != platf::pix_fmt_e::yuv422p &&
                   requested_format != platf::pix_fmt_e::yuv422p10)) {
        return -1;
      }

      width = in_width;
      height = in_height;
      capture_format = requested_format;
      stream = make_stream();
      cuda_driver = dlopen("libcuda.so.1", RTLD_LAZY | RTLD_LOCAL);
      if (cuda_driver) {
        host_alloc = reinterpret_cast<host_alloc_fn_t>(dlsym(cuda_driver, "cuMemHostAlloc"));
        host_free = reinterpret_cast<host_free_fn_t>(dlsym(cuda_driver, "cuMemFreeHost"));
      }
      if (!stream || !host_alloc || !host_free) {
        BOOST_LOG(error) << "CUDA driver does not expose pinned-host allocation functions"sv;
        return -1;
      }
      return 0;
    }

    /**
     * @brief Attach the CPU frame that will be submitted to x264.
     *
     * @param new_frame FFmpeg software frame.
     * @return 0 on success; -1 when the negotiated format is unsupported.
     */
    int set_frame(AVFrame *new_frame) override {
      host_frame.reset(new_frame);
      frame = new_frame;
      if (!frame) {
        return -1;
      }

      const bool identity_8bit = frame->format == AV_PIX_FMT_YUV444P;
      const bool identity_10bit = frame->format == AV_PIX_FMT_YUV444P10LE;
      const bool yuv422_8bit = frame->format == AV_PIX_FMT_YUV422P;
      const bool yuv422_10bit = frame->format == AV_PIX_FMT_YUV422P10LE;
      if ((!identity_8bit && !identity_10bit && !yuv422_8bit && !yuv422_10bit) ||
          (identity_8bit && capture_format != platf::pix_fmt_e::yuv444p) ||
          (identity_10bit && capture_format != platf::pix_fmt_e::yuv444p16) ||
          (yuv422_8bit && capture_format != platf::pix_fmt_e::yuv422p) ||
          (yuv422_10bit && capture_format != platf::pix_fmt_e::yuv422p10)) {
        BOOST_LOG(error) << "CUDA software encode received an unsupported frame/capture format pair"sv;
        return -1;
      }

      output_width = frame->width;
      output_height = frame->height;
      const auto scalar = std::fminf(
        output_width / static_cast<float>(width),
        output_height / static_cast<float>(height)
      );
      content_width = std::max(1, static_cast<int>(width * scalar));
      content_height = std::max(1, static_cast<int>(height * scalar));
      content_offset_x = (output_width - content_width) / 2;
      content_offset_y = (output_height - content_height) / 2;
      BOOST_LOG(info)
        << "PLANK software capture geometry: source=" << width << 'x' << height
        << " content=" << content_width << 'x' << content_height
        << '+' << content_offset_x << '+' << content_offset_y
        << " encode=" << output_width << 'x' << output_height;
      const auto plane_size = static_cast<std::size_t>(output_width) * output_height;
      host_readback_size = yuv422_8bit  ? plane_size * 2U :
                           yuv422_10bit ? plane_size * 4U :
                                          plane_size * 3U;
      CU_CHECK(host_alloc(&host_readback, host_readback_size, 1U), "Couldn't allocate pinned software-encoder readback");

      if (yuv422_8bit || yuv422_10bit) {
        const std::size_t bytes_per_sample = yuv422_10bit ? 2U : 1U;
        const auto luma_size = plane_size * bytes_per_sample;
        const auto chroma_size = (plane_size / 2U) * bytes_per_sample;
        auto *base = static_cast<std::uint8_t *>(host_readback);
        std::memset(base, 0, luma_size);
        if (yuv422_10bit) {
          auto *u = reinterpret_cast<std::uint16_t *>(base + luma_size);
          auto *v = reinterpret_cast<std::uint16_t *>(base + luma_size + chroma_size);
          std::fill_n(u, plane_size / 2U, static_cast<std::uint16_t>(512));
          std::fill_n(v, plane_size / 2U, static_cast<std::uint16_t>(512));
        } else {
          std::memset(base + luma_size, 128, chroma_size * 2U);
        }

        frame->data[0] = base;
        frame->data[1] = base + luma_size;
        frame->data[2] = base + luma_size + chroma_size;
        frame->linesize[0] = output_width * bytes_per_sample;
        frame->linesize[1] = frame->linesize[2] = (output_width / 2) * bytes_per_sample;

        CU_CHECK(cdf->cuMemAlloc(&device_compact, host_readback_size), "Couldn't allocate CUDA YUV422 planes");
        CU_CHECK(cdf->cuMemcpyHtoDAsync(device_compact, host_readback, host_readback_size, stream.get()), "Couldn't initialize CUDA YUV422 background");
        auto converter = sws_t::make(width, height, output_width, output_height, width * 4);
        if (!converter) {
          return -1;
        }
        sws = std::move(*converter);
        mode = yuv422_10bit ? mode_e::yuv422_10bit : mode_e::yuv422_8bit;
        return 0;
      }

      if (identity_8bit) {
        auto *base = static_cast<std::uint8_t *>(host_readback);
        frame->data[0] = base;
        frame->data[1] = base + plane_size;
        frame->data[2] = base + plane_size * 2U;
        frame->linesize[0] = frame->linesize[1] = frame->linesize[2] = output_width;
      } else {
        if (av_frame_get_buffer(frame, 64) < 0) {
          BOOST_LOG(error) << "Couldn't allocate x264 10-bit software frame"sv;
          return -1;
        }
      }
      CU_CHECK(cdf->cuMemAlloc(&device_compact, host_readback_size), "Couldn't allocate compact CUDA identity planes");
      CU_CHECK(cdf->cuMemsetD8Async(device_compact, 0, host_readback_size, stream.get()), "Couldn't initialize identity span background");

      auto converter = sws_t::make(width, height, output_width, output_height, width * 4);
      if (!converter) {
        return -1;
      }
      sws = std::move(*converter);
      mode = identity_8bit ? mode_e::identity_8bit : mode_e::identity_10bit;
      BOOST_LOG(info)
        << "PLANK software identity input: planar GBR, codec depth="
        << (identity_8bit ? 8 : 10)
        << "-bit, CUDA readback depth=8-bit";
      if (mode == mode_e::identity_10bit) {
        start_workers();
      }
      return 0;
    }

    /**
     * @brief Apply identity GBR coefficients to the CUDA converter.
     */
    void apply_colorspace() override {
      if (mode == mode_e::identity_8bit ||
          mode == mode_e::identity_10bit ||
          mode == mode_e::yuv422_8bit ||
          mode == mode_e::yuv422_10bit) {
        sws.apply_colorspace(colorspace);
      }
    }

    /**
     * @brief Convert and read back one NvFBC CUDA image.
     *
     * @param img Captured NvFBC CUDA image.
     * @return 0 on success; -1 on CUDA or conversion failure.
     */
    int convert(platf::img_t &img) override {
      const auto conversion_start = std::chrono::steady_clock::now();
      auto &texture = static_cast<cuda::img_t &>(img).tex;
      if (mode == mode_e::identity_8bit || mode == mode_e::identity_10bit) {
        auto *base = reinterpret_cast<std::uint8_t *>(static_cast<std::uintptr_t>(device_compact));
        const auto plane_size = static_cast<std::size_t>(output_width) * output_height;
        const auto texture_object =
          (width != output_width || height != output_height) ? texture.texture.linear : texture.texture.point;
        if (sws.convert_yuv444(base, base + plane_size, base + plane_size * 2U, output_width, texture_object, stream.get())) {
          return -1;
        }
        CU_CHECK(cdf->cuMemcpyDtoHAsync(host_readback, device_compact, host_readback_size, stream.get()), "Couldn't read back compact identity planes");
      } else if (mode == mode_e::yuv422_8bit || mode == mode_e::yuv422_10bit) {
        auto *base = reinterpret_cast<std::uint8_t *>(static_cast<std::uintptr_t>(device_compact));
        const auto plane_size = static_cast<std::size_t>(output_width) * output_height;
        const auto bytes_per_sample = mode == mode_e::yuv422_10bit ? 2U : 1U;
        const auto luma_size = plane_size * bytes_per_sample;
        const auto chroma_size = (plane_size / 2U) * bytes_per_sample;
        const auto texture_object =
          (width != output_width || height != output_height) ? texture.texture.linear : texture.texture.point;
        const auto conversion_status = mode == mode_e::yuv422_10bit ?
                                         sws.convert_yuv422_10bit(base, base + luma_size, base + luma_size + chroma_size, output_width * 2U, output_width, texture_object, stream.get()) :
                                         sws.convert_yuv422(base, base + luma_size, base + luma_size + chroma_size, output_width, output_width / 2U, texture_object, stream.get());
        if (conversion_status) {
          return -1;
        }
        CU_CHECK(cdf->cuMemcpyDtoHAsync(host_readback, device_compact, host_readback_size, stream.get()), "Couldn't read back YUV422 planes");
      } else {
        return -1;
      }

      const auto synchronization_start = std::chrono::steady_clock::now();
      CU_CHECK(cdf->cuStreamSynchronize(stream.get()), "Couldn't synchronize software-encoder readback");
      const auto synchronization_end = std::chrono::steady_clock::now();
      dispatch_us.push_back(
        std::chrono::duration<double, std::micro>(synchronization_start - conversion_start).count()
      );
      readback_sync_us.push_back(
        std::chrono::duration<double, std::micro>(synchronization_end - synchronization_start).count()
      );
      if (mode == mode_e::identity_10bit) {
        const auto expansion_start = std::chrono::steady_clock::now();
        expand_10bit();
        depth_expansion_us.push_back(
          std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - expansion_start).count()
        );
      }
      total_conversion_us.push_back(
        std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - conversion_start).count()
      );
      return 0;
    }

  private:
    /**
     * @brief CUDA pinned-host allocation function loaded from the display driver.
     */
    using host_alloc_fn_t = CUresult(CUDAAPI *)(void **, std::size_t, unsigned int);

    /**
     * @brief CUDA pinned-host release function loaded from the display driver.
     */
    using host_free_fn_t = CUresult(CUDAAPI *)(void *);

    /**
     * @brief Active CUDA-to-software conversion mode.
     */
    enum class mode_e {
      unset,  ///< Frame format has not been attached.
      identity_8bit,  ///< Compact planar 8-bit identity GBR readback for libx264.
      identity_10bit,  ///< Compact identity GBR readback followed by CPU depth expansion.
      yuv422_8bit,  ///< CUDA Rec. 709 conversion to pinned planar 8-bit YUV 4:2:2.
      yuv422_10bit  ///< CUDA Rec. 709 conversion to pinned planar 10-bit YUV 4:2:2.
    };

    /**
     * @brief Start persistent workers used for 8-to-10-bit plane expansion.
     */
    void start_workers() {
      const auto available = std::max(1U, std::thread::hardware_concurrency());
      const auto worker_count = std::min(7U, available > 1U ? available - 1U : 0U);
      workers.reserve(worker_count);
      for (unsigned int index = 0; index < worker_count; ++index) {
        workers.emplace_back([this, index, worker_count](std::stop_token stop) {
          std::uint64_t observed_generation = 0;
          while (!stop.stop_requested()) {
            {
              std::unique_lock lock {work_mutex};
              work_ready.wait(lock, stop, [&]() {
                return generation != observed_generation;
              });
              if (stop.stop_requested()) {
                return;
              }
              observed_generation = generation;
            }

            expand_rows(index, worker_count + 1U);
            {
              std::lock_guard lock {work_mutex};
              if (--workers_pending == 0) {
                work_done.notify_one();
              }
            }
          }
        });
      }
    }

    /**
     * @brief Expand a strided share of compact 8-bit rows to full-range 10-bit.
     *
     * @param share_index Worker share index.
     * @param share_count Total worker and caller shares.
     */
    void expand_rows(unsigned int share_index, unsigned int share_count) {
      const auto *source = static_cast<const std::uint8_t *>(host_readback);
      for (std::size_t row = share_index; row < static_cast<std::size_t>(output_height) * 3U; row += share_count) {
        const auto plane = row / output_height;
        const auto plane_row = row % output_height;
        const auto *source_row = source + row * output_width;
        auto *destination_row = reinterpret_cast<std::uint16_t *>(frame->data[plane] + plane_row * frame->linesize[plane]);
        for (int x = 0; x < output_width; ++x) {
          destination_row[x] = video::expand_8bit_to_10bit(source_row[x]);
        }
      }
    }

    /**
     * @brief Expand all compact identity planes and wait for worker completion.
     *
     */
    void expand_10bit() {
      const auto worker_count = static_cast<unsigned int>(workers.size());
      {
        std::lock_guard lock {work_mutex};
        workers_pending = worker_count;
        ++generation;
      }
      work_ready.notify_all();
      expand_rows(worker_count, worker_count + 1U);

      if (worker_count != 0) {
        std::unique_lock lock {work_mutex};
        work_done.wait(lock, [&]() {
          return workers_pending == 0;
        });
      }
    }

    frame_t host_frame;  ///< Owned FFmpeg software frame.
    sws_t sws;  ///< CUDA identity-GBR converter.
    stream_t stream;  ///< CUDA conversion and readback stream.
    std::vector<std::jthread> workers;  ///< Persistent CPU depth-expansion workers.
    std::mutex work_mutex;  ///< Protects expansion generation and completion state.
    std::condition_variable_any work_ready;  ///< Signals a new compact frame to workers.
    std::condition_variable work_done;  ///< Signals completion of all worker shares.
    video::timing_samples_t dispatch_us;  ///< CUDA conversion/copy dispatch time before synchronization.
    video::timing_samples_t readback_sync_us;  ///< Wait for CUDA conversion and GPU-to-CPU readback completion.
    video::timing_samples_t depth_expansion_us;  ///< Exact compact 8-bit to 10-bit CPU expansion time.
    video::timing_samples_t total_conversion_us;  ///< Full CUDA software conversion and readback service time.
    std::uint64_t generation = 0;  ///< Current compact-frame expansion generation.
    unsigned int workers_pending = 0;  ///< Workers still expanding the current frame.
    CUdeviceptr device_compact = 0;  ///< Compact three-plane 8-bit CUDA surface.
    void *cuda_driver = nullptr;  ///< Dynamic CUDA driver handle for pinned-host APIs.
    host_alloc_fn_t host_alloc = nullptr;  ///< Pinned-host allocation entry point.
    host_free_fn_t host_free = nullptr;  ///< Pinned-host release entry point.
    void *host_readback = nullptr;  ///< Pinned native or compact host readback.
    std::size_t host_readback_size = 0;  ///< Pinned readback allocation size in bytes.
    int width = 0;  ///< NvFBC capture width.
    int height = 0;  ///< NvFBC capture height.
    int output_width = 0;  ///< Encoded frame width.
    int output_height = 0;  ///< Encoded frame height.
    int content_width = 0;  ///< Aspect-preserving scaled content width.
    int content_height = 0;  ///< Aspect-preserving scaled content height.
    int content_offset_x = 0;  ///< Horizontal letterbox offset in the encoded frame.
    int content_offset_y = 0;  ///< Vertical letterbox offset in the encoded frame.
    platf::pix_fmt_e capture_format = platf::pix_fmt_e::unknown;  ///< Negotiated software capture format.
    mode_e mode = mode_e::unset;  ///< Active readback and conversion mode.
  };

  /**
   * CUDA conversion surface registered directly with NVENC.
   */
  class cuda_nvenc_t final: public platf::nvenc_encode_device_t {
  public:
    ~cuda_nvenc_t() override {
      video::log_frame_timing("native10_nvenc_upload_gpu", upload_gpu_us);
      video::log_frame_timing("native10_nvenc_conversion_gpu", conversion_gpu_us);
      video::log_frame_timing("native10_nvenc_dispatch", dispatch_us);
      video::log_frame_timing("native10_nvenc_sync", sync_us);
      video::log_frame_timing("native10_nvenc_total", total_us);
      for (auto event : timing_events) {
        if (event) {
          CU_CHECK_IGNORE(cdf->cuEventDestroy(event), "Couldn't destroy NVENC timing event");
        }
      }
      if (timing_driver) {
        dlclose(timing_driver);
      }
      encoder.reset();
      nvenc = nullptr;
      if (native10_source && cdf) {
        CU_CHECK_IGNORE(cdf->cuMemFree(native10_source), "Couldn't free native X11 CUDA upload surface");
      }
      if (surface && cdf) {
        CU_CHECK_IGNORE(cdf->cuMemFree(surface), "Couldn't free direct NVENC surface");
      }
    }

    int init(int in_width, int in_height, platf::pix_fmt_e format) {
      if (!cdf) {
        BOOST_LOG(warning) << "cuda not initialized"sv;
        return -1;
      }

      input_width = in_width;
      input_height = in_height;
      buffer_format = format;

      CU_CHECK(cdf->cuCtxGetCurrent(&context), "Couldn't get current CUDA context");
      if (!context) {
        CU_CHECK(cdf->cuDeviceGet(&cuda_device, 0), "Couldn't get CUDA device");
        CU_CHECK(cdf->cuDevicePrimaryCtxRetain(&context, cuda_device), "Couldn't retain CUDA primary context");
        primary_context.device = cuda_device;
        primary_context.retained = true;
        CU_CHECK(cdf->cuCtxPushCurrent(context), "Couldn't select CUDA primary context");
        primary_context.pushed = true;
      }

      return 0;
    }

    int init_native10(int in_width, int in_height, platf::pix_fmt_e format) {
      if (init(in_width, in_height, format)) {
        return -1;
      }
      native10_input = true;
      return 0;
    }

    bool init_encoder(const ::video::config_t &client_config, const ::video::sunshine_colorspace_t &colorspace) override {
      output_width = client_config.width;
      output_height = client_config.height;
      const bool ten_bit = buffer_format == platf::pix_fmt_e::p010 || buffer_format == platf::pix_fmt_e::yuv444p16;
      const bool yuv444 = buffer_format == platf::pix_fmt_e::yuv444p || buffer_format == platf::pix_fmt_e::yuv444p16;
      if (buffer_format != platf::pix_fmt_e::nv12 && buffer_format != platf::pix_fmt_e::p010 && !yuv444) {
        BOOST_LOG(error) << "Direct CUDA NVENC does not support pixel format " << platf::from_pix_fmt(buffer_format);
        return false;
      }
      if (native10_input &&
          (buffer_format != platf::pix_fmt_e::yuv444p16 ||
           colorspace.colorspace != video::colorspace_e::identity_gbr ||
           colorspace.bit_depth != 10)) {
        BOOST_LOG(error) << "Native X11 direct NVENC requires 10-bit 4:4:4 identity input"sv;
        return false;
      }
      if (native10_input &&
          (output_width != input_width || output_height != input_height)) {
        BOOST_LOG(info) << "Native X11 direct NVENC CUDA scaling: "sv
                        << input_width << 'x' << input_height << " -> "sv
                        << output_width << 'x' << output_height;
      }

      const std::size_t row_bytes = static_cast<std::size_t>(output_width) * (ten_bit ? 2U : 1U);
      const std::size_t rows = static_cast<std::size_t>(output_height) * (yuv444 ? 3U : 3U) / (yuv444 ? 1U : 2U);
      std::size_t allocated_pitch = 0;
      if (check(cdf->cuMemAllocPitch(&surface, &allocated_pitch, row_bytes, rows, 16U), "Couldn't allocate direct NVENC surface: "sv)) {
        return false;
      }
      pitch = static_cast<std::uint32_t>(allocated_pitch);

      if (native10_input) {
        std::size_t source_pitch = 0;
        if (check(cdf->cuMemAllocPitch(&native10_source, &source_pitch, static_cast<std::size_t>(input_width) * 4U, input_height, 16U), "Couldn't allocate native X11 CUDA upload surface: "sv)) {
          return false;
        }
        native10_source_pitch = static_cast<std::uint32_t>(source_pitch);
      }

      stream = make_stream();
      if (!stream) {
        return false;
      }
      if (native10_input) {
        init_timing();
      }
      if (!native10_input) {
        auto converter = sws_t::make(input_width, input_height, output_width, output_height, input_width * 4);
        if (!converter) {
          return false;
        }
        sws = std::move(*converter);
        sws.apply_colorspace(colorspace);
        if (sws.viewport.width < input_width || sws.viewport.height < input_height) {
          area_scaler = area_scaler_t::make(input_width, input_height, sws.viewport.width, sws.viewport.height);
          if (!area_scaler) {
            return false;
          }
          // The filtered texture already matches the content viewport. Keep its
          // letterbox offsets, but do not sample/resize it a second time.
          sws.scale = 1.0f;
          BOOST_LOG(info) << "NvFBC direct NVENC GPU area scaling: "sv
                          << input_width << 'x' << input_height << " -> "sv
                          << sws.viewport.width << 'x' << sws.viewport.height;
        }
      }

      encoder = ::nvenc::make_nvenc_cuda_encoder({
        context,
        static_cast<std::uintptr_t>(surface),
        pitch,
      });
      if (!encoder || !encoder->create_encoder(config::video.nv, client_config, colorspace, buffer_format)) {
        encoder.reset();
        return false;
      }
      nvenc = encoder.get();
      return true;
    }

    int convert(platf::img_t &img) override {
      const auto started = std::chrono::steady_clock::now();
      // Every early exit drains work before capture can reuse/unmap img. This
      // also covers a conversion failure after a successful asynchronous upload.
      auto drain = util::fail_guard([&]() {
        CU_CHECK_IGNORE(cdf->cuStreamSynchronize(stream.get()), "Couldn't drain failed NVENC conversion");
      });
      auto *base = reinterpret_cast<std::uint8_t *>(static_cast<std::uintptr_t>(surface));
      int status = 0;
      if (native10_input) {
        if (!img.data || img.width != input_width || img.height != input_height ||
            img.pixel_pitch != 4 || img.row_pitch < input_width * 4) {
          BOOST_LOG(error) << "Native X11 direct NVENC received an incompatible packed RGB10 frame"sv;
          return -1;
        }
        CUDA_MEMCPY2D copy {};
        copy.srcMemoryType = CU_MEMORYTYPE_HOST;
        copy.srcHost = img.data;
        copy.srcPitch = static_cast<std::size_t>(img.row_pitch);
        copy.dstMemoryType = CU_MEMORYTYPE_DEVICE;
        copy.dstDevice = native10_source;
        copy.dstPitch = native10_source_pitch;
        copy.WidthInBytes = static_cast<std::size_t>(input_width) * 4U;
        copy.Height = input_height;
        record_timing(0);
        CU_CHECK(cdf->cuMemcpy2DAsync(&copy, stream.get()), "Couldn't upload native X11 RGB10 frame");
        record_timing(1);
        status = scale_xrgb10_to_yuv444_10bit(
          static_cast<std::uintptr_t>(native10_source),
          native10_source_pitch,
          input_width,
          input_height,
          base,
          base + pitch * output_height,
          base + pitch * output_height * 2,
          pitch,
          output_width,
          output_height,
          stream.get()
        );
        record_timing(2);
      } else {
        auto &texture = static_cast<cuda::img_t &>(img).tex;
        auto source = texture.texture.point;
        if (area_scaler) {
          status = area_scaler->resize(source, stream.get());
          if (status) {
            return status;
          }
          source = area_scaler->texture();
        }
        if (buffer_format == platf::pix_fmt_e::nv12) {
          status = sws.convert_nv12(base, base + pitch * output_height, pitch, pitch, source, stream.get());
        } else if (buffer_format == platf::pix_fmt_e::p010) {
          status = sws.convert_p010(base, base + pitch * output_height, pitch, pitch, source, stream.get());
        } else if (buffer_format == platf::pix_fmt_e::yuv444p16) {
          status = sws.convert_yuv444_10bit(base, base + pitch * output_height, base + pitch * output_height * 2, pitch, source, stream.get());
        } else {
          status = sws.convert_yuv444(base, base + pitch * output_height, base + pitch * output_height * 2, pitch, source, stream.get());
        }
      }
      const auto submitted = std::chrono::steady_clock::now();
      const auto synchronized = cdf->cuStreamSynchronize(stream.get());
      drain.disable();
      CU_CHECK(synchronized, "Couldn't synchronize direct NVENC conversion");
      if (status) {
        return status;
      }
      if (native10_input) {
        const auto completed = std::chrono::steady_clock::now();
        dispatch_us.push_back(std::chrono::duration<double, std::micro>(submitted - started).count());
        sync_us.push_back(std::chrono::duration<double, std::micro>(completed - submitted).count());
        total_us.push_back(std::chrono::duration<double, std::micro>(completed - started).count());
        if (timing_ready) {
          float upload = 0, conversion = 0;
          if (elapsed_time(&upload, timing_events[0], timing_events[1]) == CUDA_SUCCESS &&
              elapsed_time(&conversion, timing_events[1], timing_events[2]) == CUDA_SUCCESS) {
            upload_gpu_us.push_back(upload * 1000.0);
            conversion_gpu_us.push_back(conversion * 1000.0);
          } else {
            timing_ready = false;
            BOOST_LOG(warning) << "Native X11 CUDA event timing unavailable; CPU stage timing retained"sv;
          }
        }
      }
      return 0;
    }

  private:
    void init_timing() {
      timing_driver = dlopen("libcuda.so.1", RTLD_NOW | RTLD_LOCAL);
      if (!timing_driver) {
        return;
      }
      elapsed_time = reinterpret_cast<elapsed_time_fn *>(dlsym(timing_driver, "cuEventElapsedTime"));
      if (!elapsed_time) {
        return;
      }
      for (auto &event : timing_events) {
        CUevent created = nullptr;
        if (cdf->cuEventCreate(&created, 0) != CUDA_SUCCESS) {
          return;
        }
        event = created;
      }
      timing_ready = true;
    }

    void record_timing(unsigned index) {
      if (timing_ready && cdf->cuEventRecord(timing_events[index], stream.get()) != CUDA_SUCCESS) {
        timing_ready = false;
        BOOST_LOG(warning) << "Native X11 CUDA event timing unavailable; CPU stage timing retained"sv;
      }
    }

    using elapsed_time_fn = CUresult CUDAAPI(float *, CUevent, CUevent);
    void *timing_driver = nullptr;
    elapsed_time_fn *elapsed_time = nullptr;
    CUevent timing_events[3] {};
    bool timing_ready = false;
    video::timing_samples_t upload_gpu_us, conversion_gpu_us, dispatch_us, sync_us, total_us;

    struct primary_context_t {
      ~primary_context_t() {
        if (retained && cdf) {
          if (pushed) {
            CUcontext popped = nullptr;
            CU_CHECK_IGNORE(cdf->cuCtxPopCurrent(&popped), "Couldn't pop CUDA primary context");
          }
          CU_CHECK_IGNORE(cdf->cuDevicePrimaryCtxRelease(device), "Couldn't release CUDA primary context");
        }
      }

      CUdevice device = 0;
      bool retained = false;
      bool pushed = false;
    } primary_context;

    std::unique_ptr<::nvenc::nvenc_encoder> encoder;
    sws_t sws;
    std::unique_ptr<area_scaler_t> area_scaler;
    stream_t stream;
    CUcontext context = nullptr;
    CUdeviceptr surface = 0;
    CUdeviceptr native10_source = 0;
    CUdevice cuda_device = 0;
    std::uint32_t pitch = 0;
    std::uint32_t native10_source_pitch = 0;
    int input_width = 0;
    int input_height = 0;
    int output_width = 0;
    int output_height = 0;
    platf::pix_fmt_e buffer_format = platf::pix_fmt_e::unknown;
    bool native10_input = false;
  };

  std::unique_ptr<platf::avcodec_encode_device_t> make_avcodec_software_encode_device(int width, int height, platf::pix_fmt_e pix_fmt) {
    if (init()) {
      return nullptr;
    }

    auto device = std::make_unique<cuda_software_t>();
    if (device->init(width, height, pix_fmt)) {
      return nullptr;
    }
    return device;
  }

  std::unique_ptr<platf::nvenc_encode_device_t> make_nvenc_encode_device(int width, int height, platf::pix_fmt_e pix_fmt) {
    if (init()) {
      return nullptr;
    }

    auto device = std::make_unique<cuda_nvenc_t>();
    if (device->init(width, height, pix_fmt)) {
      return nullptr;
    }
    return device;
  }

  std::unique_ptr<platf::nvenc_encode_device_t> make_nvenc_native10_encode_device(int width, int height, platf::pix_fmt_e pix_fmt) {
    if (init()) {
      return nullptr;
    }

    auto device = std::make_unique<cuda_nvenc_t>();
    if (device->init_native10(width, height, pix_fmt)) {
      return nullptr;
    }
    return device;
  }

  namespace nvfbc {
    static PNVFBCCREATEINSTANCE createInstance {};
    static NVFBC_API_FUNCTION_LIST func {NVFBC_VERSION};

    static constexpr inline NVFBC_BOOL nv_bool(bool b) {
      return b ? NVFBC_TRUE : NVFBC_FALSE;
    }

    static void *handle {nullptr};

    /**
     * @brief Load NvFBC and create the CUDA capture helper.
     *
     * @return 0 on success; nonzero or negative platform status on failure.
     */
    int init() {
      static bool funcs_loaded = false;

      if (funcs_loaded) {
        return 0;
      }

      if (!handle) {
        handle = dyn::handle({"libnvidia-fbc.so.1", "libnvidia-fbc.so"});
        if (!handle) {
          return -1;
        }
      }

      std::vector<std::tuple<dyn::apiproc *, const char *>> funcs {
        {(dyn::apiproc *) &createInstance, "NvFBCCreateInstance"},
      };

      if (dyn::load(handle, funcs)) {
        dlclose(handle);
        handle = nullptr;

        return -1;
      }

      auto status = cuda::nvfbc::createInstance(&cuda::nvfbc::func);
      if (status) {
        BOOST_LOG(error) << "Unable to create NvFBC instance"sv;

        dlclose(handle);
        handle = nullptr;
        return -1;
      }

      funcs_loaded = true;
      return 0;
    }

    /**
     * @brief NvFBC CUDA context selected for a capture session.
     */
    class ctx_t {
    public:
      /**
       * @brief Create an NvFBC session context for a native capture handle.
       *
       * @param handle Native library or object handle used by the operation.
       */
      ctx_t(NVFBC_SESSION_HANDLE handle):
          handle {handle} {
        NVFBC_BIND_CONTEXT_PARAMS params {NVFBC_BIND_CONTEXT_PARAMS_VER};

        if (func.nvFBCBindContext(handle, &params)) {
          BOOST_LOG(error) << "Couldn't bind NvFBC context to current thread: " << func.nvFBCGetLastErrorStr(handle);
        } else {
          bound = true;
        }
      }

      ~ctx_t() {
        if (!bound) {
          return;
        }

        NVFBC_RELEASE_CONTEXT_PARAMS params {NVFBC_RELEASE_CONTEXT_PARAMS_VER};
        if (func.nvFBCReleaseContext(handle, &params)) {
          BOOST_LOG(error) << "Couldn't release NvFBC context from current thread: " << func.nvFBCGetLastErrorStr(handle);
        }
      }

      /**
       * @brief Mark the context as released by an operation that invalidated the handle.
       */
      void disarm() {
        bound = false;
      }

      NVFBC_SESSION_HANDLE handle;  ///< NVIDIA FBC capture session handle.
      bool bound {};  ///< Whether this object owns a context binding to release.
    };

    /**
     * @brief NvFBC dynamic-library handle and function table.
     */
    class handle_t {
      enum flag_e {
        SESSION_HANDLE,
        SESSION_CAPTURE,
        MAX_FLAGS,
      };

    public:
      handle_t() = default;

      /**
       * @brief Move an NvFBC API handle and its resolved function table.
       *
       * @param other Source object whose state is copied or moved into this object.
       */
      handle_t(handle_t &&other):
          handle_flags {other.handle_flags},
          handle {other.handle} {
        other.handle_flags.reset();
      }

      /**
       * @brief Assign state from another instance while preserving ownership semantics.
       *
       * @param other Source object whose state is copied or moved into this object.
       * @return Reference or value produced by the operator.
       */
      handle_t &operator=(handle_t &&other) {
        std::swap(handle_flags, other.handle_flags);
        std::swap(handle, other.handle);

        return *this;
      }

      /**
       * @brief Allocate the underlying object and wrap it in the owning handle.
       *
       * @return Created backend object, or null when creation fails.
       */
      static std::optional<handle_t> make() {
        NVFBC_CREATE_HANDLE_PARAMS params {NVFBC_CREATE_HANDLE_PARAMS_VER};

        // Set privateData to allow NvFBC on consumer NVIDIA GPUs.
        // Based on https://github.com/keylase/nvidia-patch/blob/3193b4b1cea91527bf09ea9b8db5aade6a3f3c0a/win/nvfbcwrp/nvfbcwrp_main.cpp#L23-L25 .
        const unsigned int MAGIC_PRIVATE_DATA[4] = {0xAEF57AC5, 0x401D1A39, 0x1B856BBE, 0x9ED0CEBA};
        params.privateData = MAGIC_PRIVATE_DATA;
        params.privateDataSize = sizeof(MAGIC_PRIVATE_DATA);

        handle_t handle;
        auto status = func.nvFBCCreateHandle(&handle.handle, &params);
        if (status) {
          BOOST_LOG(error) << "Failed to create session: "sv << handle.last_error();

          return std::nullopt;
        }

        handle.handle_flags[SESSION_HANDLE] = true;

        return handle;
      }

      /**
       * @brief Read the last error string from the active NvFBC session.
       *
       * @return Human-readable NvFBC error string.
       */
      const char *last_error() {
        return func.nvFBCGetLastErrorStr(handle);
      }

      /**
       * @brief Return or update the current status value.
       *
       * @return Status status.
       */
      std::optional<NVFBC_GET_STATUS_PARAMS> status() {
        NVFBC_GET_STATUS_PARAMS params {NVFBC_GET_STATUS_PARAMS_VER};

        auto status = func.nvFBCGetStatus(handle, &params);
        if (status) {
          BOOST_LOG(error) << "Failed to get NvFBC status: "sv << last_error();

          return std::nullopt;
        }

        return params;
      }

      /**
       * @brief Run the capture loop for this backend.
       *
       * @param capture_params Capture params.
       * @return Capture status reported to the streaming pipeline.
       */
      int capture(NVFBC_CREATE_CAPTURE_SESSION_PARAMS &capture_params) {
        if (func.nvFBCCreateCaptureSession(handle, &capture_params)) {
          BOOST_LOG(error) << "Failed to start capture session: "sv << last_error();
          return -1;
        }

        handle_flags[SESSION_CAPTURE] = true;

        NVFBC_TOCUDA_SETUP_PARAMS setup_params {
          NVFBC_TOCUDA_SETUP_PARAMS_VER,
          NVFBC_BUFFER_FORMAT_BGRA,
        };

        if (func.nvFBCToCudaSetUp(handle, &setup_params)) {
          BOOST_LOG(error) << "Failed to setup cuda interop with nvFBC: "sv << last_error();
          return -1;
        }
        return 0;
      }

      /**
       * @brief Release the NvFBC capture session and wait for capture work to stop.
       *
       * @return Stop status.
       */
      int stop() {
        if (!handle_flags[SESSION_CAPTURE]) {
          return 0;
        }

        NVFBC_DESTROY_CAPTURE_SESSION_PARAMS params {NVFBC_DESTROY_CAPTURE_SESSION_PARAMS_VER};

        if (func.nvFBCDestroyCaptureSession(handle, &params)) {
          BOOST_LOG(error) << "Couldn't destroy capture session: "sv << last_error();

          return -1;
        }

        handle_flags[SESSION_CAPTURE] = false;

        return 0;
      }

      /**
       * @brief Reset the object to its initial empty state.
       *
       * @return Reset status.
       */
      int reset() {
        if (!handle_flags[SESSION_HANDLE]) {
          return 0;
        }

        stop();

        NVFBC_DESTROY_HANDLE_PARAMS params {NVFBC_DESTROY_HANDLE_PARAMS_VER};

        ctx_t ctx {handle};
        if (func.nvFBCDestroyHandle(handle, &params)) {
          BOOST_LOG(error) << "Couldn't destroy session handle: "sv << func.nvFBCGetLastErrorStr(handle);
        } else {
          // NvFBCDestroyHandle() implicitly releases the context and invalidates
          // the handle, so ctx must not call NvFBCReleaseContext() afterward.
          ctx.disarm();
        }

        handle_flags[SESSION_HANDLE] = false;

        return 0;
      }

      ~handle_t() {
        reset();
      }

      std::bitset<MAX_FLAGS> handle_flags;  ///< Handle flags.

      NVFBC_SESSION_HANDLE handle;  ///< NVIDIA FBC capture session handle.
    };

    /**
     * @brief NvFBC display capture backend that produces CUDA frames.
     */
    class display_t: public platf::display_t {
    public:
      /**
       * @brief Initialize NvFBC capture for the selected display.
       *
       * @param display_name Display name.
       * @param config Configuration values to apply.
       * @return 0 on success; nonzero or negative platform status on failure.
       */
      int init(const std::string_view &display_name, const ::video::config_t &config) {
        auto handle = handle_t::make();
        if (!handle) {
          return -1;
        }

        ctx_t ctx {handle->handle};

        auto status_params = handle->status();
        if (!status_params) {
          return -1;
        }

        int streamedMonitor = -1;
        if (!display_name.empty()) {
          if (status_params->bXRandRAvailable) {
            int monitor_nr = -1;
            const auto [end, error] = std::from_chars(
              display_name.data(),
              display_name.data() + display_name.size(),
              monitor_nr
            );
            const bool numeric_name = error == std::errc {} && end == display_name.data() + display_name.size();
            if (!numeric_name) {
              for (std::uint32_t index = 0; index < status_params->dwOutputNum; ++index) {
                if (display_name == status_params->outputs[index].name) {
                  monitor_nr = static_cast<int>(index);
                  break;
                }
              }
            }

            if (monitor_nr < 0 || monitor_nr >= status_params->dwOutputNum) {
              BOOST_LOG(warning) << "Can't stream monitor ["sv << display_name << "], defaulting to virtual desktop"sv;
            } else {
              streamedMonitor = monitor_nr;
            }
          } else {
            BOOST_LOG(warning) << "XrandR not available, streaming entire virtual desktop"sv;
          }
        }

        delay = ::video::capture_frame_interval(config);

        capture_params = NVFBC_CREATE_CAPTURE_SESSION_PARAMS {NVFBC_CREATE_CAPTURE_SESSION_PARAMS_VER};

        capture_params.eCaptureType = NVFBC_CAPTURE_SHARED_CUDA;
        capture_params.bDisableAutoModesetRecovery = nv_bool(true);

        capture_params.dwSamplingRateMs = 1000 /* ms */ / config.framerate;

        if (streamedMonitor != -1) {
          auto &output = status_params->outputs[streamedMonitor];

          width = output.trackedBox.w;
          height = output.trackedBox.h;
          offset_x = output.trackedBox.x;
          offset_y = output.trackedBox.y;

          capture_params.eTrackingType = NVFBC_TRACKING_OUTPUT;
          capture_params.dwOutputId = output.dwId;
        } else {
          capture_params.eTrackingType = NVFBC_TRACKING_SCREEN;

          width = status_params->screenSize.w;
          height = status_params->screenSize.h;
        }

        env_width = status_params->screenSize.w;
        env_height = status_params->screenSize.h;

        this->handle = std::move(*handle);
        return 0;
      }

      platf::capture_e capture(const push_captured_image_cb_t &push_captured_image_cb, const pull_free_image_cb_t &pull_free_image_cb, bool *cursor) override {
        auto next_frame = std::chrono::steady_clock::now();

        {
          // We must create at least one texture on this thread before calling NvFBCToCudaSetUp()
          // Otherwise it fails with "Unable to register an OpenGL buffer to a CUDA resource (result: 201)" message
          std::shared_ptr<platf::img_t> img_dummy;
          pull_free_image_cb(img_dummy);
        }

        // Force display_t::capture to initialize handle_t::capture
        cursor_visible = !*cursor;

        ctx_t ctx {handle.handle};
        auto fg = util::fail_guard([&]() {
          handle.reset();
        });

        sleep_overshoot_logger.reset();

        while (true) {
          auto now = std::chrono::steady_clock::now();
          if (next_frame > now) {
            std::this_thread::sleep_for(next_frame - now);
            sleep_overshoot_logger.first_point(next_frame);
            sleep_overshoot_logger.second_point_now_and_log();
          }

          next_frame += delay;
          if (next_frame < now) {  // some major slowdown happened; we couldn't keep up
            next_frame = now + delay;
          }

          std::shared_ptr<platf::img_t> img_out;
          auto status = snapshot(pull_free_image_cb, img_out, 150ms, *cursor);
          switch (status) {
            case platf::capture_e::reinit:
            case platf::capture_e::error:
            case platf::capture_e::interrupted:
              return status;
            case platf::capture_e::timeout:
              if (!push_captured_image_cb(std::move(img_out), false)) {
                return platf::capture_e::ok;
              }
              break;
            case platf::capture_e::ok:
              if (!push_captured_image_cb(std::move(img_out), true)) {
                return platf::capture_e::ok;
              }
              break;
            default:
              BOOST_LOG(error) << "Unrecognized capture status ["sv << (int) status << ']';
              return status;
          }
        }

        return platf::capture_e::ok;
      }

      // Reinitialize the capture session.
      /**
       * @brief Reinitialize NvFBC capture after a recoverable failure.
       *
       * @param cursor Cursor image or visibility state to composite.
       * @return Reinit status.
       */
      platf::capture_e reinit(bool cursor) {
        if (handle.stop()) {
          return platf::capture_e::error;
        }

        cursor_visible = cursor;
        if (cursor) {
          capture_params.bPushModel = nv_bool(false);
          capture_params.bWithCursor = nv_bool(true);
          capture_params.bAllowDirectCapture = nv_bool(false);
        } else {
          capture_params.bPushModel = nv_bool(true);
          capture_params.bWithCursor = nv_bool(false);
          capture_params.bAllowDirectCapture = nv_bool(true);
        }

        if (handle.capture(capture_params)) {
          return platf::capture_e::error;
        }

        // If trying to capture directly, test if it actually does.
        if (capture_params.bAllowDirectCapture) {
          CUdeviceptr device_ptr;
          NVFBC_FRAME_GRAB_INFO info;

          NVFBC_TOCUDA_GRAB_FRAME_PARAMS grab {
            NVFBC_TOCUDA_GRAB_FRAME_PARAMS_VER,
            NVFBC_TOCUDA_GRAB_FLAGS_NOWAIT,
            &device_ptr,
            &info,
            0,
          };

          // Direct Capture may fail the first few times, even if it's possible
          for (int x = 0; x < 3; ++x) {
            if (auto status = func.nvFBCToCudaGrabFrame(handle.handle, &grab)) {
              if (status == NVFBC_ERR_MUST_RECREATE) {
                return platf::capture_e::reinit;
              }

              BOOST_LOG(error) << "Couldn't capture nvFramebuffer: "sv << handle.last_error();

              return platf::capture_e::error;
            }

            if (info.bDirectCapture) {
              break;
            }

            BOOST_LOG(debug) << "Direct capture failed attempt ["sv << x << ']';
          }

          if (!info.bDirectCapture) {
            BOOST_LOG(debug) << "Direct capture failed, trying the extra copy method"sv;
            // Direct capture failed
            capture_params.bPushModel = nv_bool(false);
            capture_params.bWithCursor = nv_bool(false);
            capture_params.bAllowDirectCapture = nv_bool(false);

            if (handle.stop() || handle.capture(capture_params)) {
              return platf::capture_e::error;
            }
          }
        }

        return platf::capture_e::ok;
      }

      /**
       * @brief Capture a display frame into the provided image object.
       *
       * @param pull_free_image_cb Callback that provides an available image buffer.
       * @param img_out Captured CUDA image returned to the streaming pipeline.
       * @param timeout Maximum time to wait for the operation.
       * @param cursor Cursor image or visibility state to composite.
       * @return Capture status reported to the streaming pipeline.
       */
      platf::capture_e snapshot(const pull_free_image_cb_t &pull_free_image_cb, std::shared_ptr<platf::img_t> &img_out, std::chrono::milliseconds timeout, bool cursor) {
        if (cursor != cursor_visible) {
          auto status = reinit(cursor);
          if (status != platf::capture_e::ok) {
            return status;
          }
        }

        CUdeviceptr device_ptr;
        NVFBC_FRAME_GRAB_INFO info;

        NVFBC_TOCUDA_GRAB_FRAME_PARAMS grab {
          NVFBC_TOCUDA_GRAB_FRAME_PARAMS_VER,
          NVFBC_TOCUDA_GRAB_FLAGS_NOWAIT,
          &device_ptr,
          &info,
          (std::uint32_t) timeout.count(),
        };

        if (auto status = func.nvFBCToCudaGrabFrame(handle.handle, &grab)) {
          if (status == NVFBC_ERR_MUST_RECREATE) {
            return platf::capture_e::reinit;
          }

          BOOST_LOG(error) << "Couldn't capture nvFramebuffer: "sv << handle.last_error();
          return platf::capture_e::error;
        }

        if (!pull_free_image_cb(img_out)) {
          return platf::capture_e::interrupted;
        }
        auto img = (img_t *) img_out.get();

        // NvFBC reports CLOCK_MONOTONIC microseconds for the time the display
        // server started rendering a new frame. Preserve it so the streaming
        // protocol can report capture-to-packet processing latency. Repeated
        // frames retain the old driver timestamp and must remain untimestamped.
        if (info.bIsNewFrame && info.ulTimestampUs != 0) {
          const auto timestamp_us = std::chrono::microseconds {
            static_cast<std::chrono::microseconds::rep>(info.ulTimestampUs)
          };
          const auto timestamp_duration =
            std::chrono::duration_cast<std::chrono::steady_clock::duration>(timestamp_us);
          const auto frame_timestamp = std::chrono::steady_clock::time_point {timestamp_duration};
          if (frame_timestamp <= std::chrono::steady_clock::now()) {
            img_out->frame_timestamp = frame_timestamp;
          }
        }

        if (img->tex.copy((std::uint8_t *) device_ptr, img->height, img->row_pitch)) {
          return platf::capture_e::error;
        }

        return platf::capture_e::ok;
      }

      /**
       * @brief Create the NvFBC CUDA readback device used by software x264.
       *
       * @param pix_fmt Software pixel format requested by x264.
       * @return Constructed CUDA-to-CPU conversion device.
       */
      std::unique_ptr<platf::avcodec_encode_device_t> make_avcodec_software_encode_device(platf::pix_fmt_e pix_fmt) override {
        return ::cuda::make_avcodec_software_encode_device(width, height, pix_fmt);
      }

      std::unique_ptr<platf::nvenc_encode_device_t> make_nvenc_encode_device(platf::pix_fmt_e pix_fmt) override {
        return ::cuda::make_nvenc_encode_device(width, height, pix_fmt);
      }

      /**
       * @brief Allocate an image buffer compatible with this display backend.
       *
       * @return Allocated img object, or null when unavailable.
       */
      std::shared_ptr<platf::img_t> alloc_img() override {
        auto img = std::make_shared<cuda::img_t>();

        img->data = nullptr;
        img->width = width;
        img->height = height;
        img->pixel_pitch = 4;
        img->row_pitch = img->width * img->pixel_pitch;

        auto tex_opt = tex_t::make(height, width * img->pixel_pitch);
        if (!tex_opt) {
          return nullptr;
        }

        img->tex = std::move(*tex_opt);

        return img;
      };

      /**
       * @brief Populate a fallback image when real capture data is unavailable.
       *
       * @return Capture status reported to the streaming pipeline.
       */
      int dummy_img(platf::img_t *) override {
        return 0;
      }

      std::chrono::nanoseconds delay;  ///< Delay before the timer task becomes eligible to run.

      bool cursor_visible;  ///< Whether the cursor should be included in the capture.
      handle_t handle;  ///< NvFBC capture handle owning the active capture session.

      NVFBC_CREATE_CAPTURE_SESSION_PARAMS capture_params;  ///< NvFBC capture-session parameters used for frame grabs.
    };
  }  // namespace nvfbc
}  // namespace cuda

namespace platf {
  /**
   * @brief Create an NvFBC CUDA display capture backend.
   *
   * @param hwdevice_type Hardware device type requested for capture or encode.
   * @param display_name Display name.
   * @param config Configuration values to apply.
   * @return Display backend, or nullptr when NvFBC/CUDA initialization fails.
   */
  std::shared_ptr<display_t> nvfbc_display(mem_type_e hwdevice_type, const std::string &display_name, const video::config_t &config) {
    if (hwdevice_type != mem_type_e::cuda) {
      BOOST_LOG(error) << "Could not initialize nvfbc display with the given hw device type"sv;
      return nullptr;
    }

    auto display = std::make_shared<cuda::nvfbc::display_t>();

    if (display->init(display_name, config)) {
      return nullptr;
    }

    return display;
  }

  /**
   * @brief Enumerate display names exposed by NvFBC.
   *
   * @return NvFBC display names, or an empty list when enumeration fails.
   */
  std::vector<std::string> nvfbc_display_names() {
    if (cuda::init() || cuda::nvfbc::init()) {
      return {};
    }

    std::vector<std::string> display_names;

    auto handle = cuda::nvfbc::handle_t::make();
    if (!handle) {
      return {};
    }

    auto status_params = handle->status();
    if (!status_params) {
      return {};
    }

    if (!status_params->bIsCapturePossible) {
      BOOST_LOG(error) << "NVidia driver doesn't support NvFBC screencasting"sv;
    }

    BOOST_LOG(info) << "Found ["sv << status_params->dwOutputNum << "] outputs"sv;
    BOOST_LOG(info) << "Virtual Desktop: "sv << status_params->screenSize.w << 'x' << status_params->screenSize.h;
    BOOST_LOG(info) << "XrandR: "sv << (status_params->bXRandRAvailable ? "available"sv : "unavailable"sv);

    for (auto x = 0; x < status_params->dwOutputNum; ++x) {
      auto &output = status_params->outputs[x];
      BOOST_LOG(info) << "-- Output --"sv;
      BOOST_LOG(debug) << "  ID: "sv << output.dwId;
      BOOST_LOG(debug) << "  Name: "sv << output.name;
      BOOST_LOG(info) << "  Resolution: "sv << output.trackedBox.w << 'x' << output.trackedBox.h;
      BOOST_LOG(info) << "  Offset: "sv << output.trackedBox.x << 'x' << output.trackedBox.y;
      display_names.emplace_back(output.name);
    }

    return display_names;
  }
}  // namespace platf
