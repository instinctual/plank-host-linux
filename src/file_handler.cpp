/**
 * @file file_handler.cpp
 * @brief Definitions for file handling functions.
 */

// standard includes
#include <cerrno>
#include <filesystem>
#include <fstream>

  #include <fcntl.h>
  #include <sys/stat.h>
  #include <unistd.h>

// local includes
#include "file_handler.h"
#include "logging.h"

namespace file_handler {
  std::string get_parent_directory(const std::string &path) {
    // remove any trailing path separators
    std::string trimmed_path = path;
    while (!trimmed_path.empty() && trimmed_path.back() == '/') {
      trimmed_path.pop_back();
    }

    std::filesystem::path p(trimmed_path);
    return p.parent_path().string();
  }

  bool make_directory(const std::string &path) {
    // first, check if the directory already exists
    if (std::filesystem::exists(path)) {
      return true;
    }

    return std::filesystem::create_directories(path);
  }

  std::string read_file(const char *path) {
    if (!std::filesystem::exists(path)) {
      BOOST_LOG(debug) << "Missing file: " << path;
      return {};
    }

    std::ifstream in(path);
    return std::string {(std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>()};
  }

  int write_file(const char *path, const std::string_view &contents, std::filesystem::perms permissions) {
    mode_t mode = 0;
    const auto has_permission = [permissions](std::filesystem::perms permission) {
      return (permissions & permission) != std::filesystem::perms::none;
    };
    mode |= has_permission(std::filesystem::perms::owner_read) ? S_IRUSR : 0;
    mode |= has_permission(std::filesystem::perms::owner_write) ? S_IWUSR : 0;
    mode |= has_permission(std::filesystem::perms::owner_exec) ? S_IXUSR : 0;
    mode |= has_permission(std::filesystem::perms::group_read) ? S_IRGRP : 0;
    mode |= has_permission(std::filesystem::perms::group_write) ? S_IWGRP : 0;
    mode |= has_permission(std::filesystem::perms::group_exec) ? S_IXGRP : 0;
    mode |= has_permission(std::filesystem::perms::others_read) ? S_IROTH : 0;
    mode |= has_permission(std::filesystem::perms::others_write) ? S_IWOTH : 0;
    mode |= has_permission(std::filesystem::perms::others_exec) ? S_IXOTH : 0;

    const int fd = ::open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, mode);
    if (fd < 0) {
      return -1;
    }

    if (::fchmod(fd, mode) != 0) {
      ::close(fd);
      return -1;
    }

    std::size_t offset = 0;
    while (offset < contents.size()) {
      const auto written = ::write(fd, contents.data() + offset, contents.size() - offset);
      if (written < 0 && errno == EINTR) {
        continue;
      }
      if (written <= 0) {
        ::close(fd);
        return -1;
      }
      offset += static_cast<std::size_t>(written);
    }

    const int sync_result = ::fsync(fd);
    const int close_result = ::close(fd);
    if (sync_result != 0 || close_result != 0) {
      return -1;
    }

    return 0;
  }
}  // namespace file_handler
