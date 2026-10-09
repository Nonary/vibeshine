#!/usr/bin/env python3
"""Compile the actual Windows safety probe against deterministic OS boundaries.

Usage: python3 test_display_recovery_safety_probe.py [repository] [C++ compiler]
This covers adapter behavior without a Windows SDK. The fake EDID decoder only
supplies controlled manufacturer IDs; native CCD and EDID parsing need their
own tests. The production header is included unchanged.
"""
import pathlib
import subprocess
import sys
import tempfile


root = pathlib.Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else pathlib.Path(__file__).resolve().parents[2]
compiler = sys.argv[2] if len(sys.argv) > 2 else "c++"

recovery_header = r'''
#pragma once
namespace display_device {
  enum class DisplayRecoveryBehavior { Automatic, Skip };
  namespace detail {
    inline thread_local auto behavior = DisplayRecoveryBehavior::Automatic;
    inline auto current_display_recovery_behavior() { return behavior; }
  }
  struct DisplayRecoveryBehaviorGuard {
    DisplayRecoveryBehavior previous;
    explicit DisplayRecoveryBehaviorGuard(DisplayRecoveryBehavior value)
      : previous(detail::behavior) { detail::behavior = value; }
    ~DisplayRecoveryBehaviorGuard() { detail::behavior = previous; }
  };
}
'''

api_header = r'''
#pragma once
#include <display_device/windows/win_api_recovery.h>
using LONG = std::int32_t;
using DWORD = std::uint32_t;
using UINT32 = std::uint32_t;
constexpr int FALSE = 0;
constexpr UINT32 DISPLAYCONFIG_PATH_ACTIVE = 1;
enum DISPLAYCONFIG_VIDEO_OUTPUT_TECHNOLOGY {
  DISPLAYCONFIG_OUTPUT_TECHNOLOGY_HDMI = 5,
  DISPLAYCONFIG_OUTPUT_TECHNOLOGY_INDIRECT_WIRED = 16,
  DISPLAYCONFIG_OUTPUT_TECHNOLOGY_INDIRECT_VIRTUAL = 17,
};
struct LUID { DWORD LowPart = 0; LONG HighPart = 0; };
struct DISPLAYCONFIG_PATH_INFO {
  struct { UINT32 id = 0; } sourceInfo;
  struct {
    LUID adapterId;
    UINT32 id = 0;
    DISPLAYCONFIG_VIDEO_OUTPUT_TECHNOLOGY outputTechnology = DISPLAYCONFIG_OUTPUT_TECHNOLOGY_HDMI;
    int targetAvailable = 0;
  } targetInfo;
  UINT32 flags = 0;
};
namespace display_device {
  enum class QueryType { Active, All };
  struct PathAndModeData { std::vector<DISPLAYCONFIG_PATH_INFO> m_paths; };
  struct EdidData {
    std::string m_manufacturer_id;
    static std::optional<EdidData> parse(const std::vector<std::byte> &bytes) {
      if (bytes.empty()) return std::nullopt;
      std::string manufacturer;
      for (auto byte : bytes) manufacturer += static_cast<char>(byte);
      return EdidData {manufacturer};
    }
  };
  using TargetKey = std::tuple<LONG, DWORD, UINT32>;
  inline TargetKey key(const DISPLAYCONFIG_PATH_INFO &path) {
    return {path.targetInfo.adapterId.HighPart, path.targetInfo.adapterId.LowPart, path.targetInfo.id};
  }
  struct WinApiLayerInterface {
    enum class ThrowAt { None, Query, Path, Edid, Id };
    std::optional<PathAndModeData> data {PathAndModeData {}};
    std::map<TargetKey, std::pair<std::string, std::string>> identities;
    std::map<TargetKey, std::string> device_ids;
    mutable std::map<TargetKey, unsigned> path_reads, edid_reads;
    mutable unsigned queries = 0;
    ThrowAt throw_at = ThrowAt::None;

    void boundary(ThrowAt location) const {
      assert(detail::current_display_recovery_behavior() == DisplayRecoveryBehavior::Skip);
      if (throw_at == location) throw std::runtime_error("simulated OS failure");
    }
    std::optional<PathAndModeData> queryDisplayConfig(QueryType type) const {
      assert(type == QueryType::All);
      ++queries;
      boundary(ThrowAt::Query);
      return data;
    }
    std::string getMonitorDevicePath(const DISPLAYCONFIG_PATH_INFO &path) const {
      ++path_reads[key(path)];
      boundary(ThrowAt::Path);
      auto found = identities.find(key(path));
      return found == identities.end() ? std::string {} : found->second.first;
    }
    std::string getDeviceId(const DISPLAYCONFIG_PATH_INFO &path) const {
      boundary(ThrowAt::Id);
      auto found = device_ids.find(key(path));
      return found == device_ids.end() ? std::string {} : found->second;
    }
    std::vector<std::byte> getEdid(const DISPLAYCONFIG_PATH_INFO &path) const {
      ++edid_reads[key(path)];
      boundary(ThrowAt::Edid);
      std::vector<std::byte> result;
      if (auto found = identities.find(key(path)); found != identities.end()) {
        for (unsigned char value : found->second.second) result.push_back(static_cast<std::byte>(value));
      }
      return result;
    }
  };
}
'''

program = r'''
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <map>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>
// Set after standard-library includes so the host library keeps its native ABI.
#ifndef _WIN32
#define _WIN32
#endif
#include "src/platform/windows/display_recovery_safety.h"

using Api = display_device::WinApiLayerInterface;
using State = display_recovery_safety::PhysicalDisplayState;
using Behavior = display_device::DisplayRecoveryBehavior;

DISPLAYCONFIG_PATH_INFO path(bool available, bool active,
    DISPLAYCONFIG_VIDEO_OUTPUT_TECHNOLOGY technology = DISPLAYCONFIG_OUTPUT_TECHNOLOGY_HDMI) {
  DISPLAYCONFIG_PATH_INFO value;
  value.targetInfo.adapterId = {41, -7};
  value.targetInfo.id = 8;
  value.targetInfo.targetAvailable = available;
  value.targetInfo.outputTechnology = technology;
  value.flags = active ? DISPLAYCONFIG_PATH_ACTIVE : 0;
  return value;
}

unsigned cases = 0;
void check(Api &api, State expected) {
  const auto original = display_device::detail::current_display_recovery_behavior();
  const auto queries = api.queries;
  assert(display_recovery_safety::probe_physical_displays(api) == expected);
  assert(api.queries == queries + 1);
  assert(display_device::detail::current_display_recovery_behavior() == original);
  ++cases;
}

unsigned target_cases = 0;
using Presence = display_recovery_safety::DisplayTargetPresence;
void check_target(Api &api, Presence expected, std::string_view id = "virtual", std::string_view monitor_path = "virtual_path") {
  const auto original = display_device::detail::current_display_recovery_behavior();
  const auto queries = api.queries;
  assert(display_recovery_safety::probe_display_target_presence(api, id, monitor_path) == expected);
  assert(api.queries == queries + 1);
  assert(display_device::detail::current_display_recovery_behavior() == original);
  ++target_cases;
}

int main() {
  Api api;
  // Neither empty nor partial high-level enumeration proves target absence.
  check_target(api, Presence::missing);
  api.data = std::nullopt;
  check_target(api, Presence::unknown);
  api = Api {};
  api.throw_at = Api::ThrowAt::Query;
  check_target(api, Presence::unknown);
  for (auto target : {path(true, false), path(true, true), path(false, true)}) {
    api = Api {};
    api.data->m_paths = {target};
    check_target(api, Presence::unknown);
  }
  api = Api {};
  api.data->m_paths = {path(false, false)};
  check_target(api, Presence::missing);
  const auto virtual_path = path(true, true);
  const auto virtual_key = display_device::key(virtual_path);
  api.data->m_paths = {virtual_path};
  api.device_ids[virtual_key] = "VIRTUAL";
  api.identities[virtual_key] = {"VIRTUAL_PATH", "SDD"};
  check_target(api, Presence::present_active); // Case-insensitive stable identity.
  api.data->m_paths.front().flags = 0;
  check_target(api, Presence::present_inactive);
  api.data->m_paths.front().flags = DISPLAYCONFIG_PATH_ACTIVE;
  api.device_ids[virtual_key] = "fallback-hash";
  check_target(api, Presence::present_active); // Stable path survives an ID change.
  check_target(api, Presence::unknown, "virtual", ""); // Hash alone cannot prove absence.
  api.device_ids.clear();
  check_target(api, Presence::present_active, "", "virtual_path");
  check_target(api, Presence::unknown, "", "");
  for (auto failure : {Api::ThrowAt::Id, Api::ThrowAt::Path}) {
    api.throw_at = failure;
    check_target(api, Presence::unknown);
  }
  api = Api {};
  auto physical_path = path(true, true);
  ++physical_path.targetInfo.id;
  const auto physical_key = display_device::key(physical_path);
  api.data->m_paths = {physical_path};
  api.device_ids[physical_key] = "physical";
  api.identities[physical_key] = {"physical_path", "DEL"};
  check_target(api, Presence::missing); // True VD loss, other physical output present.
  api.data->m_paths.push_back(virtual_path);
  check_target(api, Presence::unknown); // Partial identity failure cannot prove loss.
  api.device_ids[virtual_key] = "virtual";
  api.identities[virtual_key] = {"virtual_path", "SDD"};
  check_target(api, Presence::present_active);
  api.data->m_paths.back().targetInfo.targetAvailable = FALSE;
  check_target(api, Presence::unknown); // Stale ACTIVE cannot prove removal.
  api.data->m_paths.pop_back();
  api.identities[physical_key].first.clear();
  check_target(api, Presence::unknown); // Missing identity on a potential target.
  assert(display_device::detail::current_display_recovery_behavior() == Behavior::Automatic);
  api = Api {};
  check(api, State::none_connected);
  assert(api.path_reads.empty() && api.edid_reads.empty());
  api.data = std::nullopt;
  check(api, State::unknown);
  assert(api.path_reads.empty() && api.edid_reads.empty());

  for (auto failure : {Api::ThrowAt::Query, Api::ThrowAt::Path, Api::ThrowAt::Edid}) {
    api = Api {};
    api.data->m_paths = {path(true, false)};
    api.throw_at = failure;
    check(api, State::unknown);
    // Nested callers must retain Skip after both success and exception.
    display_device::DisplayRecoveryBehaviorGuard outer {Behavior::Skip};
    check(api, State::unknown);
  }
  assert(display_device::detail::current_display_recovery_behavior() == Behavior::Automatic);
  api = Api {};
  {
    display_device::DisplayRecoveryBehaviorGuard outer {Behavior::Skip};
    check(api, State::none_connected);
  }
  assert(display_device::detail::current_display_recovery_behavior() == Behavior::Automatic);

  api = Api {};
  const auto physical = path(true, false);
  api.data->m_paths = {physical};
  api.identities[display_device::key(physical)] = {R"(\\?\DISPLAY#DEL1234#one)", "DEL"};
  check(api, State::connected_inactive);
  api.identities.clear();
  check(api, State::connected_inactive); // Missing identity cannot prove headlessness.
  api.data->m_paths = {path(true, true)};
  check(api, State::active);
  api.data->m_paths = {path(false, true)};
  check(api, State::unknown);

  api = Api {};
  api.data->m_paths = {path(true, false, DISPLAYCONFIG_OUTPUT_TECHNOLOGY_INDIRECT_WIRED)};
  check(api, State::connected_inactive);
  api = Api {};
  api.data->m_paths = {path(true, true, DISPLAYCONFIG_OUTPUT_TECHNOLOGY_INDIRECT_VIRTUAL)};
  api.throw_at = Api::ThrowAt::Path;
  check(api, State::none_connected);
  assert(api.path_reads.empty() && api.edid_reads.empty());
  api.data->m_paths.front().targetInfo.targetAvailable = FALSE;
  check(api, State::none_connected); // Proven virtual departure stays virtual.

  for (const auto identity : {"SunshineVirtualDisplay", R"(\\?\display#sdd5001#one)", R"(\\?\DISPLAY#SMK1234#one)"}) {
    api = Api {};
    api.data->m_paths = {physical};
    api.identities[display_device::key(physical)] = {identity, ""};
    api.throw_at = Api::ThrowAt::Edid;
    check(api, State::none_connected);
    assert(api.edid_reads.empty());
  }
  for (const auto manufacturer : {"SDD", "smk"}) {
    api = Api {};
    api.data->m_paths = {physical};
    api.identities[display_device::key(physical)] = {"", manufacturer};
    check(api, State::none_connected);
    assert(api.path_reads.at(display_device::key(physical)) == 1);
    assert(api.edid_reads.at(display_device::key(physical)) == 1);
  }

  api = Api {};
  api.data->m_paths = {path(false, false)};
  api.throw_at = Api::ThrowAt::Path;
  check(api, State::none_connected);
  assert(api.path_reads.empty() && api.edid_reads.empty());

  // Alternative sources share a target identity; different adapters do not.
  api = Api {};
  auto alternative = physical;
  alternative.sourceInfo.id = 99;
  auto other_adapter = physical;
  ++other_adapter.targetInfo.adapterId.LowPart;
  api.data->m_paths = {physical, alternative, other_adapter};
  api.identities[display_device::key(physical)] = {"", "SDD"};
  check(api, State::connected_inactive);
  assert(api.path_reads.size() == 2 && api.edid_reads.size() == 2);
  for (const auto &[key, reads] : api.path_reads) {
    assert(reads == 1 && api.edid_reads.at(key) == 1);
  }
  // Cache lifetime ends with the query, including previously negative matches.
  api.identities[display_device::key(other_adapter)] = {"", "SMK"};
  check(api, State::none_connected);
  assert(api.edid_reads.at(display_device::key(other_adapter)) == 2);
  std::cout << cases << " production Windows safety-probe cases passed.\n";
  std::cout << target_cases << " production target-presence cases passed.\n";
}
'''

with tempfile.TemporaryDirectory(prefix="display-recovery-safety-probe-") as temporary:
    directory = pathlib.Path(temporary)
    includes = directory / "display_device/windows"
    includes.mkdir(parents=True)
    (includes / "win_api_recovery.h").write_text(recovery_header)
    (includes / "win_api_layer_interface.h").write_text(api_header)
    source = directory / "test.cpp"
    source.write_text(program)
    binary = directory / "test"
    subprocess.run([
        compiler, "-std=c++20", "-Wall", "-Wextra", "-Werror",
        "-I", str(directory), "-I", str(root), str(source), "-o", str(binary),
    ], check=True)
    subprocess.run([str(binary)], check=True)
