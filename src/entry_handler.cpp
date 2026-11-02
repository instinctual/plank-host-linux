/**
 * @file entry_handler.cpp
 * @brief Definitions for entry handling functions.
 */
// standard includes
#include <csignal>
#include <format>
#include <iostream>
#include <thread>

// local includes
#include "config.h"
#include "entry_handler.h"
#include "globals.h"
#include "httpcommon.h"
#include "logging.h"
#include "network.h"
#include "platform/common.h"

using namespace std::literals;

namespace args {
  int help(const char *name) {
    logging::print_help(name);
    return 0;
  }

  int version() {
    // version was already logged at startup
    return 0;
  }

}  // namespace args

namespace lifetime {
  char **argv;  ///< Command-line argument vector.
  std::atomic_int desired_exit_code;  ///< Desired exit code.

  void exit_sunshine(int exit_code, bool async) {
    // Store the exit code of the first exit_sunshine() call
    int zero = 0;
    desired_exit_code.compare_exchange_strong(zero, exit_code);

    // Raise SIGINT to start termination
    std::raise(SIGINT);

    // Termination will happen asynchronously, but the caller may
    // have wanted synchronous behavior.
    while (!async) {
      std::this_thread::sleep_for(1s);
    }
  }

  void debug_trap() {
    std::raise(SIGTRAP);
  }

  char **get_argv() {
    return argv;
  }
}  // namespace lifetime

void log_publisher_data() {
  BOOST_LOG(info) << "Package Publisher: "sv << SUNSHINE_PUBLISHER_NAME;
  BOOST_LOG(info) << "Publisher Website: "sv << SUNSHINE_PUBLISHER_WEBSITE;
  if (std::string_view {SUNSHINE_PUBLISHER_ISSUE_URL}.size() != 0) {
    BOOST_LOG(info) << "Support: "sv << SUNSHINE_PUBLISHER_ISSUE_URL;
  }
}
