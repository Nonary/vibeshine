/**
 * @file src/platform/linux/input/inputtino_sc26_policy.h
 * @brief Kernel-free logic of the Linux Steam Controller (2026) profile.
 * @details Everything here is plain data in, report bytes out, so it can be tested without
 *          /dev/uhid: profile selection, the client-state to `lvg::sc26_usb::state` adapter
 *          (a port of libvirtualgamepad's `driver/src/steam_controller.cpp`, which cannot be
 *          compiled on Linux because its protocol header includes <windows.h>), touch routing
 *          for one- and two-pad clients, the control-channel command filter and the haptic
 *          and settings feedback the device hands to the stream.
 */
#pragma once

// standard includes
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <map>
#include <optional>

// lib includes
#include <libvirtualgamepad/sc26_usb.h>

// local includes
#include "src/steam_controller_protocol.h"

namespace platf::gamepad::sc26 {
  namespace sc = lvg::sc26_usb;

  /**
   * @brief Capabilities only a client reading a real Steam Controller (2026) sends.
   * @details `LI_CTYPE_STEAM` alone is not enough: a Steam Deck's built-in controls arrive with
   *          that type too and should keep the DualSense profile. Same gate as Windows'
   *          `vhf_gamepad::is_steam_controller_client`.
   */
  inline constexpr std::uint16_t steam_controller_client_caps = LI_CCAP_GRIP_SENSE | LI_CCAP_STEAM_HAPTIC | LI_CCAP_SC26_BUTTONS;

  [[nodiscard]] constexpr bool is_steam_controller_client(const std::uint8_t type, const std::uint16_t capabilities) noexcept {
    return type == LI_CTYPE_STEAM && (capabilities & steam_controller_client_caps) != 0;
  }

  /// Rate the client is asked to stream motion at: the state report carries the IMU at 250 Hz.
  inline constexpr std::uint16_t motion_rate_hz = 250;

  /// Interval of the state report. Steam's stick test stalls on bursty reports, so the device
  /// sends on a fixed clock and never in reaction to input.
  inline constexpr std::uint32_t report_interval_us = 4000;
  /// Interval of the battery report, as the real unit sends it.
  inline constexpr std::uint32_t battery_interval_us = 3500000;
  /// A synthesized rumble stops when Steam stops refreshing it (the unit's own safety timeout is ~50 ms).
  inline constexpr std::uint32_t synthesized_rumble_timeout_us = 100000;

  /**
   * @brief Firmware build time the device reports until the real controller's own arrives.
   * @details This is the build of the controller firmware Steam ships (IBEX_FW_6AE90BC5.fw,
   *          TRITON_FW_TS in Steam's hardwareupdater.cfg). Steam's hardware updater lists a wired
   *          unit for an update whenever its build time differs from that file's, a newer one
   *          included (offered as a downgrade), so only the exact value keeps Steam from
   *          offering an update for the virtual device. The date captured from a real unit
   *          (sc::captured_firmware_build_time) made Steam start one on every new host. When
   *          Steam ships newer firmware the offer comes back until this value is bumped; it stays
   *          harmless, because the update starts with the bootloader command 0x90, which
   *          set_feature() refuses.
   */
  inline constexpr std::uint32_t steam_firmware_build_time = 0x6AE90BC5;

  [[nodiscard]] inline sc::feature_state default_features() noexcept {
    sc::feature_state features {};
    features.attrs.firmware_build_time = steam_firmware_build_time;
    return features;
  }

  /**
   * @brief Per-controller state behind the virtual device.
   */
  struct pad_t {
    sc::state device {};
    sc::feature_state features = default_features();
    /// The arrival advertised LI_CCAP_SC26_BUTTONS: clicks and capacitive states are authoritative.
    bool sc26_explicit {};
    /// The arrival advertised LI_CCAP_DUAL_TOUCHPAD: the touch event's pad index picks the pad.
    bool dual_touchpad {};
    bool grip_explicit {};
    bool stick_touch_explicit {};
    /// One-pad clients: which pad each pointer went down on.
    std::map<std::uint32_t, std::uint8_t> pointer_pad;
  };

  /**
   * @brief Translates Vibeshine's button flags into the controller's button word.
   * @details Paddle order follows the Moonlight Steam Controller drivers (R4, L4, R5, L5). Start
   *          and Back go to the bits SDL's Triton driver reads them from (VIEW 0x40 is start,
   *          MENU 0x4000 is back), not the bits their names suggest, or Steam swaps them.
   */
  [[nodiscard]] inline std::uint32_t map_buttons(const std::uint32_t flags, const pad_t &pad) noexcept {
    struct pair_t {
      std::uint32_t from;
      std::uint32_t to;
    };

    constexpr pair_t map[] = {
      {A_FLAG, sc::btn_a},
      {B_FLAG, sc::btn_b},
      {X_FLAG, sc::btn_x},
      {Y_FLAG, sc::btn_y},
      {UP_FLAG, sc::btn_dpad_up},
      {DOWN_FLAG, sc::btn_dpad_down},
      {LEFT_FLAG, sc::btn_dpad_left},
      {RIGHT_FLAG, sc::btn_dpad_right},
      {PLAY_FLAG, sc::btn_view},
      {BACK_FLAG, sc::btn_menu},
      {LS_CLK_FLAG, sc::btn_l3},
      {RS_CLK_FLAG, sc::btn_r3},
      {LB_FLAG, sc::btn_l},
      {RB_FLAG, sc::btn_r},
      {SPECIAL_FLAG, sc::btn_steam},
      {MISC_FLAG, sc::btn_qam},
      {PADDLE1_FLAG, sc::btn_r4},
      {PADDLE2_FLAG, sc::btn_l4},
      {PADDLE3_FLAG, sc::btn_r5},
      {PADDLE4_FLAG, sc::btn_l5},
    };

    std::uint32_t out = 0;
    for (const auto &p : map) {
      if (flags & p.from) {
        out |= p.to;
      }
    }

    if (pad.sc26_explicit) {
      if (flags & LEFT_PAD_CLICK_FLAG) {
        out |= sc::btn_left_pad_click;
      }
      if (flags & RIGHT_PAD_CLICK_FLAG) {
        out |= sc::btn_right_pad_click;
      }
      if (flags & LEFT_TRIGGER_CLICK_FLAG) {
        out |= sc::btn_left_trigger_click;
      }
      if (flags & RIGHT_TRIGGER_CLICK_FLAG) {
        out |= sc::btn_right_trigger_click;
      }
    } else if (flags & TOUCHPAD_FLAG) {
      // One click flag for two pads: the click lands on the pad under a finger, both when
      // both are touched, and the right pad when neither is, so it still registers.
      const bool left = pad.device.pad_touched[0];
      const bool right = pad.device.pad_touched[1];
      if (left) {
        out |= sc::btn_left_pad_click;
      }
      if (right || !left) {
        out |= sc::btn_right_pad_click;
      }
    }
    return out;
  }

  /**
   * @brief Encodes the 0x42 state report from the latest client state.
   * @param now_us Microsecond clock; Steam integrates the gyro over its deltas.
   */
  [[nodiscard]] inline sc::input_report encode_input(
    pad_t &pad,
    const std::uint32_t flags,
    const std::int16_t left_x,
    const std::int16_t left_y,
    const std::int16_t right_x,
    const std::int16_t right_y,
    const std::uint8_t left_trigger,
    const std::uint8_t right_trigger,
    const std::uint64_t now_us
  ) noexcept {
    auto &st = pad.device;
    st.imu_timestamp = static_cast<std::uint32_t>(now_us);

    if (pad.sc26_explicit) {
      pad.grip_explicit = true;
      pad.stick_touch_explicit = true;
    }
    st.trigger_click_from_analog = !pad.sc26_explicit;

    // Grip and stick touch arrive as button bits. The first one seen makes them authoritative
    // for good; before that, grips read released and a deflected stick reads touched.
    if (flags & (LEFT_GRIP_TOUCH_FLAG | RIGHT_GRIP_TOUCH_FLAG)) {
      pad.grip_explicit = true;
    }
    st.grip_touch[0] = pad.grip_explicit && (flags & LEFT_GRIP_TOUCH_FLAG);
    st.grip_touch[1] = pad.grip_explicit && (flags & RIGHT_GRIP_TOUCH_FLAG);

    if (flags & (LEFT_STICK_TOUCH_FLAG | RIGHT_STICK_TOUCH_FLAG)) {
      pad.stick_touch_explicit = true;
    }
    if (pad.stick_touch_explicit) {
      st.stick_touch_from_deflection = false;
      st.stick_touch[0] = (flags & LEFT_STICK_TOUCH_FLAG) != 0;
      st.stick_touch[1] = (flags & RIGHT_STICK_TOUCH_FLAG) != 0;
    }

    return sc::encode_input(map_buttons(flags, pad), left_x, left_y, right_x, right_y, left_trigger, right_trigger, st);
  }

  [[nodiscard]] inline std::uint16_t normalized_touch(const float value) noexcept {
    if (!(value > 0.0f)) {  // Also catches NaN.
      return 0;
    }
    if (value >= 1.0f) {
      return 65535;
    }
    return static_cast<std::uint16_t>(value * 65535.0f);
  }

  /**
   * @brief Applies one touch event from the client to the pads.
   * @details A two-pad client (LI_CCAP_DUAL_TOUCHPAD) names the pad in `touchpad_index`. A
   *          one-pad client sends both pads as one DualShock-style pad: the half a finger went
   *          down on picks the pad for that pointer's moves and release (a release carries no
   *          position), and each half is stretched back to a full pad.
   * @return True when the pad state changed.
   */
  inline bool apply_touch(
    pad_t &pad,
    const std::uint8_t event,
    const std::uint32_t pointer,
    const std::uint8_t touchpad_index,
    float x,
    const float y,
    const float pressure
  ) noexcept {
    auto &st = pad.device;
    if (event == LI_TOUCH_EVENT_CANCEL_ALL) {
      pad.pointer_pad.clear();
      st.pad_touched[0] = st.pad_touched[1] = false;
      st.pad_pressure[0] = st.pad_pressure[1] = 0;
      return true;
    }
    if (event != LI_TOUCH_EVENT_DOWN && event != LI_TOUCH_EVENT_MOVE && event != LI_TOUCH_EVENT_HOVER &&
        event != LI_TOUCH_EVENT_UP && event != LI_TOUCH_EVENT_CANCEL) {
      return false;
    }
    const bool release = event == LI_TOUCH_EVENT_UP || event == LI_TOUCH_EVENT_CANCEL;

    std::uint8_t contact;
    if (pad.dual_touchpad) {
      contact = touchpad_index != 0 ? 1 : 0;
    } else {
      auto existing = pad.pointer_pad.find(pointer);
      if (existing == pad.pointer_pad.end()) {
        if (event != LI_TOUCH_EVENT_DOWN || !std::isfinite(x)) {
          return false;
        }
        const std::uint8_t half = x >= 0.5f ? 1 : 0;
        for (const auto &[id, occupied] : pad.pointer_pad) {
          if (occupied == half) {
            return false;  // Each pad is single touch.
          }
        }
        existing = pad.pointer_pad.emplace(pointer, half).first;
      }
      contact = existing->second;
      if (release) {
        pad.pointer_pad.erase(existing);
      }
      x = std::clamp(contact ? (x - 0.5f) * 2.0f : x * 2.0f, 0.0f, 1.0f);
    }

    if (release) {
      st.pad_touched[contact] = false;
      st.pad_pressure[contact] = 0;
      return true;
    }
    // The pads have no hover state: a hover or move only updates a contact that is down.
    if (event != LI_TOUCH_EVENT_DOWN && !st.pad_touched[contact]) {
      return false;
    }
    st.pad_touched[contact] = true;
    st.pad_x[contact] = normalized_touch(x);
    st.pad_y[contact] = normalized_touch(y);
    st.pad_pressure[contact] = normalized_touch(pressure);
    return true;
  }

  /**
   * @brief Applies a motion sample (accelerometer m/s^2, gyroscope deg/s, SDL axes).
   */
  inline void apply_motion(pad_t &pad, const std::uint8_t type, const float x, const float y, const float z) noexcept {
    const auto milli = [](const float value) -> std::int32_t {
      if (!std::isfinite(value)) {
        return 0;
      }
      return static_cast<std::int32_t>(std::lround(std::clamp(value, -2.0e6f, 2.0e6f) * 1000.0f));
    };
    if (type == LI_MOTION_TYPE_ACCEL) {
      sc::apply_accel_milli(pad.device, milli(x), milli(y), milli(z));
    } else if (type == LI_MOTION_TYPE_GYRO) {
      sc::apply_gyro_milli(pad.device, milli(x), milli(y), milli(z));
    }
  }

  inline void apply_battery(pad_t &pad, const std::uint8_t state, const std::uint8_t percentage) noexcept {
    auto &st = pad.device;
    // LI_BATTERY_PERCENTAGE_UNKNOWN keeps the last known level instead of reporting a full battery.
    if (percentage <= 100) {
      st.battery_percent = percentage;
    }
    switch (state) {
      case LI_BATTERY_STATE_CHARGING:
        st.charge_state = sc::charge_charging;
        break;
      case LI_BATTERY_STATE_FULL:
        st.charge_state = sc::charge_done;
        st.battery_percent = 100;
        break;
      default:
        st.charge_state = sc::charge_discharging;
        break;
    }
  }

  /// The `LI_SCATTR_*` bits the virtual device has a field for.
  inline constexpr std::uint32_t attributes_applied_mask =
    LI_SCATTR_FIRMWARE_BUILD_TIME | LI_SCATTR_BOOTLOADER_BUILD_TIME | LI_SCATTR_BOARD_REVISION |
    LI_SCATTR_CAPABILITIES | LI_SCATTR_BUILD_ID | LI_SCATTR_SERIALS;
  static_assert(sc::attr_update_firmware_build_time == LI_SCATTR_FIRMWARE_BUILD_TIME);
  static_assert(sc::attr_update_bootloader_build_time == LI_SCATTR_BOOTLOADER_BUILD_TIME);
  static_assert(sc::attr_update_board_revision == LI_SCATTR_BOARD_REVISION);
  static_assert(sc::attr_update_capabilities == LI_SCATTR_CAPABILITIES);
  static_assert(sc::attr_update_build_id == LI_SCATTR_BUILD_ID);
  static_assert(sc::attr_update_serials == LI_SCATTR_SERIALS);

  /**
   * @brief Hands the real controller's firmware attributes to the control-channel responder.
   * @return The `LI_SCATTR_*` bits that were applied.
   */
  inline std::uint32_t apply_attributes(
    pad_t &pad,
    const std::uint8_t valid_mask,
    const std::uint32_t firmware_build_time,
    const std::uint32_t bootloader_build_time,
    const std::uint32_t board_revision,
    const std::uint32_t capabilities,
    const std::array<char, 16> &build_id,
    const std::array<char, 16> &unit_serial,
    const std::array<char, 16> &board_serial
  ) noexcept {
    sc::attribute_update update {};
    update.flags = valid_mask & attributes_applied_mask;
    update.firmware_build_time = firmware_build_time;
    update.bootloader_build_time = bootloader_build_time;
    update.board_revision = board_revision;
    update.capabilities = capabilities;
    // The client NUL-terminates; the last byte is forced anyway so a bad packet cannot run over.
    const auto copy = [](char (&out)[16], const std::array<char, 16> &in) {
      std::memcpy(out, in.data(), sizeof(out));
      out[sizeof(out) - 1] = '\0';
    };
    copy(update.build_id, build_id);
    copy(update.unit_serial, unit_serial);
    copy(update.board_serial, board_serial);
    return sc::apply_attributes(update, pad.features);
  }

  /**
   * @brief The control-channel commands the virtual controller takes.
   * @details These are the commands the responder in `sc26_usb.h` implements, which are the
   *          ones a real unit saw from Steam in the captured handshake plus the public settings
   *          queries. Everything else is refused, firmware updates first: a virtual device has
   *          no firmware, and an update Steam started here could never reach the real controller.
   */
  [[nodiscard]] constexpr bool is_known_command(const std::uint8_t command) noexcept {
    switch (command) {
      case sc::cmd_clear_digital_mappings:
      case sc::cmd_get_attributes_values:
      case sc::cmd_set_default_digital_mappings:
      case sc::cmd_set_settings_values:
      case sc::cmd_get_settings_values:
      case sc::cmd_get_settings_maxs:
      case sc::cmd_get_settings_defaults:
      case sc::cmd_load_default_settings:
      case sc::cmd_trigger_haptic_pulse:
      case sc::cmd_get_string_attribute:
      case sc::cmd_write_c1:
      case sc::cmd_write_dc:
      case sc::cmd_write_e2:
      case sc::cmd_get_keyed_value:
      case sc::cmd_get_device_info:
        return true;
      default:
        return false;
    }
  }

  enum class feature_result_e {
    accepted,  ///< Handled; the reply waits for the next GetFeature.
    refused,  ///< A command the virtual controller does not take; the host gets an I/O error.
    malformed,  ///< Not a control-channel transfer, or its length field runs past the data.
  };

  /**
   * @brief Handles a SetFeature on feature report 1. `data` starts at the report id.
   */
  [[nodiscard]] inline feature_result_e set_feature(pad_t &pad, const std::uint8_t *data, const std::size_t size) noexcept {
    if (data == nullptr || size < 3 || data[0] != sc::features_report_id) {
      return feature_result_e::malformed;
    }
    if (!is_known_command(data[1])) {
      // A GetFeature after a refused command must not return the previous command's reply.
      pad.features.have_reply = false;
      pad.features.last_unknown_command = data[1];
      ++pad.features.unknown_commands;
      return feature_result_e::refused;
    }
    return sc::set_feature(data, size, pad.features) ? feature_result_e::accepted : feature_result_e::malformed;
  }

  /**
   * @brief A haptic output report as the client replays it (control packet 0x5504).
   */
  struct haptic_report_t {
    std::uint8_t length {};
    std::array<std::uint8_t, STEAM_HAPTIC_REPORT_MAX> report {};
  };

  /**
   * @brief Picks the haptic output reports a LI_CCAP_STEAM_HAPTIC client replays on its pads.
   * @details 0x80 rumble through 0x85 script, cut to the descriptor's length; a truncated
   *          report is dropped rather than forwarded. 0x86..0x89 have no known meaning.
   */
  [[nodiscard]] inline std::optional<haptic_report_t> forwarded_haptic(const std::uint8_t *data, const std::size_t size) noexcept {
    if (data == nullptr || size < 2 || data[0] < sc::haptic_rumble_id || data[0] > sc::haptic_script_id) {
      return std::nullopt;
    }
    const std::size_t length = sc::report_size(data[0]);
    if (length == 0 || size < length || length > STEAM_HAPTIC_REPORT_MAX) {
      return std::nullopt;
    }
    haptic_report_t out;
    out.length = static_cast<std::uint8_t>(length);
    std::memcpy(out.report.data(), data, length);
    return out;
  }

  /**
   * @brief A settings write as a LI_CCAP_STEAM_SETTINGS client applies it (control packet 0x5505).
   */
  struct settings_chunk_t {
    std::uint8_t command {};
    std::uint8_t count {};
    std::array<std::uint8_t, STEAM_SETTINGS_PAYLOAD_SIZE> triples {};
  };

  /**
   * @brief Splits a SET_SETTINGS_VALUES (or LOAD_DEFAULT_SETTINGS) into client-sized chunks.
   * @return The number of chunks written to `out` (0 for any other transfer).
   */
  inline std::size_t settings_chunks(const std::uint8_t *data, const std::size_t size, std::array<settings_chunk_t, 3> &out) noexcept {
    if (data == nullptr || size < 3 || data[0] != sc::features_report_id) {
      return 0;
    }
    const std::uint8_t command = data[1];
    const std::size_t length = data[2];
    if (length > size - 3) {
      return 0;
    }
    if (command == sc::cmd_load_default_settings) {
      out[0] = {};
      out[0].command = command;
      return 1;
    }
    if (command != sc::cmd_set_settings_values) {
      return 0;
    }
    const std::size_t triples = length / 3;
    const std::size_t needed = (triples + STEAM_SETTINGS_TRIPLES_MAX - 1) / STEAM_SETTINGS_TRIPLES_MAX;
    if (triples == 0 || needed > out.size()) {
      return 0;
    }
    for (std::size_t n = 0; n < needed; ++n) {
      const std::size_t first = n * STEAM_SETTINGS_TRIPLES_MAX;
      const std::size_t count = std::min<std::size_t>(triples - first, STEAM_SETTINGS_TRIPLES_MAX);
      out[n] = {};
      out[n].command = command;
      out[n].count = static_cast<std::uint8_t>(count);
      std::memcpy(out[n].triples.data(), data + 3 + first * 3, count * 3);
    }
    return needed;
  }

  [[nodiscard]] inline const char *command_name(const std::uint8_t command) noexcept {
    switch (command) {
      case sc::cmd_clear_digital_mappings:
        return "CLEAR_DIGITAL_MAPPINGS";
      case sc::cmd_get_attributes_values:
        return "GET_ATTRIBUTES_VALUES";
      case sc::cmd_set_default_digital_mappings:
        return "SET_DEFAULT_DIGITAL_MAPPINGS";
      case sc::cmd_set_settings_values:
        return "SET_SETTINGS_VALUES";
      case sc::cmd_load_default_settings:
        return "LOAD_DEFAULT_SETTINGS";
      case sc::cmd_get_string_attribute:
        return "GET_STRING_ATTRIBUTE";
      case sc::cmd_get_keyed_value:
        return "GET_KEYED_VALUE";
      case sc::cmd_get_device_info:
        return "GET_DEVICE_INFO";
      default:
        return "unknown";
    }
  }
}  // namespace platf::gamepad::sc26
