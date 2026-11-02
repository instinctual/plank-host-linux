/**
 * @file src/config.cpp
 * @brief Definitions for the configuration of Sunshine.
 */
// standard includes
#include <algorithm>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <iostream>
#include <thread>
#include <unordered_map>
#include <utility>

// lib includes
#include <boost/asio.hpp>
#include <boost/filesystem.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>

// local includes
#include "config.h"
#include "entry_handler.h"
#include "file_handler.h"
#include "logging.h"
#include "nvhttp.h"
#include "platform/common.h"
#include "utility.h"

namespace fs = std::filesystem;
using namespace std::literals;

constexpr auto CA_DIR = "credentials";  ///< Subdirectory under app data that stores Sunshine credentials.
const std::string PRIVATE_KEY_FILE = std::string(CA_DIR) + "/cakey.pem";  ///< Relative path to the persisted private key PEM file.
const std::string CERTIFICATE_FILE = std::string(CA_DIR) + "/cacert.pem";  ///< Relative path to the persisted certificate PEM file.
namespace config {

  namespace nv {

    /**
     * @brief Parse the `nvenc_twopass` configuration value.
     *
     * @param preset Encoder preset value supplied by the configuration.
     * @return Parsed enum value, or the setting-specific default when the text is unknown.
     */
    nvenc::nvenc_two_pass twopass_from_view(const std::string_view &preset) {
      if (preset == "disabled") {
        return nvenc::nvenc_two_pass::disabled;
      }
      if (preset == "quarter_res") {
        return nvenc::nvenc_two_pass::quarter_resolution;
      }
      if (preset == "full_res") {
        return nvenc::nvenc_two_pass::full_resolution;
      }
      BOOST_LOG(warning) << "config: unknown nvenc_twopass value: " << preset;
      return nvenc::nvenc_two_pass::quarter_resolution;
    }

    /**
     * @brief Parse the `nvenc_split_encode` configuration value.
     *
     * @param preset Encoder preset value supplied by the configuration.
     * @return Parsed enum value, or the setting-specific default when the text is unknown.
     */
    nvenc::nvenc_split_frame_encoding split_encode_from_view(const std::string_view &preset) {
      using enum nvenc::nvenc_split_frame_encoding;
      if (preset == "disabled") {
        return disabled;
      }
      if (preset == "driver_decides") {
        return driver_decides;
      }
      if (preset == "enabled") {
        return force_enabled;
      }
      BOOST_LOG(warning) << "config: unknown nvenc_split_encode value: " << preset;
      return driver_decides;
    }

  }  // namespace nv

  /**
   * @brief Default video configuration values used before file and CLI overrides.
   */
  video_t video {
    28,  // qp

    2,  // min_threads
    {
      "superfast"s,  // preset
      "zerolatency"s,  // tune
      100,  // VBV maximum-rate percentage
      0,  // VBV buffer frames (legacy automatic sizing)
      0,  // scene-change threshold
    },  // software

    {},  // nv
    {},  // adapter_name

    0  // minimum_fps_target (0 = framerate)
  };

  /**
   * @brief Default audio configuration values used before file and CLI overrides.
   */
  audio_t audio {
    {},  // audio_sink
    {},  // virtual_sink
    true,  // stream audio
  };

  /**
   * @brief Default stream configuration values used before file and CLI overrides.
   */
  stream_t stream {
    10s,  // ping_timeout
  };

  /**
   * @brief Default NVHTTP server configuration values used before file and CLI overrides.
   */
  nvhttp_t nvhttp {
    PRIVATE_KEY_FILE,
    CERTIFICATE_FILE,

    platf::get_host_name(),  // host_name,
    "plank-state.json"s,  // file_state
    false,  // publish_session_user
  };

  /**
   * @brief Default input configuration values used before file and CLI overrides.
   */
  input_t input {
    {
      {0x10, 0xA0},
      {0x11, 0xA2},
      {0x12, 0xA4},
    },
    500ms,  // key_repeat_delay
    std::chrono::duration<double> {1 / 24.9},  // key_repeat_period


    true,  // keyboard enabled
    false,  // map Right Alt to the Windows key
    true,  // mouse enabled
    true,  // always send scancodes
    true,  // high resolution scrolling
  };

  /**
   * @brief Default top-level Sunshine configuration values used before file and CLI overrides.
   */
  sunshine_t sunshine {
    2,  // min_log_level
    0,  // flags
#ifdef PLANK_PRODUCT_BUILD
    "/etc/plank/host.conf",  // config file
#else
    platf::appdata().string() + "/sunshine.conf",  // config file
#endif
    {},  // cmd args
    28989,  // Base port number
#ifdef PLANK_PRODUCT_BUILD
    "/var/log/plank/host.log",  // log file
#else
    platf::appdata().string() + "/sunshine.log",  // log file
#endif
    false,  // PLANK mDNS advertisement
    "physical",  // PLANK startup display policy
  };

  /**
   * @brief Return whether a character terminates a configuration line.
   *
   * @param ch Character currently being classified by the parser.
   * @return True when the tested parser condition is met.
   */
  bool endline(char ch) {
    return ch == '\r' || ch == '\n';
  }

  /**
   * @brief Return whether a character is horizontal parser whitespace.
   *
   * @param ch Character currently being classified by the parser.
   * @return True when the tested parser condition is met.
   */
  bool space_tab(char ch) {
    return ch == ' ' || ch == '\t';
  }

  /**
   * @brief Return whether a character should be treated as parser whitespace.
   *
   * @param ch Character currently being classified by the parser.
   * @return True when the tested parser condition is met.
   */
  bool whitespace(char ch) {
    return space_tab(ch) || endline(ch);
  }

  /**
   * @brief Return whether a non-empty configuration line is an INI section header.
   *
   * Sections are organizational only. Option names remain globally scoped so the
   * existing runtime option names and command-line overrides stay unchanged.
   */
  bool section_header(std::string_view line) {
    const auto comment = line.find('#');
    line = line.substr(0, comment);

    const auto first = line.find_first_not_of(" \t");
    if (first == std::string_view::npos) {
      return false;
    }
    const auto last = line.find_last_not_of(" \t");
    return line[first] == '[' && line[last] == ']' && last > first + 1;
  }

  /**
   * @brief Copy a configuration text range while stripping inline comments.
   *
   * @param begin Iterator or pointer marking the start of the input range.
   * @param end Iterator or pointer marking the end of the input range.
   * @return Value converted to string.
   */
  std::string to_string(const char *begin, const char *end) {
    std::string result;

    KITTY_WHILE_LOOP(auto pos = begin, pos != end, {
      auto comment = std::find(pos, end, '#');
      auto endl = std::find_if(comment, end, endline);

      result.append(pos, comment);

      pos = endl;
    })

    return result;
  }

  /**
   * @brief Advance over a bracketed list while honoring nested brackets.
   *
   * @param skipper Function used to skip characters while parsing.
   * @param end Iterator or pointer marking the end of the input range.
   * @return Iterator positioned after the matching closing bracket or at the end.
   */
  template<class It>
  It skip_list(It skipper, It end) {
    int stack = 1;
    while (skipper != end && stack) {
      if (*skipper == '[') {
        ++stack;
      }
      if (*skipper == ']') {
        --stack;
      }

      ++skipper;
    }

    return skipper;
  }

  std::pair<
    std::string_view::const_iterator,
    std::optional<std::pair<std::string, std::string>>>
    /**
     * @brief Parse one `name = value` configuration entry.
     *
     * @param begin Iterator or pointer marking the start of the input range.
     * @param end Iterator or pointer marking the end of the input range.
     * @return Iterator for the next line and the parsed key-value pair when one was found.
     */
    parse_option(std::string_view::const_iterator begin, std::string_view::const_iterator end) {
    begin = std::find_if_not(begin, end, whitespace);
    auto endl = std::find_if(begin, end, endline);
    if (section_header(std::string_view(begin, endl))) {
      return std::make_pair(endl, std::nullopt);
    }
    auto endc = std::find(begin, endl, '#');
    endc = std::find_if(std::make_reverse_iterator(endc), std::make_reverse_iterator(begin), std::not_fn(whitespace)).base();

    auto eq = std::find(begin, endc, '=');
    if (eq == endc || eq == begin) {
      return std::make_pair(endl, std::nullopt);
    }

    auto end_name = std::find_if_not(std::make_reverse_iterator(eq), std::make_reverse_iterator(begin), space_tab).base();
    auto begin_val = std::find_if_not(eq + 1, endc, space_tab);

    if (begin_val == endl) {
      return std::make_pair(endl, std::nullopt);
    }

    // Lists might contain newlines
    if (*begin_val == '[') {
      endl = skip_list(begin_val + 1, end);

      // Check if we reached the end of the file without finding a closing bracket
      // We know we have a valid closing bracket if:
      // 1. We didn't reach the end, or
      // 2. We reached the end but the last character was the matching closing bracket
      if (endl == end && end == begin_val + 1) {
        BOOST_LOG(warning) << "config: Missing ']' in config option: " << to_string(begin, end_name);
        return std::make_pair(endl, std::nullopt);
      }
    }

    return std::make_pair(
      endl,
      std::make_pair(to_string(begin, end_name), to_string(begin_val, endl))
    );
  }

  /**
   * @brief Parse INI-style PLANK configuration text into key-value entries.
   *
   * Section headers organize the file but do not namespace option names.
   */
  std::unordered_map<std::string, std::string> parse_config(const std::string_view &file_content) {
    std::unordered_map<std::string, std::string> vars;

    auto pos = std::begin(file_content);
    auto end = std::end(file_content);

    while (pos < end) {
      // auto newline = std::find_if(pos, end, [](auto ch) { return ch == '\n' || ch == '\r'; });
      TUPLE_2D(endl, var, parse_option(pos, end));

      pos = endl;
      if (pos != end) {
        pos += (*pos == '\r') ? 2 : 1;
      }

      if (!var) {
        continue;
      }

      vars.emplace(std::move(*var));
    }

    return vars;
  }

  /**
   * @brief Consume a string setting from the parsed configuration map.
   *
   * @param vars Parsed configuration entries; consumed keys are erased.
   * @param name Configuration key to consume.
   * @param input Destination field updated when the setting exists and parses successfully.
   */
  void string_f(std::unordered_map<std::string, std::string> &vars, const std::string &name, std::string &input) {
    auto it = vars.find(name);
    if (it == std::end(vars)) {
      return;
    }

    input = std::move(it->second);

    vars.erase(it);
  }

  /**
   * @brief Consume a setting and convert it with a caller-provided parser.
   *
   * @param vars Parsed configuration entries; consumed keys are erased.
   * @param name Configuration key to consume.
   * @param input Destination field updated when the setting exists and parses successfully.
   * @param f Converter applied to the raw configuration string.
   */
  template<typename T, typename F>
  void generic_f(std::unordered_map<std::string, std::string> &vars, const std::string &name, T &input, F &&f) {
    std::string tmp;
    string_f(vars, name, tmp);
    if (!tmp.empty()) {
      input = f(tmp);
    }
  }

  /**
   * @brief Consume a string setting only when it matches an allowed value.
   *
   * @param vars Parsed configuration entries; consumed keys are erased.
   * @param name Configuration key to consume.
   * @param input Destination field updated when the setting exists and parses successfully.
   * @param allowed_vals Accepted string values for this setting.
   */
  void string_restricted_f(std::unordered_map<std::string, std::string> &vars, const std::string &name, std::string &input, const std::vector<std::string_view> &allowed_vals) {
    std::string temp;
    string_f(vars, name, temp);

    for (auto &allowed_val : allowed_vals) {
      if (temp == allowed_val) {
        input = std::move(temp);
        return;
      }
    }
  }

  /**
   * @brief Parse a comma-separated string setting into a list.
   *
   * @param vars Configuration key-value map.
   * @param name Setting name.
   * @param output Parsed string list.
   */
  void string_list_f(std::unordered_map<std::string, std::string> &vars, const std::string &name, std::vector<std::string> &output) {  // NOSONAR(cpp:S6045): transparent hasher not available for unordered_map in this codebase
    std::string temp;
    string_f(vars, name, temp);

    if (temp.empty()) {
      return;
    }

    output.clear();
    std::stringstream ss(temp);
    std::string item;
    while (std::getline(ss, item, ',')) {
      // Trim whitespace
      item.erase(0, item.find_first_not_of(" \t\r\n"));
      item.erase(item.find_last_not_of(" \t\r\n") + 1);
      if (!item.empty()) {
        output.push_back(item);
      }
    }
  }

  /**
   * @brief Consume a path setting and normalize it under the app data directory when relative.
   *
   * @param vars Parsed configuration entries; consumed keys are erased.
   * @param name Configuration key to consume.
   * @param input Destination field updated when the setting exists and parses successfully.
   */
  void path_f(std::unordered_map<std::string, std::string> &vars, const std::string &name, fs::path &input) {
    // appdata needs to be retrieved once only
    static auto appdata = platf::appdata();

    std::string temp;
    string_f(vars, name, temp);

    if (!temp.empty()) {
      input = temp;
    }

    if (input.is_relative()) {
      input = appdata / input;
    }

    auto dir = input;
    dir.remove_filename();

    // Ensure the directories exists
    if (!fs::exists(dir)) {
      fs::create_directories(dir);
    }
  }

  /**
   * @brief Consume a path setting and normalize it under the app data directory when relative.
   *
   * @param vars Parsed configuration entries; consumed keys are erased.
   * @param name Configuration key to consume.
   * @param input Destination field updated when the setting exists and parses successfully.
   */
  void path_f(std::unordered_map<std::string, std::string> &vars, const std::string &name, std::string &input) {
    fs::path temp = input;

    path_f(vars, name, temp);

    input = temp.string();
  }

  /**
   * @brief Parse a decimal or hexadecimal integer configuration value.
   *
   * @param value Raw configuration value, optionally surrounded by quotes.
   * @return Parsed integer value.
   */
  int parse_config_integer(std::string_view value) {
    if (value.size() >= 2 && value.front() == '"') {
      value = value.substr(1, value.size() - 2);
    }

    if (value.starts_with("0x"sv)) {
      return util::from_hex<int>(value.substr(2));
    }
    return static_cast<int>(util::from_view(value));
  }

  /**
   * @brief Consume an integer setting from decimal or hexadecimal configuration text.
   *
   * @param vars Parsed configuration entries; consumed keys are erased.
   * @param name Configuration key to consume.
   * @param input Destination field updated when the setting exists and parses successfully.
   */
  void int_f(std::unordered_map<std::string, std::string> &vars, const std::string &name, int &input) {
    auto it = vars.find(name);

    if (it == std::end(vars)) {
      return;
    }

    input = parse_config_integer(it->second);
    vars.erase(it);
  }

  /**
   * @brief Consume an integer setting from decimal or hexadecimal configuration text.
   *
   * @param vars Parsed configuration entries; consumed keys are erased.
   * @param name Configuration key to consume.
   * @param input Destination field updated when the setting exists and parses successfully.
   */
  void int_f(std::unordered_map<std::string, std::string> &vars, const std::string &name, std::optional<int> &input) {
    auto it = vars.find(name);

    if (it == std::end(vars)) {
      return;
    }

    input = parse_config_integer(it->second);
    vars.erase(it);
  }

  /**
   * @brief Consume an integer setting from decimal or hexadecimal configuration text.
   *
   * @param vars Parsed configuration entries; consumed keys are erased.
   * @param name Configuration key to consume.
   * @param input Destination field updated when the setting exists and parses successfully.
   * @param f Converter applied to the raw configuration string.
   */
  template<class F>
  void int_f(std::unordered_map<std::string, std::string> &vars, const std::string &name, int &input, F &&f) {
    std::string tmp;
    string_f(vars, name, tmp);
    if (!tmp.empty()) {
      input = f(tmp);
    }
  }

  /**
   * @brief Consume an integer setting from decimal or hexadecimal configuration text.
   *
   * @param vars Parsed configuration entries; consumed keys are erased.
   * @param name Configuration key to consume.
   * @param input Destination field updated when the setting exists and parses successfully.
   * @param f Converter applied to the raw configuration string.
   */
  template<class F>
  void int_f(std::unordered_map<std::string, std::string> &vars, const std::string &name, std::optional<int> &input, F &&f) {
    std::string tmp;
    string_f(vars, name, tmp);
    if (!tmp.empty()) {
      input = f(tmp);
    }
  }

  /**
   * @brief Consume an integer setting only when it falls inside an inclusive range.
   *
   * @param vars Parsed configuration entries; consumed keys are erased.
   * @param name Configuration key to consume.
   * @param input Destination field updated when the setting exists and parses successfully.
   * @param range Inclusive range accepted for the parsed value.
   */
  void int_between_f(std::unordered_map<std::string, std::string> &vars, const std::string &name, int &input, const std::pair<int, int> &range) {
    int temp = input;

    int_f(vars, name, temp);

    TUPLE_2D_REF(lower, upper, range);
    if (temp >= lower && temp <= upper) {
      input = temp;
    }
  }

  /**
   * @brief Convert common textual boolean forms to a boolean value.
   *
   * @param boolean Configuration string to classify as enabled or disabled.
   * @return True when the tested parser condition is met.
   */
  bool to_bool(std::string &boolean) {
    std::for_each(std::begin(boolean), std::end(boolean), [](char ch) {
      return (char) std::tolower(ch);
    });

    return boolean == "true"sv ||
           boolean == "yes"sv ||
           boolean == "enable"sv ||
           boolean == "enabled"sv ||
           boolean == "on"sv ||
           (std::find(std::begin(boolean), std::end(boolean), '1') != std::end(boolean));
  }

  /**
   * @brief Consume a boolean setting from the parsed configuration map.
   *
   * @param vars Parsed configuration entries; consumed keys are erased.
   * @param name Configuration key to consume.
   * @param input Destination field updated when the setting exists and parses successfully.
   */
  void bool_f(std::unordered_map<std::string, std::string> &vars, const std::string &name, bool &input) {
    std::string tmp;
    string_f(vars, name, tmp);

    if (tmp.empty()) {
      return;
    }

    input = to_bool(tmp);
  }

  /**
   * @brief Consume a floating-point setting from the parsed configuration map.
   *
   * @param vars Parsed configuration entries; consumed keys are erased.
   * @param name Configuration key to consume.
   * @param input Destination field updated when the setting exists and parses successfully.
   */
  void double_f(std::unordered_map<std::string, std::string> &vars, const std::string &name, double &input) {
    std::string tmp;
    string_f(vars, name, tmp);

    if (tmp.empty()) {
      return;
    }

    char *c_str_p;
    auto val = std::strtod(tmp.c_str(), &c_str_p);

    if (c_str_p == tmp.c_str()) {
      return;
    }

    input = val;
  }

  /**
   * @brief Consume a floating-point setting only when it falls inside an inclusive range.
   *
   * @param vars Parsed configuration entries; consumed keys are erased.
   * @param name Configuration key to consume.
   * @param input Destination field updated when the setting exists and parses successfully.
   * @param range Inclusive range accepted for the parsed value.
   */
  void double_between_f(std::unordered_map<std::string, std::string> &vars, const std::string &name, double &input, const std::pair<double, double> &range) {
    double temp = input;

    double_f(vars, name, temp);

    TUPLE_2D_REF(lower, upper, range);
    if (temp >= lower && temp <= upper) {
      input = temp;
    }
  }

  /**
   * @brief Consume a comma-separated or bracketed string list setting.
   *
   * @param vars Parsed configuration entries; consumed keys are erased.
   * @param name Configuration key to consume.
   * @param input Destination field updated when the setting exists and parses successfully.
   */
  void list_string_f(std::unordered_map<std::string, std::string> &vars, const std::string &name, std::vector<std::string> &input) {
    std::string string;
    string_f(vars, name, string);

    if (string.empty()) {
      return;
    }

    input.clear();

    auto begin = std::cbegin(string);
    if (*begin == '[') {
      ++begin;
    }

    begin = std::find_if_not(begin, std::cend(string), whitespace);
    if (begin == std::cend(string)) {
      return;
    }

    auto pos = begin;
    while (pos < std::cend(string)) {
      if (*pos == '[') {
        pos = skip_list(pos + 1, std::cend(string)) + 1;
      } else if (*pos == ']') {
        break;
      } else if (*pos == ',') {
        input.emplace_back(begin, pos);
        pos = begin = std::find_if_not(pos + 1, std::cend(string), whitespace);
      } else {
        ++pos;
      }
    }

    if (pos != begin) {
      input.emplace_back(begin, pos);
    }
  }

  /**
   * @brief Consume an integer list setting from decimal or hexadecimal configuration text.
   *
   * @param vars Parsed configuration entries; consumed keys are erased.
   * @param name Configuration key to consume.
   * @param input Destination field updated when the setting exists and parses successfully.
   */
  void list_int_f(std::unordered_map<std::string, std::string> &vars, const std::string &name, std::vector<int> &input) {
    std::vector<std::string> list;
    list_string_f(vars, name, list);

    // check if list is empty, i.e. when the value doesn't exist in the config file
    if (list.empty()) {
      return;
    }

    // The framerate list must be cleared before adding values from the file configuration.
    // If the list is not cleared, then the specified parameters do not affect the behavior of the sunshine server.
    // That is, if you set only 30 fps in the configuration file, it will not work because by default, during initialization the list includes 10, 30, 60, 90 and 120 fps.
    input.clear();
    for (auto &el : list) {
      std::string_view val = el;

      // If value is something like: "756" instead of 756
      if (val.size() >= 2 && val[0] == '"') {
        val = val.substr(1, val.size() - 2);
      }

      int tmp;

      // If the integer is a hexadecimal
      if (val.size() >= 2 && val.substr(0, 2) == "0x"sv) {
        tmp = util::from_hex<int>(val.substr(2));
      } else {
        tmp = (int) util::from_view(val);
      }
      input.emplace_back(tmp);
    }
  }

  /**
   * @brief Consume an integer-pair list into a mapping table.
   *
   * @param vars Parsed configuration entries; consumed keys are erased.
   * @param name Configuration key to consume.
   * @param input Destination field updated when the setting exists and parses successfully.
   */
  void map_int_int_f(std::unordered_map<std::string, std::string> &vars, const std::string &name, std::unordered_map<int, int> &input) {
    std::vector<int> list;
    list_int_f(vars, name, list);

    // The list needs to be a multiple of 2
    if (list.size() % 2) {
      BOOST_LOG(warning) << "config: expected "sv << name << " to have a multiple of two elements --> not "sv << list.size();
      return;
    }

    int x = 0;
    while (x < list.size()) {
      auto key = list[x++];
      auto val = list[x++];

      input.emplace(key, val);
    }
  }

  /**
   * @brief Apply single-character command-line flags to the global Sunshine flags bitset.
   *
   * @param line Configuration line being parsed.
   * @return 0 when all flags are recognized; -1 when at least one flag is unknown.
   */
  int apply_flags(const char *line) {
    int ret = 0;
    while (*line != '\0') {
      switch (*line) {
        case '2':
          config::sunshine.flags[config::flag::FORCE_VIDEO_HEADER_REPLACE].flip();
          break;
        default:
          BOOST_LOG(warning) << "config: Unrecognized flag: ["sv << *line << ']' << std::endl;
          ret = -1;
      }

      ++line;
    }

    return ret;
  }

  /**
   * @brief Log parsed configuration entries and optionally record them as modified.
   */
  void log_config_settings(const std::unordered_map<std::string, std::string> &vars, bool save) {
    for (auto &[name, val] : vars) {
      bool is_redacted = std::ranges::find(config::redacted_config, name) != config::redacted_config.end();

      BOOST_LOG(info) << "config: '"sv << name << "' = "sv << (is_redacted ? "[redacted]" : val);

      if (save) {
        modified_config_settings[name] = val;
      }
    }
  }

  /**
   * @brief Apply parsed configuration entries to the global runtime configuration.
   *
   * @param vars Parsed configuration entries; consumed keys are erased.
   */
  void apply_config(std::unordered_map<std::string, std::string> &&vars) {
    log_config_settings(vars, true);

    int_f(vars, "min_threads", video.min_threads);
    string_f(vars, "sw_preset", video.sw.sw_preset);
    string_f(vars, "sw_tune", video.sw.sw_tune);
    int_between_f(vars, "sw_vbv_maxrate_percentage", video.sw.vbv_maxrate_percentage, {100, 400});
    int_between_f(vars, "sw_vbv_buffer_frames", video.sw.vbv_buffer_frames, {0, 16});
    int_between_f(vars, "sw_scenecut", video.sw.scenecut, {0, 100});

    int_between_f(vars, "nvenc_preset", video.nv.quality_preset, {1, 7});
    int_between_f(vars, "nvenc_vbv_increase", video.nv.vbv_percentage_increase, {0, 400});
    bool_f(vars, "nvenc_spatial_aq", video.nv.adaptive_quantization);
    generic_f(vars, "nvenc_twopass", video.nv.two_pass, nv::twopass_from_view);
    bool_f(vars, "nvenc_h264_cavlc", video.nv.h264_cavlc);
    generic_f(vars, "nvenc_split_encode", video.nv.split_frame_encoding, nv::split_encode_from_view);


    string_f(vars, "adapter_name", video.adapter_name);
    double_between_f(vars, "minimum_fps_target", video.minimum_fps_target, {0.0, 1000.0});

    path_f(vars, "pkey", nvhttp.pkey);
    path_f(vars, "cert", nvhttp.cert);
    string_f(vars, "host_name", nvhttp.host_name);
    bool_f(vars, "publish_session_user", nvhttp.publish_session_user);
    path_f(vars, "log_path", config::sunshine.log_file);
    path_f(vars, "file_state", nvhttp.file_state);

    // The root-owned PAM broker enforces this setting. Consume it here so the
    // media worker accepts the shared configuration without owning auth policy.
    bool broker_allow_root_login = false;
    bool_f(vars, "allow_root_login", broker_allow_root_login);

    string_f(vars, "audio_sink", audio.sink);

    int to = -1;
    int_between_f(vars, "ping_timeout", to, {-1, std::numeric_limits<int>::max()});
    if (to != -1) {
      stream.ping_timeout = std::chrono::milliseconds(to);
    }

    map_int_int_f(vars, "keybindings"s, input.keybindings);

    double repeat_frequency {0};
    double_between_f(vars, "key_repeat_frequency", repeat_frequency, {0, std::numeric_limits<double>::max()});

    if (repeat_frequency > 0) {
      config::input.key_repeat_period = std::chrono::duration<double> {1 / repeat_frequency};
    }

    to = -1;
    int_f(vars, "key_repeat_delay", to);
    if (to >= 0) {
      input.key_repeat_delay = std::chrono::milliseconds {to};
    }

    bool_f(vars, "mdns_discovery", sunshine.mdns_discovery);
    string_restricted_f(vars, "startup_layout", sunshine.startup_layout, {"physical"sv, "virtual"sv});

    int port = sunshine.port;
    int_between_f(vars, "port"s, port, {1024 - nvhttp::PORT_HTTPS, 65535});
    sunshine.port = (std::uint16_t) port;

    std::string log_level_string;
    string_f(vars, "min_log_level", log_level_string);

    if (!log_level_string.empty()) {
      if (log_level_string == "verbose"sv) {
        sunshine.min_log_level = 0;
      } else if (log_level_string == "debug"sv) {
        sunshine.min_log_level = 1;
      } else if (log_level_string == "info"sv) {
        sunshine.min_log_level = 2;
      } else if (log_level_string == "warning"sv) {
        sunshine.min_log_level = 3;
      } else if (log_level_string == "error"sv) {
        sunshine.min_log_level = 4;
      } else if (log_level_string == "fatal"sv) {
        sunshine.min_log_level = 5;
      } else if (log_level_string == "none"sv) {
        sunshine.min_log_level = 6;
      } else {
        // accept digit directly
        auto val = log_level_string[0];
        if (val >= '0' && val < '7') {
          sunshine.min_log_level = val - '0';
        }
      }
    }

    if (sunshine.min_log_level <= 3) {
      for (auto &[var, _] : vars) {
        std::cout << "Warning: Unrecognized configurable option ["sv << var << ']' << std::endl;
      }
    }
  }

  /**
   * @brief Parse serialized text into the corresponding runtime representation.
   */
  int parse(int argc, char *argv[]) {
    std::unordered_map<std::string, std::string> cmd_vars;

    for (auto x = 1; x < argc; ++x) {
      auto line = argv[x];

      if (line == "--help"sv) {
        logging::print_help(*argv);
        return 1;
      }
      else if (*line == '-') {
        if (*(line + 1) == '-') {
          sunshine.cmd.name = line + 2;
          sunshine.cmd.argc = argc - x - 1;
          sunshine.cmd.argv = argv + x + 1;

          break;
        }
        if (apply_flags(line + 1)) {
          logging::print_help(*argv);
          return -1;
        }
      } else {
        auto line_end = line + strlen(line);

        auto pos = std::find(line, line_end, '=');
        if (pos == line_end) {
          sunshine.config_file = line;
        } else {
          TUPLE_EL(var, 1, parse_option(line, line_end));
          if (!var) {
            logging::print_help(*argv);
            return -1;
          }

          TUPLE_EL_REF(name, 0, *var);

          auto it = cmd_vars.find(name);
          if (it != std::end(cmd_vars)) {
            cmd_vars.erase(it);
          }

          cmd_vars.emplace(std::move(*var));
        }
      }
    }

    bool config_loaded = false;
    try {
      // Create appdata folder if it does not exist
      file_handler::make_directory(platf::appdata().string());

      // A packaged PLANK host has one administrator-owned configuration source.
      if (!fs::exists(sunshine.config_file)) {
#ifdef PLANK_PRODUCT_BUILD
        throw std::runtime_error("PLANK host configuration does not exist: " + sunshine.config_file);
#else
        std::ofstream {sunshine.config_file};
#endif
      }

      // Read config file
      auto vars = parse_config(file_handler::read_file(sunshine.config_file.c_str()));

      for (auto &[name, value] : cmd_vars) {
        vars.insert_or_assign(std::move(name), std::move(value));
      }

      // Apply the config. Note: This will try to create any paths
      // referenced in the config, so we may receive exceptions if
      // the path is incorrect or inaccessible.
      apply_config(std::move(vars));
      config_loaded = true;
    } catch (const std::filesystem::filesystem_error &err) {
      BOOST_LOG(fatal) << "Failed to apply config: "sv << err.what();
    } catch (const boost::filesystem::filesystem_error &err) {
      BOOST_LOG(fatal) << "Failed to apply config: "sv << err.what();
    }

    if (!config_loaded) {
      return -1;
    }


    return 0;
  }
}  // namespace config
