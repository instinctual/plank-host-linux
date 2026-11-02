/**
 * @file src/platform/linux/x11grab.cpp
 * @brief Definitions for x11 capture.
 */
// standard includes
#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <mutex>
#include <ranges>
#include <thread>
#include <vector>

// plaform includes
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/stat.h>
#include <unistd.h>
#include <X11/Xatom.h>
#include <X11/extensions/shape.h>
#include <X11/extensions/Xfixes.h>
#include <X11/extensions/Xrandr.h>
#include <X11/X.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <xcb/shm.h>
#include <xcb/xfixes.h>

// local includes
#include "cuda.h"
#include "graphics.h"
#include "misc.h"
#include "src/config.h"
#include "src/globals.h"
#include "src/logging.h"
#include "src/platform/common.h"
#include "src/video.h"
#include "x11grab.h"
#include "x11_worker_exit.h"

using namespace std::literals;

namespace platf {
  void x11::unpack_xrgb10_to_gbr10_row(const std::uint32_t *source,
                                       std::uint16_t *green,
                                       std::uint16_t *blue,
                                       std::uint16_t *red,
                                       std::size_t pixel_count) {
    for (std::size_t x = 0; x < pixel_count; ++x) {
      const std::uint32_t pixel = source[x];
      red[x] = static_cast<std::uint16_t>(pixel & 0x3ffU);
      green[x] = static_cast<std::uint16_t>((pixel >> 10U) & 0x3ffU);
      blue[x] = static_cast<std::uint16_t>((pixel >> 20U) & 0x3ffU);
    }
  }

  namespace {
    constexpr std::uint32_t bilinear_weight_one = 1U << 16U;

    struct bilinear_axis_sample_t {
      int lower;
      int upper;
      std::uint32_t upper_weight;
    };

    std::vector<bilinear_axis_sample_t> make_bilinear_axis_samples(
      int source_size,
      int destination_size
    ) {
      std::vector<bilinear_axis_sample_t> samples;
      samples.reserve(static_cast<std::size_t>(destination_size));
      for (int destination = 0; destination < destination_size; ++destination) {
        const double source_position = std::max(
          0.0,
          (static_cast<double>(destination) + 0.5) * source_size /
              destination_size -
            0.5
        );
        const int lower = std::min(static_cast<int>(source_position), source_size - 1);
        const int upper = std::min(lower + 1, source_size - 1);
        const auto upper_weight = static_cast<std::uint32_t>(std::llround(
          (source_position - lower) * bilinear_weight_one
        ));
        samples.push_back({lower, upper, upper_weight});
      }
      return samples;
    }

    std::uint16_t bilinear_xrgb10_channel(
      std::uint32_t top_left,
      std::uint32_t top_right,
      std::uint32_t bottom_left,
      std::uint32_t bottom_right,
      unsigned int shift,
      std::uint32_t x_weight,
      std::uint32_t y_weight
    ) {
      const std::uint64_t x_inverse = bilinear_weight_one - x_weight;
      const std::uint64_t y_inverse = bilinear_weight_one - y_weight;
      const auto channel = [shift](std::uint32_t pixel) {
        return static_cast<std::uint64_t>((pixel >> shift) & 0x3ffU);
      };
      const std::uint64_t weighted =
        channel(top_left) * x_inverse * y_inverse +
        channel(top_right) * x_weight * y_inverse +
        channel(bottom_left) * x_inverse * y_weight +
        channel(bottom_right) * x_weight * y_weight;
      return static_cast<std::uint16_t>((weighted + (1ULL << 31U)) >> 32U);
    }

    void scale_xrgb10_to_gbr10_rows(
      const std::uint8_t *source,
      std::size_t source_pitch,
      const std::vector<bilinear_axis_sample_t> &x_samples,
      const std::vector<bilinear_axis_sample_t> &y_samples,
      std::uint8_t *green,
      std::size_t green_pitch,
      std::uint8_t *blue,
      std::size_t blue_pitch,
      std::uint8_t *red,
      std::size_t red_pitch,
      unsigned int share_index,
      unsigned int share_count
    ) {
      for (std::size_t y = share_index; y < y_samples.size(); y += share_count) {
        const auto &y_sample = y_samples[y];
        const auto *top = reinterpret_cast<const std::uint32_t *>(
          source + static_cast<std::size_t>(y_sample.lower) * source_pitch);
        const auto *bottom = reinterpret_cast<const std::uint32_t *>(
          source + static_cast<std::size_t>(y_sample.upper) * source_pitch);
        auto *green_row = reinterpret_cast<std::uint16_t *>(green + y * green_pitch);
        auto *blue_row = reinterpret_cast<std::uint16_t *>(blue + y * blue_pitch);
        auto *red_row = reinterpret_cast<std::uint16_t *>(red + y * red_pitch);
        for (std::size_t x = 0; x < x_samples.size(); ++x) {
          const auto &x_sample = x_samples[x];
          const auto top_left = top[x_sample.lower];
          const auto top_right = top[x_sample.upper];
          const auto bottom_left = bottom[x_sample.lower];
          const auto bottom_right = bottom[x_sample.upper];
          red_row[x] = bilinear_xrgb10_channel(
            top_left, top_right, bottom_left, bottom_right, 0U,
            x_sample.upper_weight, y_sample.upper_weight);
          green_row[x] = bilinear_xrgb10_channel(
            top_left, top_right, bottom_left, bottom_right, 10U,
            x_sample.upper_weight, y_sample.upper_weight);
          blue_row[x] = bilinear_xrgb10_channel(
            top_left, top_right, bottom_left, bottom_right, 20U,
            x_sample.upper_weight, y_sample.upper_weight);
        }
      }
    }
  }  // namespace

  void x11::scale_xrgb10_to_gbr10(const std::uint8_t *source,
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
                                  int destination_height) {
    if (!source || !green || !blue || !red || source_width <= 0 || source_height <= 0 ||
        destination_width <= 0 || destination_height <= 0) {
      return;
    }
    const auto x_samples = make_bilinear_axis_samples(source_width, destination_width);
    const auto y_samples = make_bilinear_axis_samples(source_height, destination_height);
    scale_xrgb10_to_gbr10_rows(
      source, source_pitch, x_samples, y_samples,
      reinterpret_cast<std::uint8_t *>(green), green_pitch,
      reinterpret_cast<std::uint8_t *>(blue), blue_pitch,
      reinterpret_cast<std::uint8_t *>(red), red_pitch, 0U, 1U);
  }

  /**
   * @brief Load XCB entry points used by the X11 capture backend.
   *
   * @return 0 when required XCB symbols are loaded; nonzero otherwise.
   */
  int load_xcb();
  /**
   * @brief Load X11 entry points used by the X11 capture backend.
   *
   * @return 0 when required X11 symbols are loaded; nonzero otherwise.
   */
  int load_x11();

  namespace x11 {
/**
 * @def _FN(x, ret, args)
 * @brief Macro for FN.
 */
#define _FN(x, ret, args) \
  /** \
   * @brief Function pointer type for the dynamically loaded X11 entry point. \
   */ \
  typedef ret(*x##_fn) args; \
  /** \
   * @brief Loaded X11 entry point pointer. \
   */ \
  static x##_fn x

    _FN(OpenDisplay, Display *, (_Xconst char *display_name));
    _FN(GetWindowAttributes, Status, (Display * display, Window w, XWindowAttributes *window_attributes_return));
    _FN(QueryPointer, Bool, (Display * display, Window w, Window *root_return, Window *child_return, int *root_x_return, int *root_y_return, int *win_x_return, int *win_y_return, unsigned int *mask_return));
    _FN(Pending, int, (Display * display));
    _FN(NextEvent, int, (Display * display, XEvent *event_return));
    _FN(Flush, int, (Display * display));
    _FN(CreateSimpleWindow, Window, (Display * display, Window parent, int x, int y, unsigned int width, unsigned int height, unsigned int border_width, unsigned long border, unsigned long background));
    _FN(ChangeWindowAttributes, int, (Display * display, Window w, unsigned long valuemask, XSetWindowAttributes *attributes));
    _FN(InternAtom, Atom, (Display * display, _Xconst char *atom_name, Bool only_if_exists));
    _FN(ChangeProperty, int, (Display * display, Window w, Atom property, Atom type, int format, int mode, _Xconst unsigned char *data, int nelements));
    _FN(MapRaised, int, (Display * display, Window w));
    _FN(DestroyWindow, int, (Display * display, Window w));
    _FN(Sync, int, (Display * display, Bool discard));

    _FN(CloseDisplay, int, (Display * display));
    _FN(Free, int, (void *data));
    _FN(InitThreads, Status, (void) );
    _FN(SetIOErrorHandler, XIOErrorHandler, (XIOErrorHandler handler));

    namespace composite {
      _FN(GetOverlayWindow, Window, (Display * display, Window window));
      _FN(ReleaseOverlayWindow, void, (Display * display, Window window));

      static int init() {
        static void *handle {nullptr};
        static bool funcs_loaded = false;
        if (funcs_loaded) {
          return 0;
        }
        if (!handle) {
          handle = dyn::handle({"libXcomposite.so.1", "libXcomposite.so"});
          if (!handle) {
            return -1;
          }
        }
        std::vector<std::tuple<dyn::apiproc *, const char *>> funcs {
          {(dyn::apiproc *) &GetOverlayWindow, "XCompositeGetOverlayWindow"},
          {(dyn::apiproc *) &ReleaseOverlayWindow, "XCompositeReleaseOverlayWindow"},
        };
        if (dyn::load(handle, funcs)) {
          return -1;
        }
        funcs_loaded = true;
        return 0;
      }
    }  // namespace composite

    namespace shape {
      _FN(CombineRectangles, void, (Display * display, Window dest, int dest_kind, int x_off, int y_off, XRectangle *rectangles, int n_rects, int op, int ordering));

      static int init() {
        static void *handle {nullptr};
        static bool funcs_loaded = false;
        if (funcs_loaded) {
          return 0;
        }
        if (!handle) {
          handle = dyn::handle({"libXext.so.6", "libXext.so"});
          if (!handle) {
            return -1;
          }
        }
        std::vector<std::tuple<dyn::apiproc *, const char *>> funcs {
          {(dyn::apiproc *) &CombineRectangles, "XShapeCombineRectangles"},
        };
        if (dyn::load(handle, funcs)) {
          return -1;
        }
        funcs_loaded = true;
        return 0;
      }
    }  // namespace shape

    namespace rr {
      _FN(GetScreenResources, XRRScreenResources *, (Display * dpy, Window window));
      _FN(GetOutputInfo, XRROutputInfo *, (Display * dpy, XRRScreenResources *resources, RROutput output));
      _FN(GetCrtcInfo, XRRCrtcInfo *, (Display * dpy, XRRScreenResources *resources, RRCrtc crtc));
      _FN(GetOutputPrimary, RROutput, (Display * dpy, Window window));
      _FN(FreeScreenResources, void, (XRRScreenResources * resources));
      _FN(FreeOutputInfo, void, (XRROutputInfo * outputInfo));
      _FN(FreeCrtcInfo, void, (XRRCrtcInfo * crtcInfo));

      static int init() {
        static void *handle {nullptr};
        static bool funcs_loaded = false;

        if (funcs_loaded) {
          return 0;
        }

        if (!handle) {
          handle = dyn::handle({"libXrandr.so.2", "libXrandr.so"});
          if (!handle) {
            return -1;
          }
        }

        std::vector<std::tuple<dyn::apiproc *, const char *>> funcs {
          {(dyn::apiproc *) &GetScreenResources, "XRRGetScreenResources"},
          {(dyn::apiproc *) &GetOutputInfo, "XRRGetOutputInfo"},
          {(dyn::apiproc *) &GetCrtcInfo, "XRRGetCrtcInfo"},
          {(dyn::apiproc *) &GetOutputPrimary, "XRRGetOutputPrimary"},
          {(dyn::apiproc *) &FreeScreenResources, "XRRFreeScreenResources"},
          {(dyn::apiproc *) &FreeOutputInfo, "XRRFreeOutputInfo"},
          {(dyn::apiproc *) &FreeCrtcInfo, "XRRFreeCrtcInfo"},
        };

        if (dyn::load(handle, funcs)) {
          return -1;
        }

        funcs_loaded = true;
        return 0;
      }

    }  // namespace rr

    namespace fix {
      _FN(GetCursorImage, XFixesCursorImage *, (Display * dpy));
      _FN(QueryExtension, Bool, (Display * dpy, int *event_base_return, int *error_base_return));
      _FN(SelectCursorInput, void, (Display * dpy, Window win, unsigned long event_mask));

      static int init() {
        static void *handle {nullptr};
        static bool funcs_loaded = false;

        if (funcs_loaded) {
          return 0;
        }

        if (!handle) {
          handle = dyn::handle({"libXfixes.so.3", "libXfixes.so"});
          if (!handle) {
            return -1;
          }
        }

        std::vector<std::tuple<dyn::apiproc *, const char *>> funcs {
          {(dyn::apiproc *) &GetCursorImage, "XFixesGetCursorImage"},
          {(dyn::apiproc *) &QueryExtension, "XFixesQueryExtension"},
          {(dyn::apiproc *) &SelectCursorInput, "XFixesSelectCursorInput"},
        };

        if (dyn::load(handle, funcs)) {
          return -1;
        }

        funcs_loaded = true;
        return 0;
      }
    }  // namespace fix

    static int init() {
      static void *handle {nullptr};
      static bool funcs_loaded = false;

      if (funcs_loaded) {
        return 0;
      }

      if (!handle) {
        handle = dyn::handle({"libX11.so.6", "libX11.so"});
        if (!handle) {
          return -1;
        }
      }

      std::vector<std::tuple<dyn::apiproc *, const char *>> funcs {
        {(dyn::apiproc *) &OpenDisplay, "XOpenDisplay"},
        {(dyn::apiproc *) &GetWindowAttributes, "XGetWindowAttributes"},
        {(dyn::apiproc *) &QueryPointer, "XQueryPointer"},
        {(dyn::apiproc *) &Pending, "XPending"},
        {(dyn::apiproc *) &NextEvent, "XNextEvent"},
        {(dyn::apiproc *) &Flush, "XFlush"},
        {(dyn::apiproc *) &CreateSimpleWindow, "XCreateSimpleWindow"},
        {(dyn::apiproc *) &ChangeWindowAttributes, "XChangeWindowAttributes"},
        {(dyn::apiproc *) &InternAtom, "XInternAtom"},
        {(dyn::apiproc *) &ChangeProperty, "XChangeProperty"},
        {(dyn::apiproc *) &MapRaised, "XMapRaised"},
        {(dyn::apiproc *) &DestroyWindow, "XDestroyWindow"},
        {(dyn::apiproc *) &Sync, "XSync"},
        {(dyn::apiproc *) &Free, "XFree"},
        {(dyn::apiproc *) &CloseDisplay, "XCloseDisplay"},
        {(dyn::apiproc *) &InitThreads, "XInitThreads"},
        {(dyn::apiproc *) &SetIOErrorHandler, "XSetIOErrorHandler"},
      };

      if (dyn::load(handle, funcs)) {
        return -1;
      }

      // Install before opening a display or starting capture/cursor threads.
      // A dead X server invalidates this worker's entire graphical generation.
      SetIOErrorHandler(retire_failed_x11_worker);
      funcs_loaded = true;
      return 0;
    }
  }  // namespace x11

  namespace xcb {
    static xcb_extension_t *shm_id;

    _FN(shm_get_image_reply, xcb_shm_get_image_reply_t *, (xcb_connection_t * c, xcb_shm_get_image_cookie_t cookie, xcb_generic_error_t **e));

    _FN(shm_get_image_unchecked, xcb_shm_get_image_cookie_t, (xcb_connection_t * c, xcb_drawable_t drawable, int16_t x, int16_t y, uint16_t width, uint16_t height, uint32_t plane_mask, uint8_t format, xcb_shm_seg_t shmseg, uint32_t offset));

    _FN(shm_attach_checked, xcb_void_cookie_t, (xcb_connection_t * c, xcb_shm_seg_t shmseg, uint32_t shmid, uint8_t read_only));
    _FN(shm_detach, xcb_void_cookie_t, (xcb_connection_t * c, xcb_shm_seg_t shmseg));

    _FN(get_extension_data, xcb_query_extension_reply_t *, (xcb_connection_t * c, xcb_extension_t *ext));

    _FN(disconnect, void, (xcb_connection_t * c));
    _FN(connection_has_error, int, (xcb_connection_t * c));
    _FN(connect, xcb_connection_t *, (const char *displayname, int *screenp));
    _FN(generate_id, std::uint32_t, (xcb_connection_t * c));
    _FN(request_check, xcb_generic_error_t *, (xcb_connection_t * c, xcb_void_cookie_t cookie));
    _FN(flush, int, (xcb_connection_t * c));

    /**
     * @brief Initialize shared-memory support for X11 capture.
     *
     * @return 0 when XCB shared-memory functions are loaded; nonzero otherwise.
     */
    int init_shm() {
      static void *handle {nullptr};
      static bool funcs_loaded = false;

      if (funcs_loaded) {
        return 0;
      }

      if (!handle) {
        handle = dyn::handle({"libxcb-shm.so.0", "libxcb-shm.so"});
        if (!handle) {
          return -1;
        }
      }

      std::vector<std::tuple<dyn::apiproc *, const char *>> funcs {
        {(dyn::apiproc *) &shm_id, "xcb_shm_id"},
        {(dyn::apiproc *) &shm_get_image_reply, "xcb_shm_get_image_reply"},
        {(dyn::apiproc *) &shm_get_image_unchecked, "xcb_shm_get_image_unchecked"},
        {(dyn::apiproc *) &shm_attach_checked, "xcb_shm_attach_checked"},
        {(dyn::apiproc *) &shm_detach, "xcb_shm_detach"},
      };

      if (dyn::load(handle, funcs)) {
        return -1;
      }

      funcs_loaded = true;
      return 0;
    }

    /**
     * @brief Initialize XFixes cursor tracking for an X11 display.
     *
     * @return 0 on success; nonzero or negative platform status on failure.
     */
    int init() {
      static void *handle {nullptr};
      static bool funcs_loaded = false;

      if (funcs_loaded) {
        return 0;
      }

      if (!handle) {
        handle = dyn::handle({"libxcb.so.1", "libxcb.so"});
        if (!handle) {
          return -1;
        }
      }

      std::vector<std::tuple<dyn::apiproc *, const char *>> funcs {
        {(dyn::apiproc *) &get_extension_data, "xcb_get_extension_data"},
        {(dyn::apiproc *) &disconnect, "xcb_disconnect"},
        {(dyn::apiproc *) &connection_has_error, "xcb_connection_has_error"},
        {(dyn::apiproc *) &connect, "xcb_connect"},
        {(dyn::apiproc *) &generate_id, "xcb_generate_id"},
        {(dyn::apiproc *) &request_check, "xcb_request_check"},
        {(dyn::apiproc *) &flush, "xcb_flush"},
      };

      if (dyn::load(handle, funcs)) {
        return -1;
      }

      funcs_loaded = true;
      return 0;
    }

#undef _FN
  }  // namespace xcb

  /**
   * @brief Release x resources.
   *
   * @param p Pointer passed to the deleter or conversion helper.
   */
  void freeX(XFixesCursorImage *);

  /**
   * @brief XCB connection pointer released with `xcb_disconnect`.
   */
  using xcb_connect_t = util::dyn_safe_ptr<xcb_connection_t, &xcb::disconnect>;
  /**
   * @brief XCB image pointer released with `xcb_image_destroy`.
   */
  using xcb_img_t = util::c_ptr<xcb_shm_get_image_reply_t>;

  /**
   * @brief XFixes cursor image pointer released with `XFree`.
   */
  using xcursor_t = util::safe_ptr<XFixesCursorImage, freeX>;

  /**
   * @brief XRandR CRTC info pointer released with `XRRFreeCrtcInfo`.
   */
  using crtc_info_t = util::dyn_safe_ptr<_XRRCrtcInfo, &x11::rr::FreeCrtcInfo>;
  /**
   * @brief XRandR output info pointer released with `XRRFreeOutputInfo`.
   */
  using output_info_t = util::dyn_safe_ptr<_XRROutputInfo, &x11::rr::FreeOutputInfo>;
  /**
   * @brief XRandR screen resources pointer released with `XRRFreeScreenResources`.
   */
  using screen_res_t = util::dyn_safe_ptr<_XRRScreenResources, &x11::rr::FreeScreenResources>;

  /**
   * @brief Shared XCB connection used by native depth-30 capture images.
   *
   * Images retain this object so their SHM detach requests always run before
   * the connection is closed, even when an encode-stage reference outlives the
   * display capture loop.
   */
  struct native10_connection_t {
    native10_connection_t(xcb_connection_t *connection, uid_t server_uid, gid_t server_gid):
        connection {connection},
        server_uid {server_uid},
        server_gid {server_gid} {
    }

    xcb_connect_t connection;  ///< XCB connection that owns all attached segments.
    std::mutex mutex;  ///< Serializes attach, capture, and detach requests.
    uid_t server_uid;  ///< Authenticated X server account allowed to write SHM.
    gid_t server_gid;  ///< Authenticated X server group allowed to write SHM.
  };

  /**
   * @brief One reusable XShm image in the native depth-30 capture ring.
   */
  struct native10_img_t: public img_t {
    explicit native10_img_t(std::shared_ptr<native10_connection_t> connection):
        connection {std::move(connection)} {
    }

    ~native10_img_t() override {
      if (connection && connection->connection && segment != 0) {
        std::lock_guard lock {connection->mutex};
        xcb::shm_detach(connection->connection.get(), segment);
        xcb::flush(connection->connection.get());
      }
      if ((std::uintptr_t) mapping != -1) {
        shmdt(mapping);
      }
      if (shm_id >= 0 && !marked_for_removal) {
        shmctl(shm_id, IPC_RMID, nullptr);
      }
      data = nullptr;
    }

    std::shared_ptr<native10_connection_t> connection;  ///< Connection kept alive by this image.
    xcb_shm_seg_t segment {};  ///< XCB-side segment identifier.
    int shm_id {-1};  ///< SysV shared-memory identifier.
    void *mapping {(void *) -1};  ///< Process mapping returned by shmat().
    bool marked_for_removal {false};  ///< Whether IPC_RMID has already been issued.
  };

  /**
   * @brief Convert packed X11 RGB 10:10:10 directly into planar identity GBR.
   *
   * Both the XShm source and libx264 destination reside in system memory, so a
   * CUDA upload followed by a larger planar readback would only add transfers.
   */
  class native10_software_t final: public avcodec_encode_device_t {
  public:
    native10_software_t(int width, int height, pix_fmt_e requested_format):
        source_width {width},
        source_height {height},
        requested_format {requested_format} {
      data = reinterpret_cast<void *>(0x1);
    }

    ~native10_software_t() override {
      for (auto &worker : workers) {
        worker.request_stop();
      }
      work_ready.notify_all();
      workers.clear();
    }

    int set_frame(AVFrame *new_frame, AVBufferRef *hw_frames_ctx) override {
      host_frame.reset(new_frame);
      frame = new_frame;
      if (hw_frames_ctx || !frame || requested_format != pix_fmt_e::yuv444p16 ||
          frame->format != AV_PIX_FMT_YUV444P10LE) {
        BOOST_LOG(error) << "Native X11 10-bit capture requires planar H.264 10-bit 4:4:4 software input"sv;
        return -1;
      }
      output_width = frame->width;
      output_height = frame->height;
      if (av_frame_get_buffer(frame, 64) < 0) {
        BOOST_LOG(error) << "Couldn't allocate native X11 10-bit software frame"sv;
        return -1;
      }
      if (source_width != output_width || source_height != output_height) {
        x_samples = make_bilinear_axis_samples(source_width, output_width);
        y_samples = make_bilinear_axis_samples(source_height, output_height);
        BOOST_LOG(info) << "Native X11 x264 10-bit scaling: "sv
                        << source_width << 'x' << source_height << " -> "sv
                        << output_width << 'x' << output_height;
      }
      start_workers();
      BOOST_LOG(info)
        << "Native X11 x264 identity input: packed RGB 10:10:10 -> planar GBR 10-bit, workers="sv
        << workers.size() + 1U;
      return 0;
    }

    int convert(platf::img_t &img) override {
      if (!frame || !img.data || img.width != source_width || img.height != source_height ||
          img.pixel_pitch != 4 || img.row_pitch < source_width * 4 ||
          colorspace.colorspace != video::colorspace_e::identity_gbr ||
          colorspace.bit_depth != 10) {
        BOOST_LOG(error) << "Native X11 10-bit capture received an incompatible frame or colorspace"sv;
        return -1;
      }

      source = img.data;
      source_pitch = img.row_pitch;
      const auto worker_count = static_cast<unsigned int>(workers.size());
      {
        std::lock_guard lock {work_mutex};
        workers_pending = worker_count;
        ++generation;
      }
      work_ready.notify_all();
      unpack_rows(worker_count, worker_count + 1U);

      if (worker_count != 0) {
        std::unique_lock lock {work_mutex};
        work_done.wait(lock, [&]() {
          return workers_pending == 0;
        });
      }
      return 0;
    }

  private:
    void start_workers() {
      if (!workers.empty()) {
        return;
      }
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

            unpack_rows(index, worker_count + 1U);
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

    void unpack_rows(unsigned int share_index, unsigned int share_count) {
      if (!x_samples.empty()) {
        scale_xrgb10_to_gbr10_rows(
          source, static_cast<std::size_t>(source_pitch), x_samples, y_samples,
          frame->data[0], static_cast<std::size_t>(frame->linesize[0]),
          frame->data[1], static_cast<std::size_t>(frame->linesize[1]),
          frame->data[2], static_cast<std::size_t>(frame->linesize[2]),
          share_index, share_count);
        return;
      }
      for (int y = static_cast<int>(share_index); y < output_height;
           y += static_cast<int>(share_count)) {
        const auto *source_row = reinterpret_cast<const std::uint32_t *>(
          source + static_cast<std::size_t>(y) * source_pitch);
        auto *green = reinterpret_cast<std::uint16_t *>(
          frame->data[0] + static_cast<std::size_t>(y) * frame->linesize[0]);
        auto *blue = reinterpret_cast<std::uint16_t *>(
          frame->data[1] + static_cast<std::size_t>(y) * frame->linesize[1]);
        auto *red = reinterpret_cast<std::uint16_t *>(
          frame->data[2] + static_cast<std::size_t>(y) * frame->linesize[2]);
        x11::unpack_xrgb10_to_gbr10_row(
          source_row, green, blue, red, static_cast<std::size_t>(output_width));
      }
    }

    frame_t host_frame;  ///< Owned FFmpeg software frame.
    std::vector<std::jthread> workers;  ///< Persistent packed-to-planar workers.
    std::mutex work_mutex;  ///< Protects generation and completion state.
    std::condition_variable_any work_ready;  ///< Signals a new source frame.
    std::condition_variable work_done;  ///< Signals completion of all shares.
    std::uint64_t generation {0};  ///< Current conversion generation.
    unsigned int workers_pending {0};  ///< Worker shares still active.
    const std::uint8_t *source {nullptr};  ///< Current packed XShm source.
    int source_pitch {0};  ///< Current packed source row stride.
    std::vector<bilinear_axis_sample_t> x_samples;  ///< Horizontal scaling map, empty for 1:1.
    std::vector<bilinear_axis_sample_t> y_samples;  ///< Vertical scaling map, empty for 1:1.
    int source_width;  ///< Packed XShm source width.
    int source_height;  ///< Packed XShm source height.
    int output_width {0};  ///< Planar x264 destination width.
    int output_height {0};  ///< Planar x264 destination height.
    pix_fmt_e requested_format;  ///< Negotiated PLANK format.
  };

  static void blend_cursor(Display *display, img_t &img, int offsetX, int offsetY) {
    xcursor_t overlay {x11::fix::GetCursorImage(display)};

    if (!overlay) {
      BOOST_LOG(error) << "Couldn't get cursor from XFixesGetCursorImage"sv;
      return;
    }

    overlay->x -= overlay->xhot;
    overlay->y -= overlay->yhot;

    overlay->x -= offsetX;
    overlay->y -= offsetY;

    overlay->x = std::max((short) 0, overlay->x);
    overlay->y = std::max((short) 0, overlay->y);

    auto pixels = (int *) img.data;

    auto screen_height = img.height;
    auto screen_width = img.width;

    auto delta_height = std::min<uint16_t>(overlay->height, std::max(0, screen_height - overlay->y));
    auto delta_width = std::min<uint16_t>(overlay->width, std::max(0, screen_width - overlay->x));
    for (auto y = 0; y < delta_height; ++y) {
      auto overlay_begin = &overlay->pixels[y * overlay->width];
      auto overlay_end = &overlay->pixels[y * overlay->width + delta_width];

      auto pixels_begin = &pixels[(y + overlay->y) * (img.row_pitch / img.pixel_pitch) + overlay->x];

      std::for_each(overlay_begin, overlay_end, [&](long pixel) {
        int *pixel_p = (int *) &pixel;

        auto colors_in = (uint8_t *) pixels_begin;

        auto alpha = (*(uint *) pixel_p) >> 24u;
        if (alpha == 255) {
          *pixels_begin = *pixel_p;
        } else {
          auto colors_out = (uint8_t *) pixel_p;
          colors_in[0] = colors_out[0] + (colors_in[0] * (255 - alpha) + 255 / 2) / 255;
          colors_in[1] = colors_out[1] + (colors_in[1] * (255 - alpha) + 255 / 2) / 255;
          colors_in[2] = colors_out[2] + (colors_in[2] * (255 - alpha) + 255 / 2) / 255;
        }
        ++pixels_begin;
      });
    }
  }

  /**
   * @brief Blend an XFixes ARGB cursor into packed X11 RGB 10:10:10 pixels.
   */
  static void blend_cursor_native10(Display *display, img_t &img, int offsetX, int offsetY) {
    xcursor_t cursor {x11::fix::GetCursorImage(display)};
    if (!cursor) {
      BOOST_LOG(error) << "Couldn't get cursor for native X11 10-bit capture"sv;
      return;
    }

    const int cursor_x = static_cast<int>(cursor->x) - cursor->xhot - offsetX;
    const int cursor_y = static_cast<int>(cursor->y) - cursor->yhot - offsetY;
    const int first_x = std::max(0, cursor_x);
    const int first_y = std::max(0, cursor_y);
    const int end_x = std::min(img.width, cursor_x + static_cast<int>(cursor->width));
    const int end_y = std::min(img.height, cursor_y + static_cast<int>(cursor->height));
    if (first_x >= end_x || first_y >= end_y) {
      return;
    }

    for (int y = first_y; y < end_y; ++y) {
      auto *destination = reinterpret_cast<std::uint32_t *>(
        img.data + static_cast<std::size_t>(y) * img.row_pitch);
      const int cursor_row = y - cursor_y;
      for (int x = first_x; x < end_x; ++x) {
        const int cursor_column = x - cursor_x;
        const std::uint32_t argb = static_cast<std::uint32_t>(
          cursor->pixels[static_cast<std::size_t>(cursor_row) * cursor->width + cursor_column]);
        const unsigned int alpha = argb >> 24U;
        if (alpha == 0) {
          continue;
        }

        const std::uint32_t background = destination[x];
        const auto blend_channel = [alpha](unsigned int foreground,
                                           unsigned int background_channel) {
          const unsigned int foreground_10 = video::expand_8bit_to_10bit(
            static_cast<std::uint8_t>(foreground));
          return (foreground_10 * alpha +
                  background_channel * (255U - alpha) + 127U) / 255U;
        };
        const unsigned int red = blend_channel((argb >> 16U) & 0xffU,
                                                background & 0x3ffU);
        const unsigned int green = blend_channel((argb >> 8U) & 0xffU,
                                                  (background >> 10U) & 0x3ffU);
        const unsigned int blue = blend_channel(argb & 0xffU,
                                                 (background >> 20U) & 0x3ffU);
        destination[x] = red | (green << 10U) | (blue << 20U);
      }
    }
  }

  /**
   * @brief X11 display, window, and attribute handles for capture.
   */
  struct x11_attr_t: public display_t {
    std::chrono::nanoseconds delay;  ///< Delay before the timer task becomes eligible to run.

    x11::xdisplay_t xdisplay;  ///< X11 display connection used for capture.
    Window xwindow;  ///< Root window being captured.
    XWindowAttributes xattr;  ///< Cached X11 window attributes used to detect size changes.

    mem_type_e mem_type;  ///< Mem type.

    /**
     * Last X (NOT the streamed monitor!) size.
     * This way we can trigger reinitialization if the dimensions changed while streaming
     */
    // int env_width, env_height;

    /**
     * @brief Open the X11 display and initialize capture attributes.
     *
     * @param mem_type Requested memory path for the capture backend.
     */
    x11_attr_t(mem_type_e mem_type):
        xdisplay {x11::OpenDisplay(nullptr)},
        xwindow {},
        xattr {},
        mem_type {mem_type} {
      x11::InitThreads();
    }

    /**
     * @brief Open the X11 display and cache capture window attributes.
     *
     * @param display_name Display name.
     * @param config Configuration values to apply.
     * @return 0 on success; nonzero or negative platform status on failure.
     */
    int init(const std::string &display_name, const ::video::config_t &config) {
      if (!xdisplay) {
        BOOST_LOG(error) << "Could not open X11 display"sv;
        return -1;
      }

      delay = ::video::capture_frame_interval(config);

      xwindow = DefaultRootWindow(xdisplay.get());

      refresh();

      int streamedMonitor = -1;
      if (!display_name.empty() && std::ranges::all_of(display_name, ::isdigit)) {
        // Resolve (legacy) monitor index from display_name
        streamedMonitor = (int) util::from_view(display_name);
      }

      screen_res_t screenr {x11::rr::GetScreenResources(xdisplay.get(), xwindow)};
      int output = screenr->noutput;

      output_info_t result;
      bool result_found = false;
      int monitor = 0;
      for (int x = 0; x < output; ++x) {
        output_info_t out_info {x11::rr::GetOutputInfo(xdisplay.get(), screenr.get(), screenr->outputs[x])};
        if (out_info) {
          // Match monitor by index if a valid one is present, otherwise try to resolve by matching output name to display_name
          if ((streamedMonitor >= 0 && monitor == streamedMonitor) || (streamedMonitor < 0 && out_info->name == display_name)) {
            result = std::move(out_info);
            result_found = true;
            break;
          }
          monitor++;
        }
      }

      if (result_found && result->crtc) {
        crtc_info_t crt_info {x11::rr::GetCrtcInfo(xdisplay.get(), screenr.get(), result->crtc)};
        BOOST_LOG(info)
          << "Streaming display: "sv << result->name << " with res "sv << crt_info->width << 'x' << crt_info->height << " offset by "sv << crt_info->x << 'x' << crt_info->y;

        width = crt_info->width;
        height = crt_info->height;
        offset_x = crt_info->x;
        offset_y = crt_info->y;
      } else {
        BOOST_LOG(warning) << "Couldn't get info for requested display ["sv << display_name << "], defaulting to recording entire virtual desktop"sv;
        width = xattr.width;
        height = xattr.height;
      }

      env_width = xattr.width;
      env_height = xattr.height;

      return 0;
    }

    /**
     * Called when the display attributes should change.
     */
    void refresh() {
      x11::GetWindowAttributes(xdisplay.get(), xwindow, &xattr);  // Update xattr's
    }

    /**
     * @brief Create AVCodec encode device.
     *
     * @param pix_fmt Sunshine pixel format to convert or allocate for.
     * @return Constructed AVCodec encode device object.
     */
    std::unique_ptr<avcodec_encode_device_t> make_avcodec_encode_device(pix_fmt_e pix_fmt) override {

#ifdef SUNSHINE_BUILD_CUDA
      if (mem_type == mem_type_e::cuda) {
        return cuda::make_avcodec_encode_device(width, height, false);
      }
#endif

      return std::make_unique<avcodec_encode_device_t>();
    }

  };

  /**
   * @brief Native depth-30 XComposite capture with per-pipeline-image SHM.
   */
  struct native10_attr_t final: public x11_attr_t {
    explicit native10_attr_t(mem_type_e mem_type):
        x11_attr_t(mem_type) {
    }

    ~native10_attr_t() override {
      if (xdisplay && compositor_keepalive != 0) {
        x11::DestroyWindow(xdisplay.get(), compositor_keepalive);
        compositor_keepalive = 0;
      }
      if (xdisplay && overlay_window != 0) {
        x11::composite::ReleaseOverlayWindow(xdisplay.get(), xwindow);
        overlay_window = 0;
      }
      if (xdisplay) {
        x11::Sync(xdisplay.get(), False);
      }
    }

    int init(const std::string &display_name, const ::video::config_t &config) {
      if (x11_attr_t::init(display_name, config)) {
        return 1;
      }
      if (ImageByteOrder(xdisplay.get()) != LSBFirst || xattr.depth != 30 ||
          xattr.visual->red_mask != 0x3ffUL ||
          xattr.visual->green_mask != 0xffc00UL ||
          xattr.visual->blue_mask != 0x3ff00000UL) {
        BOOST_LOG(error)
          << "Native X11 10-bit capture requires LSB-first depth-30 RGB 10:10:10; root depth="sv
          << xattr.depth << " masks="sv << util::hex(xattr.visual->red_mask).to_string_view()
          << ',' << util::hex(xattr.visual->green_mask).to_string_view()
          << ',' << util::hex(xattr.visual->blue_mask).to_string_view();
        return -1;
      }

      overlay_window = x11::composite::GetOverlayWindow(xdisplay.get(), xwindow);
      if (overlay_window == 0 ||
          !x11::GetWindowAttributes(xdisplay.get(), overlay_window, &overlay_attr) ||
          overlay_attr.width != env_width || overlay_attr.height != env_height ||
          overlay_attr.depth != 30 ||
          overlay_attr.visual->red_mask != xattr.visual->red_mask ||
          overlay_attr.visual->green_mask != xattr.visual->green_mask ||
          overlay_attr.visual->blue_mask != xattr.visual->blue_mask) {
        BOOST_LOG(error) << "Native X11 10-bit capture found no matching depth-30 XComposite overlay"sv;
        return -1;
      }

      // A fully transparent, input-empty override window keeps Mutter from
      // unredirecting a fullscreen Flame surface. Without it the XComposite
      // overlay becomes black while final scanout continues through NvFBC.
      compositor_keepalive = x11::CreateSimpleWindow(
        xdisplay.get(), xwindow, 0, 0, 2, 2, 0, 0, 0);
      if (compositor_keepalive == 0) {
        BOOST_LOG(error) << "Couldn't create native X11 compositor keepalive"sv;
        return -1;
      }
      XSetWindowAttributes keepalive_attr {};
      keepalive_attr.override_redirect = True;
      x11::ChangeWindowAttributes(
        xdisplay.get(), compositor_keepalive, CWOverrideRedirect, &keepalive_attr);
      const Atom opacity_atom = x11::InternAtom(
        xdisplay.get(), "_NET_WM_WINDOW_OPACITY", False);
      const unsigned long opacity = 0;
      x11::ChangeProperty(
        xdisplay.get(), compositor_keepalive, opacity_atom, XA_CARDINAL, 32,
        PropModeReplace, reinterpret_cast<const unsigned char *>(&opacity), 1);
      x11::shape::CombineRectangles(
        xdisplay.get(), compositor_keepalive, ShapeInput, 0, 0,
        nullptr, 0, ShapeSet, Unsorted);
      x11::MapRaised(xdisplay.get(), compositor_keepalive);
      x11::Sync(xdisplay.get(), False);

      xcb_connection_t *raw_connection = xcb::connect(nullptr, nullptr);
      if (!raw_connection || xcb::connection_has_error(raw_connection)) {
        if (raw_connection) {
          xcb::disconnect(raw_connection);
        }
        BOOST_LOG(error) << "Couldn't create native X11 XCB connection"sv;
        return -1;
      }
      uid_t xserver_uid = geteuid();
      gid_t xserver_gid = getegid();
      if (geteuid() == 0) {
        const char *xauthority = std::getenv("XAUTHORITY");
        struct stat authority_stat {};
        if (!xauthority || stat(xauthority, &authority_stat) != 0) {
          xcb::disconnect(raw_connection);
          BOOST_LOG(error)
            << "Native X11 capture cannot identify the authenticated X server account from XAUTHORITY"sv;
          return -1;
        }
        xserver_uid = authority_stat.st_uid;
        xserver_gid = authority_stat.st_gid;
      }
      connection = std::make_shared<native10_connection_t>(
        raw_connection, xserver_uid, xserver_gid);
      const auto *shm_extension = xcb::get_extension_data(
        connection->connection.get(), xcb::shm_id);
      if (!shm_extension || !shm_extension->present) {
        BOOST_LOG(error) << "Native X11 10-bit capture requires MIT-SHM"sv;
        connection.reset();
        return -1;
      }

      BOOST_LOG(info)
        << "Native X11 10-bit source verified: XComposite overlay="sv
        << env_width << 'x' << env_height
        << " capture="sv << width << 'x' << height
        << '+' << offset_x << '+' << offset_y
        << " depth=30 storage=32bpp masks=0x000003ff/0x000ffc00/0x3ff00000 X-server-uid="sv
        << connection->server_uid;
      return 0;
    }

    capture_e capture(const push_captured_image_cb_t &push_captured_image_cb,
                      const pull_free_image_cb_t &pull_free_image_cb,
                      bool *cursor) override {
      auto next_frame = std::chrono::steady_clock::now();
      sleep_overshoot_logger.reset();
      while (true) {
        const auto now = std::chrono::steady_clock::now();
        if (next_frame > now) {
          std::this_thread::sleep_for(next_frame - now);
          sleep_overshoot_logger.first_point(next_frame);
          sleep_overshoot_logger.second_point_now_and_log();
        }
        next_frame += delay;
        if (next_frame < now) {
          next_frame = now + delay;
        }

        std::shared_ptr<platf::img_t> img_out;
        const auto status = snapshot(pull_free_image_cb, img_out, *cursor);
        switch (status) {
          case capture_e::reinit:
          case capture_e::error:
          case capture_e::interrupted:
            return status;
          case capture_e::timeout:
            if (!push_captured_image_cb(std::move(img_out), false)) {
              return capture_e::ok;
            }
            break;
          case capture_e::ok:
            if (!push_captured_image_cb(std::move(img_out), true)) {
              return capture_e::ok;
            }
            break;
          default:
            BOOST_LOG(error) << "Unrecognized native X11 capture status ["sv
                             << static_cast<int>(status) << ']';
            return status;
        }
      }
    }

    capture_e snapshot(const pull_free_image_cb_t &pull_free_image_cb,
                       std::shared_ptr<platf::img_t> &img_out,
                       bool cursor) {
      refresh();
      XWindowAttributes current_overlay {};
      if (xattr.width != env_width || xattr.height != env_height ||
          !x11::GetWindowAttributes(xdisplay.get(), overlay_window, &current_overlay) ||
          current_overlay.width != env_width || current_overlay.height != env_height ||
          current_overlay.depth != 30) {
        BOOST_LOG(warning) << "Native X11 capture topology changed; requesting reinitialization"sv;
        return capture_e::reinit;
      }
      if (!pull_free_image_cb(img_out)) {
        return capture_e::interrupted;
      }
      auto *img = dynamic_cast<native10_img_t *>(img_out.get());
      if (!img || img->connection != connection) {
        BOOST_LOG(error) << "Native X11 capture received an incompatible image buffer"sv;
        return capture_e::error;
      }

      xcb_img_t reply;
      const auto frame_timestamp = std::chrono::steady_clock::now();
      {
        std::lock_guard lock {connection->mutex};
        const auto cookie = xcb::shm_get_image_unchecked(
          connection->connection.get(), static_cast<xcb_drawable_t>(overlay_window),
          offset_x, offset_y, width, height, ~0U, XCB_IMAGE_FORMAT_Z_PIXMAP,
          img->segment, 0);
        reply.reset(xcb::shm_get_image_reply(
          connection->connection.get(), cookie, nullptr));
      }
      if (!reply || reply->depth != 30 || reply->size != frame_size()) {
        BOOST_LOG(error) << "Native X11 XShmGetImage returned an invalid depth or size"sv;
        return capture_e::reinit;
      }
      img->frame_timestamp = frame_timestamp;
      if (cursor) {
        blend_cursor_native10(xdisplay.get(), *img, offset_x, offset_y);
      }
      return capture_e::ok;
    }

    std::shared_ptr<img_t> alloc_img() override {
      if (!connection || !connection->connection) {
        return nullptr;
      }
      auto img = std::make_shared<native10_img_t>(connection);
      img->width = width;
      img->height = height;
      img->pixel_pitch = 4;
      img->row_pitch = width * 4;
      img->shm_id = shmget(IPC_PRIVATE, frame_size(), IPC_CREAT | 0600);
      if (img->shm_id < 0) {
        BOOST_LOG(error) << "Native X11 shmget failed: "sv << strerror(errno);
        return nullptr;
      }
      struct shmid_ds segment_info {};
      if (shmctl(img->shm_id, IPC_STAT, &segment_info) != 0) {
        BOOST_LOG(error) << "Native X11 shmctl(IPC_STAT) failed: "sv << strerror(errno);
        return nullptr;
      }
      segment_info.shm_perm.uid = connection->server_uid;
      segment_info.shm_perm.gid = connection->server_gid;
      segment_info.shm_perm.mode = 0600;
      if (shmctl(img->shm_id, IPC_SET, &segment_info) != 0) {
        BOOST_LOG(error) << "Native X11 shmctl(IPC_SET) failed: "sv << strerror(errno);
        return nullptr;
      }
      img->mapping = shmat(img->shm_id, nullptr, 0);
      if ((std::uintptr_t) img->mapping == -1) {
        BOOST_LOG(error) << "Native X11 shmat failed: "sv << strerror(errno);
        return nullptr;
      }
      img->data = static_cast<std::uint8_t *>(img->mapping);
      {
        std::lock_guard lock {connection->mutex};
        img->segment = xcb::generate_id(connection->connection.get());
        const auto attach_cookie = xcb::shm_attach_checked(
          connection->connection.get(), img->segment, img->shm_id, false);
        xcb_generic_error_t *xcb_error = xcb::request_check(
          connection->connection.get(), attach_cookie);
        if (xcb_error) {
          BOOST_LOG(error) << "Native X11 SHM attach failed with X11 error "sv
                           << static_cast<unsigned int>(xcb_error->error_code);
          std::free(xcb_error);
          return nullptr;
        }
      }
      if (shmctl(img->shm_id, IPC_RMID, nullptr) != 0) {
        BOOST_LOG(error) << "Native X11 shmctl(IPC_RMID) failed: "sv << strerror(errno);
        return nullptr;
      }
      img->marked_for_removal = true;
      return img;
    }

    int dummy_img(img_t *img) override {
      if (!img) {
        return -1;
      }
      auto pull_dummy_img_callback = [&img](std::shared_ptr<platf::img_t> &img_out) {
        img_out = img->shared_from_this();
        return true;
      };
      std::shared_ptr<platf::img_t> img_out;
      return snapshot(pull_dummy_img_callback, img_out, false) == capture_e::ok ? 0 : -1;
    }

    std::unique_ptr<avcodec_encode_device_t> make_avcodec_software_encode_device(
        pix_fmt_e pix_fmt) override {
      if (pix_fmt != pix_fmt_e::yuv444p16) {
        BOOST_LOG(error) << "Native X11 10-bit capture currently supports only H.264 10-bit 4:4:4 identity"sv;
        return nullptr;
      }
      return std::make_unique<native10_software_t>(width, height, pix_fmt);
    }

    std::unique_ptr<nvenc_encode_device_t> make_nvenc_encode_device(
        pix_fmt_e pix_fmt) override {
      if (pix_fmt != pix_fmt_e::yuv444p16) {
        BOOST_LOG(error) << "Native X11 direct NVENC supports only 10-bit 4:4:4 identity"sv;
        return nullptr;
      }
      return cuda::make_nvenc_native10_encode_device(width, height, pix_fmt);
    }

    std::uint32_t frame_size() const {
      return static_cast<std::uint32_t>(width) *
             static_cast<std::uint32_t>(height) * 4U;
    }

    Window overlay_window {};  ///< Depth-30 XComposite overlay being captured.
    Window compositor_keepalive {};  ///< Transparent window preventing unredirect.
    XWindowAttributes overlay_attr {};  ///< Verified overlay geometry and format.
    std::shared_ptr<native10_connection_t> connection;  ///< SHM capture connection.
  };

  /**
   * @brief Create an X11 display capture backend.
   *
   * @param hwdevice_type Hardware device type requested for capture or encode.
   * @param display_name Display name.
   * @param config Configuration values to apply.
   * @return X11 display backend, or nullptr when initialization fails.
   */
  std::shared_ptr<display_t> x11_display(platf::mem_type_e hwdevice_type, const std::string &display_name, const ::video::config_t &config) {
    if (hwdevice_type != platf::mem_type_e::system && hwdevice_type != platf::mem_type_e::cuda) {
      BOOST_LOG(error) << "Could not initialize x11 display with the given hw device type"sv;
      return nullptr;
    }

    if (xcb::init_shm() || xcb::init() || x11::init() || x11::rr::init() || x11::fix::init()) {
      BOOST_LOG(error) << "Couldn't init x11 libraries"sv;

      return nullptr;
    }

    if (config.capture_source != ::video::capture_source_e::x11_native10) {
      BOOST_LOG(error) << "Rejecting unsupported generic X11 capture source"sv;
      return nullptr;
    }

    if (x11::composite::init() || x11::shape::init()) {
      BOOST_LOG(error) << "Couldn't load XComposite/XShape for native 10-bit capture"sv;
      return nullptr;
    }
    auto native_display = std::make_shared<native10_attr_t>(hwdevice_type);
    if (native_display->init(display_name, config)) {
      return nullptr;
    }
    return native_display;
  }

  /**
   * @brief Enumerate display names accepted by the X11 backend.
   *
   * @return X11 display names, or an empty list when X11 probing fails.
   */
  std::vector<std::string> x11_display_names() {
    if (load_x11() || load_xcb()) {
      BOOST_LOG(error) << "Couldn't init x11 libraries"sv;

      return {};
    }

    BOOST_LOG(info) << "Detecting displays"sv;

    x11::xdisplay_t xdisplay {x11::OpenDisplay(nullptr)};
    if (!xdisplay) {
      return {};
    }

    auto xwindow = DefaultRootWindow(xdisplay.get());
    screen_res_t screenr {x11::rr::GetScreenResources(xdisplay.get(), xwindow)};
    int output = screenr->noutput;

    std::vector<std::string> names;
    int monitor = 0;
    for (int x = 0; x < output; ++x) {
      output_info_t out_info {x11::rr::GetOutputInfo(xdisplay.get(), screenr.get(), screenr->outputs[x])};
      if (out_info) {
        BOOST_LOG(info) << "Detected display: "sv << out_info->name << " (id: "sv << monitor++ << ")"sv << out_info->name << " connected: "sv << (out_info->connection == RR_Connected);
        names.emplace_back(out_info->name);
      }
    }

    return names;
  }

  /**
   * @brief Enumerate connected XRandR outputs with stable connector identity.
   */
  std::vector<display_info_t> x11_display_infos() {
    if (load_x11() || load_xcb()) {
      return {};
    }

    x11::xdisplay_t xdisplay {x11::OpenDisplay(nullptr)};
    if (!xdisplay) {
      return {};
    }

    const auto root = DefaultRootWindow(xdisplay.get());
    screen_res_t resources {x11::rr::GetScreenResources(xdisplay.get(), root)};
    if (!resources) {
      return {};
    }
    const auto primary = x11::rr::GetOutputPrimary(xdisplay.get(), root);

    std::vector<display_info_t> outputs;
    for (int index = 0; index < resources->noutput; ++index) {
      const auto output_id = resources->outputs[index];
      output_info_t output {x11::rr::GetOutputInfo(xdisplay.get(), resources.get(), output_id)};
      if (!output || output->connection != RR_Connected || output->crtc == None) {
        continue;
      }
      crtc_info_t crtc {x11::rr::GetCrtcInfo(xdisplay.get(), resources.get(), output->crtc)};
      if (!crtc || crtc->width == 0 || crtc->height == 0) {
        continue;
      }

      std::string name {output->name, static_cast<std::size_t>(output->nameLen)};
      int rotation = 0;
      if (crtc->rotation & RR_Rotate_90) {
        rotation = 90;
      } else if (crtc->rotation & RR_Rotate_180) {
        rotation = 180;
      } else if (crtc->rotation & RR_Rotate_270) {
        rotation = 270;
      }

      int refresh_millihz = 0;
      const auto mode = std::find_if(resources->modes, resources->modes + resources->nmode,
                                     [crtc_mode = crtc->mode](const XRRModeInfo &candidate) {
                                       return candidate.id == crtc_mode;
                                     });
      if (mode != resources->modes + resources->nmode && mode->hTotal && mode->vTotal) {
        auto vertical_total = static_cast<double>(mode->vTotal);
        if (mode->modeFlags & RR_DoubleScan) {
          vertical_total *= 2.0;
        }
        refresh_millihz = static_cast<int>(std::lround(
          (static_cast<double>(mode->dotClock) * 1000.0) /
          (static_cast<double>(mode->hTotal) * vertical_total)
        ));
      }

      outputs.push_back(display_info_t {
        .id = "x11:"s + name,
        .name = name,
        .capture_name = name,
        .x = crtc->x,
        .y = crtc->y,
        .width = static_cast<int>(crtc->width),
        .height = static_cast<int>(crtc->height),
        .rotation = rotation,
        .refresh_millihz = refresh_millihz,
        .primary = output_id == primary,
      });
    }
    return outputs;
  }

  /**
   * @brief Release x resources.
   */
  void freeX(XFixesCursorImage *p) {
    x11::Free(p);
  }

  /**
   * @brief Load XCB entry points used by the X11 capture backend.
   */
  int load_xcb() {
    // This will be called once only
    static int xcb_status = xcb::init_shm() || xcb::init();

    return xcb_status;
  }

  /**
   * @brief Load X11 entry points used by the X11 capture backend.
   */
  int load_x11() {
    // This will be called once only
    static int x11_status =
      window_system == window_system_e::NONE ||
      x11::init() || x11::rr::init() || x11::fix::init();

    return x11_status;
  }

  namespace x11 {
    std::optional<cursor_t> cursor_t::make() {
      if (load_x11()) {
        return std::nullopt;
      }

      cursor_t cursor;

      cursor.ctx.reset((cursor_ctx_t::pointer) x11::OpenDisplay(nullptr));
      if (!cursor.ctx) {
        return std::nullopt;
      }

      return cursor;
    }

    bool cursor_t::capture(egl::cursor_t &img) {
      auto display = (xdisplay_t::pointer) ctx.get();

      if (img.desktop_width <= 0 || img.desktop_height <= 0) {
        XWindowAttributes root_attributes {};
        if (!x11::GetWindowAttributes(display, DefaultRootWindow(display), &root_attributes) ||
            root_attributes.width <= 0 || root_attributes.height <= 0) {
          return false;
        }
        img.desktop_width = root_attributes.width;
        img.desktop_height = root_attributes.height;
      }

      xcursor_t xcursor = fix::GetCursorImage(display);
      if (!xcursor) {
        return false;
      }

      if (img.serial != xcursor->cursor_serial) {
        auto buf_size = xcursor->width * xcursor->height * sizeof(int);

        if (img.buffer.size() < buf_size) {
          img.buffer.resize(buf_size);
        }

        std::transform(xcursor->pixels, xcursor->pixels + buf_size / 4, (int *) img.buffer.data(), [](long pixel) -> int {
          return pixel;
        });
      }

      img.data = img.buffer.data();
      img.width = img.src_w = xcursor->width;
      img.height = img.src_h = xcursor->height;
      img.hotspot_x = xcursor->xhot;
      img.hotspot_y = xcursor->yhot;
      img.x = xcursor->x - xcursor->xhot;
      img.y = xcursor->y - xcursor->yhot;
      img.pixel_pitch = 4;
      img.row_pitch = img.pixel_pitch * img.width;
      img.serial = xcursor->cursor_serial;
      img.visible = std::any_of(
        reinterpret_cast<const std::uint32_t *>(img.data),
        reinterpret_cast<const std::uint32_t *>(img.data) +
          static_cast<std::size_t>(img.width) * img.height,
        [](const std::uint32_t pixel) { return (pixel & 0xFF000000U) != 0; }
      );
      return true;
    }

    bool cursor_t::subscribe_shape_events() {
      auto display = (xdisplay_t::pointer) ctx.get();
      const auto root = DefaultRootWindow(display);
      XWindowAttributes root_attributes {};
      int error_base = 0;
      if (!x11::GetWindowAttributes(display, root, &root_attributes) ||
          root_attributes.width <= 0 || root_attributes.height <= 0 ||
          !fix::QueryExtension(display, &xfixes_event_base_, &error_base)) {
        return false;
      }

      desktop_width_ = root_attributes.width;
      desktop_height_ = root_attributes.height;
      fix::SelectCursorInput(display, root, XFixesDisplayCursorNotifyMask);
      x11::Flush(display);
      return true;
    }

    bool cursor_t::query_position(cursor_position_t &position) {
      if (desktop_width_ <= 0 || desktop_height_ <= 0) {
        return false;
      }

      auto display = (xdisplay_t::pointer) ctx.get();
      Window root_return = None;
      Window child_return = None;
      int root_x = 0;
      int root_y = 0;
      int window_x = 0;
      int window_y = 0;
      unsigned int mask = 0;
      if (!x11::QueryPointer(
            display, DefaultRootWindow(display), &root_return, &child_return,
            &root_x, &root_y, &window_x, &window_y, &mask)) {
        return false;
      }

      position = cursor_position_t {
        .x = root_x,
        .y = root_y,
        .desktop_width = desktop_width_,
        .desktop_height = desktop_height_,
      };
      return true;
    }

    int cursor_t::consume_shape_change() {
      if (xfixes_event_base_ < 0) {
        return -1;
      }

      auto display = (xdisplay_t::pointer) ctx.get();
      bool shape_changed = false;
      while (x11::Pending(display) > 0) {
        XEvent event {};
        x11::NextEvent(display, &event);
        if (event.type == xfixes_event_base_ + XFixesCursorNotify) {
          shape_changed = true;
        }
      }
      return shape_changed ? 1 : 0;
    }

    void cursor_t::blend(img_t &img, int offsetX, int offsetY) {
      blend_cursor((xdisplay_t::pointer) ctx.get(), img, offsetX, offsetY);
    }

    /**
     * @brief Open and initialize the display connection used for capture.
     */
    xdisplay_t make_display() {
      return OpenDisplay(nullptr);
    }

    /**
     * @brief Release display resources.
     */
    void freeDisplay(_XDisplay *xdisplay) {
      CloseDisplay(xdisplay);
    }

    /**
     * @brief Release cursor context resources.
     *
     * @param ctx Native context object used by the operation or callback.
     */
    void freeCursorCtx(cursor_ctx_t::pointer ctx) {
      CloseDisplay((xdisplay_t::pointer) ctx);
    }
  }  // namespace x11
}  // namespace platf
