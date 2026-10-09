/** @file Owner-bound Linux recovery records within the shared machine state. */
#pragma once

#include "state_storage.h"
#include "platform/linux/private_display_snapshot_policy.h"

#include <boost/property_tree/ptree.hpp>
#include <map>
#include <nlohmann/json.hpp>

namespace statefile::linux_display_snapshot_storage {
  using records_t = std::map<std::string, std::string>;
  namespace pt = boost::property_tree;

  inline std::string key(const std::string &owner) {
    constexpr char digits[] = "0123456789abcdef";
    std::string result;
    for (const unsigned char value : owner) {
      result.push_back(digits[value >> 4]);
      result.push_back(digits[value & 15]);
    }
    return result;
  }

  inline std::optional<std::string> record_owner(const std::string &contents) {
    if (contents.empty() || contents.size() > 1024 * 1024) return std::nullopt;
    try {
      const auto record = nlohmann::json::parse(contents);
      const auto owner = record.value("owner", std::string {});
      if (owner.empty() || owner.size() > 128 ||
          !platf::linux_private_display::snapshot_policy::decode<nlohmann::json>(contents, owner)) return std::nullopt;
      return owner;
    } catch (...) {
      return std::nullopt;
    }
  }

  /** Validate every existing record before selecting or migrating any owner. */
  inline std::optional<records_t> collect(const pt::ptree &tree) {
    records_t records;
    if (const auto entries = tree.get_child_optional("root.linux_display_topologies")) {
      if (!entries->data().empty() || entries->size() > 64) return std::nullopt;
      for (const auto &[encoded_owner, node] : *entries) {
        if (!node.empty()) return std::nullopt;
        const auto owner = record_owner(node.data());
        if (!owner || key(*owner) != encoded_owner || !records.emplace(*owner, node.data()).second) return std::nullopt;
      }
    }
    if (const auto legacy = tree.get_child_optional("root.linux_display_topology")) {
      if (!legacy->empty()) return std::nullopt;
      const auto owner = record_owner(legacy->data());
      if (!owner) return std::nullopt;
      const auto [existing, inserted] = records.emplace(*owner, legacy->data());
      // A contradictory legacy record remains an obligation, not a fallback
      // that a newer entry may silently supersede.
      if (!inserted && existing->second != legacy->data()) return std::nullopt;
    }
    return records;
  }

  inline linux_display_snapshot_read_result_t read(const pt::ptree &tree, const std::string &owner) {
    if (owner.empty() || owner.size() > 128) return {};
    const auto records = collect(tree);
    if (!records) return {};
    const auto record = records->find(owner);
    if (record == records->end()) return {linux_display_snapshot_status_e::missing, {}};
    return {linux_display_snapshot_status_e::loaded, record->second};
  }

  /** Change one owner only; keep foreign intent even across greeter/desktop reboot. */
  inline bool update(pt::ptree &tree, const std::string &owner, const std::optional<std::string> &snapshot) {
    if (owner.empty() || owner.size() > 128) return false;
    auto records = collect(tree);
    if (!records) return false;
    if (snapshot) {
      const auto saved_owner = record_owner(*snapshot);
      if (!saved_owner || *saved_owner != owner) return false;
      records->insert_or_assign(owner, *snapshot);
    } else {
      records->erase(owner);
    }
    if (records->size() > 64) return false;
    pt::ptree entries;
    for (const auto &[saved_owner, contents] : *records) {
      pt::ptree node;
      node.put_value(contents);
      entries.push_back({key(saved_owner), std::move(node)});
    }
    tree.put_child("root.linux_display_topologies", entries);
    tree.get_child("root").erase("linux_display_topology");
    return true;
  }
}  // namespace statefile::linux_display_snapshot_storage
