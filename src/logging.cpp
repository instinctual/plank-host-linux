/**
 * @file src/logging.cpp
 * @brief Definitions for logging related functions.
 */
// standard includes
#include <algorithm>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <streambuf>
#include <utility>

// lib includes
#include <boost/core/null_deleter.hpp>
#include <boost/format.hpp>
#include <boost/log/attributes/clock.hpp>
#include <boost/log/common.hpp>
#include <boost/log/expressions.hpp>
#include <boost/log/sinks.hpp>
#include <boost/log/sources/severity_logger.hpp>
#include <boost/log/utility/exception_handler.hpp>

// local includes
#include "logging.h"

// conditional includes
#ifdef __ANDROID__
  #include <android/log.h>
#endif

extern "C" {
#include <libavutil/log.h>
}

using namespace std::literals;

namespace bl = boost::log;

namespace {
  /**
   * @brief File stream buffer that rotates a bounded log before a record would
   * exceed the configured size.
   */
  class rotating_file_streambuf: public std::streambuf {
  public:
    explicit rotating_file_streambuf(std::filesystem::path log_path):
        log_path_ {std::move(log_path)} {
      open_file();
    }

  protected:
    std::streamsize xsputn(const char *data, std::streamsize count) override {
      if (count <= 0) {
        return 0;
      }

      const auto byte_count = static_cast<std::uintmax_t>(count);
      const auto remaining_size = logging::max_log_file_size - std::min(current_size_, logging::max_log_file_size);
      if (current_size_ != 0 && byte_count > remaining_size) {
        rotate();
      }

      file_.write(data, count);
      if (!file_) {
        return 0;
      }

      current_size_ += byte_count;
      return count;
    }

    int_type overflow(int_type character) override {
      if (traits_type::eq_int_type(character, traits_type::eof())) {
        return traits_type::not_eof(character);
      }

      const auto value = traits_type::to_char_type(character);
      return xsputn(&value, 1) == 1 ? character : traits_type::eof();
    }

    int sync() override {
      file_.flush();
      return file_ ? 0 : -1;
    }

  private:
    void open_file() {
      std::error_code file_size_error;
      current_size_ = std::filesystem::file_size(log_path_, file_size_error);
      if (file_size_error) {
        current_size_ = 0;
      }

      file_.open(log_path_, std::ios::out | std::ios::app | std::ios::binary);
      if (!file_) {
        std::cerr << "Failed to open log file '" << log_path_.string() << "'\n";
      }
    }

    void rotate() {
      file_.flush();
      file_.close();

      if (const auto rotation_error = logging::rotate_log_file(log_path_)) {
        std::cerr << "Failed to rotate log file '" << log_path_.string() << "': " << rotation_error.message() << '\n';
      }

      file_.clear();
      open_file();
    }

    std::filesystem::path log_path_;
    std::ofstream file_;
    std::uintmax_t current_size_ {0};
  };

  /**
   * @brief Ostream wrapper that owns a rotating file buffer.
   */
  class rotating_file_stream: public std::ostream {
  public:
    explicit rotating_file_stream(const std::filesystem::path &log_path):
        std::ostream {nullptr},
        buffer_ {log_path} {
      rdbuf(&buffer_);
    }

  private:
    rotating_file_streambuf buffer_;
  };
}  // namespace

boost::shared_ptr<boost::log::sinks::asynchronous_sink<boost::log::sinks::text_ostream_backend>> sink;  ///< Sink.

bl::sources::severity_logger<int> verbose {0};  ///< Dominating output.
bl::sources::severity_logger<int> debug {1};  ///< Follow what is happening.
bl::sources::severity_logger<int> info {2};  ///< Should be informed about.
bl::sources::severity_logger<int> warning {3};  ///< Strange events.
bl::sources::severity_logger<int> error {4};  ///< Recoverable errors.
bl::sources::severity_logger<int> fatal {5};  ///< Unrecoverable errors.
#ifdef SUNSHINE_TESTS
bl::sources::severity_logger<int> tests {10};  ///< Automatic tests output.
#endif

BOOST_LOG_ATTRIBUTE_KEYWORD(severity, "Severity", int)

namespace logging {
  deinit_t::~deinit_t() {
    deinit();
  }

  void deinit() {
    log_flush();
    bl::core::get()->remove_sink(sink);
    sink.reset();
  }

  /**
   * @brief Format a Boost.Log record for Sunshine log output.
   */
  void formatter(const boost::log::record_view &view, boost::log::formatting_ostream &os) {
    constexpr const char *message = "Message";
    constexpr const char *severity = "Severity";

    auto log_level = view.attribute_values()[severity].extract<int>().get();

    std::string_view log_type;
    switch (log_level) {
      case 0:
        log_type = "Verbose: "sv;
        break;
      case 1:
        log_type = "Debug: "sv;
        break;
      case 2:
        log_type = "Info: "sv;
        break;
      case 3:
        log_type = "Warning: "sv;
        break;
      case 4:
        log_type = "Error: "sv;
        break;
      case 5:
        log_type = "Fatal: "sv;
        break;
#ifdef SUNSHINE_TESTS
      case 10:
        log_type = "Tests: "sv;
        break;
#endif
    };

    auto now = std::chrono::system_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
      now - std::chrono::time_point_cast<std::chrono::seconds>(now)
    );

    auto t = std::chrono::system_clock::to_time_t(now);
    auto lt = *std::localtime(&t);

    os << "["sv << std::put_time(&lt, "%Y-%m-%d %H:%M:%S.") << boost::format("%03u") % ms.count() << "]: "sv
       << log_type << view.attribute_values()[message].extract<std::string>();
  }

  [[nodiscard]] boost::shared_ptr<std::ostream> make_rotating_file_stream(const std::filesystem::path &log_path) {
    return boost::make_shared<rotating_file_stream>(log_path);
  }
#ifdef __ANDROID__
  namespace sinks = boost::log::sinks;
  namespace expr = boost::log::expressions;

  void android_log(const std::string &message, int severity) {
    android_LogPriority android_priority;
    switch (severity) {
      case 0:
        android_priority = ANDROID_LOG_VERBOSE;
        break;
      case 1:
        android_priority = ANDROID_LOG_DEBUG;
        break;
      case 2:
        android_priority = ANDROID_LOG_INFO;
        break;
      case 3:
        android_priority = ANDROID_LOG_WARN;
        break;
      case 4:
        android_priority = ANDROID_LOG_ERROR;
        break;
      case 5:
        android_priority = ANDROID_LOG_FATAL;
        break;
      default:
        android_priority = ANDROID_LOG_UNKNOWN;
        break;
    }
    __android_log_print(android_priority, "PLANK", "%s", message.c_str());
  }

  // custom sink backend for android
  struct android_sink_backend: public sinks::basic_sink_backend<sinks::concurrent_feeding> {
    void consume(const bl::record_view &rec) {
      int log_sev = rec[severity].get();
      const std::string log_msg = rec[expr::smessage].get();
      // log to android
      android_log(log_msg, log_sev);
    }
  };
#endif

  [[nodiscard]] std::unique_ptr<deinit_t> init(int min_log_level, const std::string &log_file) {
    if (sink) {
      // Deinitialize the logging system before reinitializing it. This can probably only ever be hit in tests.
      deinit();
    }

    const auto log_path = std::filesystem::path {std::u8string {log_file.begin(), log_file.end()}};

#ifndef __ANDROID__
    setup_av_logging(min_log_level);
#endif

    sink = boost::make_shared<text_sink>();

#if !defined(SUNSHINE_TESTS) && !defined(__linux__)
    // Linux package services keep detailed runtime output in the private
    // product log. Their stdout is reserved for early startup failures and
    // supervisor diagnostics, so routine records do not flood journald.
    boost::shared_ptr<std::ostream> stream {&std::cout, boost::null_deleter()};
    sink->locked_backend()->add_stream(stream);
#endif

    sink->locked_backend()->add_stream(make_rotating_file_stream(log_path));
    sink->set_filter(severity >= min_log_level);
    sink->set_formatter(&formatter);

    // Prevent the async sink's background thread from dying on backend exceptions.
    // Without this, a single I/O error (disk full, file locked, broken stdout, etc.)
    // kills the thread and all subsequent log records are silently lost.
    sink->set_exception_handler(bl::make_exception_suppressor());

    // Flush after each log record to ensure log file contents on disk isn't stale.
    // This is particularly important when running from a Windows service.
    sink->locked_backend()->auto_flush(true);

    bl::core::get()->add_sink(sink);

#ifdef __ANDROID__
    auto android_sink = boost::make_shared<sinks::synchronous_sink<android_sink_backend>>();
    bl::core::get()->add_sink(android_sink);
#endif
    return std::make_unique<deinit_t>();
  }

#ifndef __ANDROID__
  void setup_av_logging(int min_log_level) {
    if (min_log_level >= 1) {
      av_log_set_level(AV_LOG_QUIET);
    } else {
      av_log_set_level(AV_LOG_DEBUG);
    }
    av_log_set_callback([](void *ptr, int level, const char *fmt, va_list vl) {
      static int print_prefix = 1;
      char buffer[1024];

      av_log_format_line(ptr, level, fmt, vl, buffer, sizeof(buffer), &print_prefix);
      if (level <= AV_LOG_ERROR) {
        // We print AV_LOG_FATAL at the error level. FFmpeg prints things as fatal that
        // are expected in some cases, such as lack of codec support or similar things.
        BOOST_LOG(error) << buffer;
      } else if (level <= AV_LOG_WARNING) {
        BOOST_LOG(warning) << buffer;
      } else if (level <= AV_LOG_INFO) {
        BOOST_LOG(info) << buffer;
      } else if (level <= AV_LOG_VERBOSE) {
        // AV_LOG_VERBOSE is less verbose than AV_LOG_DEBUG
        BOOST_LOG(debug) << buffer;
      } else {
        BOOST_LOG(verbose) << buffer;
      }
    });
  }

#endif

  void log_flush() {
    if (sink) {
      sink->flush();
    }
  }

  void print_help(const char *name) {
    std::cout
      << "Usage: "sv << name << " [options] [/path/to/configuration_file] [--cmd]"sv << std::endl
      << "    Any configurable option can be overwritten with: \"name=value\""sv << std::endl
      << std::endl
      << "    Note: The configuration will be created if it doesn't exist."sv << std::endl
      << std::endl
      << "    --help                    | print help"sv << std::endl
      << "    --version                 | print the PLANK host version"sv << std::endl
      << std::endl
      << "    flags"sv << std::endl
      << "        -1 | Do not load previously saved state and do retain any state after shutdown"sv << std::endl
      << "           | Effectively start with a temporary workstation identity"sv << std::endl
      << "        -2 | Force replacement of headers in video stream"sv << std::endl
      << std::endl;
  }

  std::string bracket(const std::string &input) {
    return "["s + input + "]"s;
  }

  std::wstring bracket(const std::wstring &input) {
    return L"["s + input + L"]"s;
  }

}  // namespace logging
