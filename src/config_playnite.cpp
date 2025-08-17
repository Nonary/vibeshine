/**
 * @file src/config_playnite.cpp
 * @brief Playnite integration configuration parsing.
 */

#include "config_playnite.h"

#include <algorithm>
#include <filesystem>
#include <sstream>
#include <string>
#include <vector>

#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>

#include "src/platform/common.h"
#include "src/logging.h"

namespace fs = std::filesystem;
using namespace std::literals;

namespace config {

  playnite_t playnite;

  static bool to_bool(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char) std::tolower(c); });
    return s == "true" || s == "yes" || s == "enable" || s == "enabled" || s == "on" || s == "1";
  }

  static void erase_take(std::unordered_map<std::string, std::string> &vars, const std::string &name, std::string &out) {
    auto it = vars.find(name);
    if (it == vars.end()) return;
    out = std::move(it->second);
    vars.erase(it);
  }

  static void parse_list(std::unordered_map<std::string, std::string> &vars, const std::string &name, std::vector<std::string> &out) {
    std::string raw;
    erase_take(vars, name, raw);
    if (raw.empty()) return;

    // Accept JSON array or simple comma-separated list
    try {
      std::stringstream ss;
      // If already an array, wrap as {"v": raw}
      ss << "{\"v\":" << raw << "}";
      boost::property_tree::ptree pt;
      boost::property_tree::read_json(ss, pt);
      out.clear();
      for (auto &child : pt.get_child("v"s)) {
        out.emplace_back(child.second.get_value<std::string>());
      }
      return;
    } catch (...) {
      // Fallback to comma-separated
    }

    out.clear();
    std::string item;
    std::stringstream ss(raw);
    while (std::getline(ss, item, ',')) {
      // trim
      item.erase(item.begin(), std::find_if(item.begin(), item.end(), [](unsigned char ch) { return !std::isspace(ch); }));
      item.erase(std::find_if(item.rbegin(), item.rend(), [](unsigned char ch) { return !std::isspace(ch); }).base(), item.end());
      if (!item.empty()) out.push_back(item);
    }
  }

  static void parse_path(std::unordered_map<std::string, std::string> &vars, const std::string &name, std::string &target) {
    std::string raw;
    erase_take(vars, name, raw);
    if (raw.empty()) return;
    fs::path p = raw;
    if (p.is_relative()) {
      p = platf::appdata() / p;
    }
    // Ensure directory exists
    auto dir = p;
    dir.remove_filename();
    if (!dir.empty() && !fs::exists(dir)) {
      fs::create_directories(dir);
    }
    target = p.string();
  }

  void apply_playnite(std::unordered_map<std::string, std::string> &vars) {
    // booleans
    std::string tmp;
    erase_take(vars, "playnite_enabled", tmp);
    if (!tmp.empty()) playnite.enabled = to_bool(tmp);
    tmp.clear();
    erase_take(vars, "playnite_auto_sync", tmp);
    if (!tmp.empty()) playnite.auto_sync = to_bool(tmp);

    // integers
    tmp.clear();
    erase_take(vars, "playnite_recent_games", tmp);
    if (!tmp.empty()) {
      try {
        playnite.recent_games = std::stoi(tmp);
      } catch (...) {
        BOOST_LOG(warning) << "config: invalid playnite_recent_games value: " << tmp;
      }
    }

    // lists
    parse_list(vars, "playnite_sync_categories", playnite.sync_categories);

    // paths
    parse_path(vars, "playnite_install_dir", playnite.install_dir);
    parse_path(vars, "playnite_extensions_dir", playnite.extensions_dir);
  }
}  // namespace config
