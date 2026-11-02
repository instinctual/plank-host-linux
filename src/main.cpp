/**
 * @file src/main.cpp
 * @brief Definitions for the main entry point for Sunshine.
 */
// standard includes
#include <codecvt>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>

#ifdef __APPLE__
  #include <mach-o/dyld.h>
#endif

// local includes
#include "entry_handler.h"
#include "globals.h"
#include "httpcommon.h"
#include "logging.h"
#include "main.h"
#include "nvhttp.h"
#include "process.h"
#include "session_stream.h"
#ifdef __linux__
  #include "session/session_context.h"
#endif
#include "video.h"

#ifdef PLANK_TRANSPORT
  #include <plank_transport.h>
#endif

using namespace std::literals;

std::map<int, std::function<void()>> signal_handlers;  ///< Signal handlers.

/**
 * @brief Forward a POSIX signal to the registered Sunshine handler.
 *
 * @param sig Native signal number being handled.
 */
void on_signal_forwarder(int sig) {
  signal_handlers.at(sig)();
}

/**
 * @brief Register the handler invoked for a POSIX signal.
 *
 * @param sig Native signal number being handled.
 * @param fn Signal handler function to install.
 */
template<class FN>
void on_signal(int sig, FN &&fn) {
  signal_handlers.emplace(sig, std::forward<FN>(fn));

  std::signal(sig, on_signal_forwarder);
}

/**
 * @brief Cmd to func.
 */
std::map<std::string_view, std::function<int(const char *name, int argc, char **argv)>> cmd_to_func {
  {"help"sv, [](const char *name, int argc, char **argv) {
     return args::help(name);
   }},
  {"version"sv, [](const char *name, int argc, char **argv) {
     return args::version();
   }},
#ifdef _WIN32
  {"restore-nvprefs-undo"sv, [](const char *name, int argc, char **argv) {
     return args::restore_nvprefs_undo();
   }},
#endif
};

#ifdef _WIN32
/**
 * @brief Handle Windows session-change messages for the monitor window.
 *
 * @param hwnd Window handle receiving the Windows control event.
 * @param uMsg U msg.
 * @param wParam W param.
 * @param lParam L param.
 * @return Process or platform callback exit code.
 */
LRESULT CALLBACK SessionMonitorWindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
  switch (uMsg) {
    case WM_CLOSE:
      DestroyWindow(hwnd);
      return 0;
    case WM_DESTROY:
      PostQuitMessage(0);
      return 0;
    case WM_ENDSESSION:
      {
        // Terminate ourselves with a blocking exit call
        std::cout << "Received WM_ENDSESSION"sv << std::endl;
        lifetime::exit_sunshine(0, false);
        return 0;
      }
    default:
      return DefWindowProc(hwnd, uMsg, wParam, lParam);
  }
}

/**
 * @brief Handle Windows console control events for shutdown.
 *
 * @param type Protocol, message, or resource type selector.
 * @return Process or platform callback exit code.
 */
WINAPI BOOL ConsoleCtrlHandler(DWORD type) {
  if (type == CTRL_CLOSE_EVENT) {
    BOOST_LOG(info) << "Console closed handler called";
    lifetime::exit_sunshine(0, false);
  }
  return FALSE;
}
#endif

/**
 * @brief Run the main event loop until Sunshine is asked to exit.
 *
 * @param shutdown_event Shutdown event.
 */
void mainThreadLoop(const std::shared_ptr<safe::event_t<bool>> &shutdown_event) {
  shutdown_event->view();
}

/**
 * @brief Run the main application or worker loop.
 *
 * @param argc The number of arguments.
 * @param argv The arguments.
 * @return Process or platform callback exit code.
 */
int main(int argc, char *argv[]) {
#ifdef __APPLE__
  // Bundle assets are referenced relative to the executable
  // (e.g. ../Resources/assets), so anchor cwd to Contents/MacOS.
  {
    char executable[2048];
    uint32_t size = sizeof(executable);
    if (_NSGetExecutablePath(executable, &size) == 0) {
      std::error_code ec;
      auto exec_dir = std::filesystem::weakly_canonical(std::filesystem::path {executable}, ec).parent_path();
      if (!ec) {
        std::filesystem::current_path(exec_dir, ec);
      }
      if (ec) {
        std::cerr << "Failed to set working directory to executable path: " << ec.message() << '\n';
      }
    }
  }
#endif

  lifetime::argv = argv;

  task_pool_util::TaskPool::task_id_t force_shutdown = nullptr;

#ifdef _WIN32
  // Avoid searching the PATH in case a user has configured their system insecurely
  // by placing a user-writable directory in the system-wide PATH variable.
  SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_APPLICATION_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);

  setlocale(LC_ALL, "C");
#endif

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
  // Use UTF-8 conversion for the default C++ locale (used by boost::log)
  std::locale::global(std::locale(std::locale(), new std::codecvt_utf8<wchar_t>));
#pragma GCC diagnostic pop

  mail::man = std::make_shared<safe::mail_raw_t>();

  // parse config file
  if (config::parse(argc, argv)) {
    return 0;
  }

  auto log_deinit_guard = logging::init(config::sunshine.min_log_level, config::sunshine.log_file);
  if (!log_deinit_guard) {
    BOOST_LOG(error) << "Logging failed to initialize"sv;
  }

  // logging can begin at this point
  // if anything is logged prior to this point, it will appear in stdout, but not in the log viewer in the UI
  // the version should be printed to the log before anything else
  BOOST_LOG(info) << "PLANK Host version: "sv << PROJECT_VERSION
                  << " commit: "sv << PROJECT_VERSION_COMMIT;

#ifdef PLANK_TRANSPORT
  if (plank_transport_abi_version() != PLANK_TRANSPORT_ABI_VERSION) {
    BOOST_LOG(fatal) << "PLANK native transport ABI mismatch"sv;
    return 9;
  }
  BOOST_LOG(info) << "PLANK native transport ABI "sv
                  << plank_transport_abi_version() << " is available"sv;
#endif

  // Log publisher metadata
  log_publisher_data();

  // Log modified_config_settings
  config::log_config_settings(config::modified_config_settings, false);
  config::modified_config_settings.clear();

#ifdef __linux__
  auto supervisor_control = plank::session::start_supervisor_control(
    [](std::uint64_t generation) {
      BOOST_LOG(info) << "Desktop attachment changed to generation "sv << generation;
      mail::man->event<std::uint64_t>(mail::desktop_reattach)->raise(generation);
    },
    [] {
      BOOST_LOG(info) << "Opening authenticated desktop; notifying the connected client"sv;
      session_stream::notify_desktop_handoff();
      std::raise(SIGTERM);
    }
  );
  if (getenv("PLANK_SESSION_CONTROL_FD") != nullptr && !supervisor_control) {
    BOOST_LOG(fatal) << "Supervisor desktop control channel was rejected"sv;
    return 8;
  }
#endif

  if (!config::sunshine.cmd.name.empty()) {
    auto fn = cmd_to_func.find(config::sunshine.cmd.name);
    if (fn == std::end(cmd_to_func)) {
      BOOST_LOG(fatal) << "Unknown command: "sv << config::sunshine.cmd.name;

      BOOST_LOG(info) << "Possible commands:"sv;
      for (auto &[key, _] : cmd_to_func) {
        BOOST_LOG(info) << '\t' << key;
      }

      return 7;
    }

    return fn->second(argv[0], config::sunshine.cmd.argc, config::sunshine.cmd.argv);
  }

#ifdef _WIN32
  // Modify relevant NVIDIA control panel settings if the system has corresponding gpu
  if (nvprefs_instance.load()) {
    // Restore global settings to the undo file left by improper termination of sunshine.exe
    nvprefs_instance.restore_from_and_delete_undo_file_if_exists();
    // Modify application settings for sunshine.exe
    nvprefs_instance.modify_application_profile();
    // Modify global settings, undo file is produced in the process to restore after improper termination
    nvprefs_instance.modify_global_profile();
    // Unload dynamic library to survive driver re-installation
    nvprefs_instance.unload();
  }

  // Wait as long as possible to terminate Sunshine.exe during logoff/shutdown
  SetProcessShutdownParameters(0x100, SHUTDOWN_NORETRY);

  // We must create a hidden window to receive shutdown notifications since we load gdi32.dll
  std::promise<HWND> session_monitor_hwnd_promise;
  auto session_monitor_hwnd_future = session_monitor_hwnd_promise.get_future();
  std::promise<void> session_monitor_join_thread_promise;
  auto session_monitor_join_thread_future = session_monitor_join_thread_promise.get_future();

  std::jthread session_monitor_thread([&]() {
    platf::set_thread_name("session_monitor");
    session_monitor_join_thread_promise.set_value_at_thread_exit();

    WNDCLASSA wnd_class {};
    wnd_class.lpszClassName = "SunshineSessionMonitorClass";
    wnd_class.lpfnWndProc = SessionMonitorWindowProc;
    if (!RegisterClassA(&wnd_class)) {
      session_monitor_hwnd_promise.set_value(nullptr);
      BOOST_LOG(error) << "Failed to register session monitor window class"sv << std::endl;
      return;
    }

    auto wnd = CreateWindowExA(
      0,
      wnd_class.lpszClassName,
      "Sunshine Session Monitor Window",
      0,
      CW_USEDEFAULT,
      CW_USEDEFAULT,
      CW_USEDEFAULT,
      CW_USEDEFAULT,
      nullptr,
      nullptr,
      nullptr,
      nullptr
    );

    session_monitor_hwnd_promise.set_value(wnd);

    if (!wnd) {
      BOOST_LOG(error) << "Failed to create session monitor window"sv << std::endl;
      return;
    }

    ShowWindow(wnd, SW_HIDE);

    // Run the message loop for our window
    MSG msg {};
    while (GetMessage(&msg, nullptr, 0, 0) > 0) {
      TranslateMessage(&msg);
      DispatchMessage(&msg);
    }
  });

  auto session_monitor_join_thread_guard = util::fail_guard([&]() {
    if (session_monitor_hwnd_future.wait_for(1s) == std::future_status::ready) {
      if (HWND session_monitor_hwnd = session_monitor_hwnd_future.get()) {
        PostMessage(session_monitor_hwnd, WM_CLOSE, 0, 0);
      }

      if (session_monitor_join_thread_future.wait_for(1s) == std::future_status::ready) {
        session_monitor_thread.join();
        return;
      } else {
        BOOST_LOG(warning) << "session_monitor_join_thread_future reached timeout";
      }
    } else {
      BOOST_LOG(warning) << "session_monitor_hwnd_future reached timeout";
    }

    session_monitor_thread.detach();
  });

#endif

  task_pool.start(1);

  // Create signal handler after logging has been initialized
  auto shutdown_event = mail::man->event<bool>(mail::shutdown);
  on_signal(SIGINT, [&force_shutdown, shutdown_event]() {
    BOOST_LOG(info) << "Interrupt handler called"sv;

    auto task = []() {
      BOOST_LOG(fatal) << "10 seconds passed, yet Sunshine's still running: Forcing shutdown"sv;
      logging::log_flush();
      lifetime::debug_trap();
    };
    force_shutdown = task_pool.pushDelayed(task, 10s).task_id;

    // Break out of the main loop
    shutdown_event->raise(true);

  });

  on_signal(SIGTERM, [&force_shutdown, shutdown_event]() {
    BOOST_LOG(info) << "Terminate handler called"sv;

    auto task = []() {
      BOOST_LOG(fatal) << "10 seconds passed, yet Sunshine's still running: Forcing shutdown"sv;
      logging::log_flush();
      lifetime::debug_trap();
    };
    force_shutdown = task_pool.pushDelayed(task, 10s).task_id;

    // Break out of the main loop
    shutdown_event->raise(true);

  });

#ifdef _WIN32
  // Terminate gracefully on Windows when console window is closed
  SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE);
#endif

  // If any of the following fail, log an error and continue so the failure is diagnosable.

  auto platf_deinit_guard = platf::init();
  if (!platf_deinit_guard) {
    BOOST_LOG(error) << "Platform failed to initialize"sv;
  }

  auto proc_deinit_guard = proc::init();
  if (!proc_deinit_guard) {
    BOOST_LOG(error) << "Proc failed to initialize"sv;
  }

  auto input_deinit_guard = input::init();

  if (video::probe_encoders()) {
    BOOST_LOG(error) << "Video failed to find working encoder"sv;
  }

  if (http::init()) {
    BOOST_LOG(fatal) << "HTTP interface failed to initialize"sv;

#ifdef _WIN32
    BOOST_LOG(fatal) << "To relaunch Sunshine successfully, use the shortcut in the Start Menu. Do not run Sunshine.exe manually."sv;
    std::this_thread::sleep_for(10s);
#endif

    return -1;
  }

  std::unique_ptr<platf::deinit_t> mDNS;
  const bool mdns_discovery_enabled = config::sunshine.mdns_discovery;
  if (!mdns_discovery_enabled) {
    BOOST_LOG(info) << "PLANK mDNS advertisement is disabled"sv;
  }
  auto sync_mDNS = std::async(std::launch::async, [&mDNS, mdns_discovery_enabled]() {
    if (mdns_discovery_enabled) {
      mDNS = platf::publish::start();
    }
  });

  // FIXME: Temporary workaround: Simple-Web_server needs to be updated or replaced
  if (shutdown_event->peek()) {
    return lifetime::desired_exit_code;
  }

  std::jthread httpThread {nvhttp::start};
  std::jthread nativeSessionThread {session_stream::start};

#ifdef _WIN32
  // If we're using the default port and GameStream is enabled, warn the user
  if (config::sunshine.port == 47989 && is_gamestream_enabled()) {
    BOOST_LOG(fatal) << "GameStream is still enabled in GeForce Experience and conflicts with the PLANK streaming ports."sv;
    BOOST_LOG(fatal) << "Disable GameStream on the SHIELD tab in GeForce Experience or change the PLANK port in /etc/plank/host.conf."sv;
  }
#endif

  mainThreadLoop(shutdown_event);

  httpThread.join();
  nativeSessionThread.join();

  task_pool.stop();
  task_pool.join();

#ifdef _WIN32
  // Restore global NVIDIA control panel settings
  if (nvprefs_instance.owning_undo_file() && nvprefs_instance.load()) {
    nvprefs_instance.restore_global_profile();
    nvprefs_instance.unload();
  }
#endif

  return lifetime::desired_exit_code;
}
