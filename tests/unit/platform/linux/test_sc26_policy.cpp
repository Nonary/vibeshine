// Standalone regression test for the Linux Steam Controller (2026) profile's kernel-free logic
// (src/platform/linux/input/inputtino_sc26_policy.h). No /dev/uhid needed.
#include "src/platform/linux/input/inputtino_sc26_policy.h"

#include <cstdio>
#include <vector>

namespace {
  int failures = 0;

  void check(const bool value, const char *message) {
    if (!value) {
      std::printf("FAIL: %s\n", message);
      ++failures;
    }
  }

  namespace sc = lvg::sc26_usb;
  using namespace platf::gamepad::sc26;

  constexpr std::uint16_t our_client_caps = LI_CCAP_ANALOG_TRIGGERS | LI_CCAP_RUMBLE | LI_CCAP_TOUCHPAD | LI_CCAP_BATTERY_STATE |
                                            LI_CCAP_GRIP_SENSE | LI_CCAP_STICK_TOUCH | LI_CCAP_STEAM_HAPTIC | LI_CCAP_SC26_BUTTONS |
                                            LI_CCAP_STEAM_ATTRIBUTES | LI_CCAP_GYRO | LI_CCAP_ACCEL;

  sc::input_report encode(pad_t &pad, const std::uint32_t flags, const std::int16_t lx = 0, const std::int16_t ly = 0, const std::uint8_t lt = 0) {
    return encode_input(pad, flags, lx, ly, 0, 0, lt, 0, 1000);
  }

  std::vector<std::uint8_t> transfer(const std::uint8_t command, const std::vector<std::uint8_t> &payload) {
    std::vector<std::uint8_t> data(sc::feature_report_size, 0);
    data[0] = sc::features_report_id;
    data[1] = command;
    data[2] = static_cast<std::uint8_t>(payload.size());
    std::copy(payload.begin(), payload.end(), data.begin() + 3);
    return data;
  }

  feature_result_e send(pad_t &pad, const std::uint8_t command, const std::vector<std::uint8_t> &payload = {}) {
    const auto data = transfer(command, payload);
    return set_feature(pad, data.data(), data.size());
  }

  void test_selection() {
    check(is_steam_controller_client(LI_CTYPE_STEAM, our_client_caps), "the moonlight-qt Steam Controller client selects the profile");
    check(!is_steam_controller_client(LI_CTYPE_STEAM, LI_CCAP_ACCEL | LI_CCAP_GYRO | LI_CCAP_TOUCHPAD | LI_CCAP_RUMBLE), "a Steam Deck client keeps its DualSense");
    check(!is_steam_controller_client(LI_CTYPE_PS, our_client_caps), "a PlayStation client never selects the profile");
  }

  void test_buttons() {
    pad_t pad;
    pad.sc26_explicit = true;
    check(map_buttons(PLAY_FLAG, pad) == sc::btn_view, "start goes to VIEW (0x40), where SDL reads start");
    check(map_buttons(BACK_FLAG, pad) == sc::btn_menu, "back goes to MENU (0x4000), where SDL reads back");
    check(map_buttons(PADDLE1_FLAG | PADDLE2_FLAG | PADDLE3_FLAG | PADDLE4_FLAG, pad) == (sc::btn_r4 | sc::btn_l4 | sc::btn_r5 | sc::btn_l5), "paddles map R4, L4, R5, L5");
    check(map_buttons(SPECIAL_FLAG | MISC_FLAG, pad) == (sc::btn_steam | sc::btn_qam), "guide and misc map to Steam and QAM");
    check(map_buttons(LEFT_PAD_CLICK_FLAG | RIGHT_TRIGGER_CLICK_FLAG, pad) == (sc::btn_left_pad_click | sc::btn_right_trigger_click), "explicit pad and trigger clicks pass through");
    check(map_buttons(TOUCHPAD_FLAG, pad) == 0, "an explicit client's legacy touchpad click is not guessed onto a pad");

    pad_t legacy;
    check(map_buttons(TOUCHPAD_FLAG, legacy) == sc::btn_right_pad_click, "a click without touch lands on the right pad");
    legacy.device.pad_touched[0] = true;
    check(map_buttons(TOUCHPAD_FLAG, legacy) == sc::btn_left_pad_click, "a click lands on the touched left pad");
    check(map_buttons(LEFT_PAD_CLICK_FLAG, legacy) == 0, "a client without SC26_BUTTONS cannot inject pad clicks");
  }

  void test_encode() {
    pad_t pad;
    pad.sc26_explicit = true;
    auto r = encode(pad, LEFT_GRIP_TOUCH_FLAG | RIGHT_STICK_TOUCH_FLAG, 32767, -32767, 255);
    check(r.report_id == sc::input_report_id, "state report id 0x42");
    check(r.left_x == 32767 && r.left_y == -32767, "sticks reach exactly +/-32767");
    check(r.left_trigger == 32767, "a full trigger reports 32767");
    check((r.buttons & sc::btn_left_grip_touch) && !(r.buttons & sc::btn_right_grip_touch), "grip touch is kept in the button word");
    check((r.buttons & sc::btn_right_stick_touch) && !(r.buttons & sc::btn_left_stick_touch), "explicit stick touch, no deflection guess");
    check(!(r.buttons & sc::btn_left_trigger_click), "an explicit client's full trigger is not a click unless it says so");
    check(r.imu_timestamp == 1000, "the report carries the microsecond clock");
    const auto r2 = encode(pad, 0);
    check(r2.sequence == static_cast<std::uint8_t>(r.sequence + 1), "the sequence advances per report");

    pad_t legacy;
    r = encode(legacy, 0, 20000, 0, 255);
    check(r.buttons & sc::btn_left_stick_touch, "a client without stick touch: a deflected stick reads touched");
    check(r.buttons & sc::btn_left_trigger_click, "a client without SC26_BUTTONS: a full trigger clicks");
    check(!(r.buttons & (sc::btn_left_grip_touch | sc::btn_right_grip_touch)), "no grip bits without grip sense");
  }

  void test_touch() {
    pad_t dual;
    dual.dual_touchpad = true;
    check(apply_touch(dual, LI_TOUCH_EVENT_DOWN, 7, 1, 0.25f, 0.5f, 1.0f), "dual: a down on pad 1 applies");
    check(dual.device.pad_touched[1] && !dual.device.pad_touched[0], "dual: pad index 1 is the right pad");
    check(dual.device.pad_x[1] == normalized_touch(0.25f), "dual: x is not rescaled");
    check(apply_touch(dual, LI_TOUCH_EVENT_UP, 7, 1, 0.0f, 0.0f, 0.0f) && !dual.device.pad_touched[1], "dual: up releases");
    check(!apply_touch(dual, LI_TOUCH_EVENT_MOVE, 7, 0, 0.5f, 0.5f, 1.0f), "dual: a move without a down creates no contact");

    pad_t split;
    check(apply_touch(split, LI_TOUCH_EVENT_DOWN, 3, 0, 0.75f, 0.5f, 1.0f), "split: a down on the right half applies");
    check(split.device.pad_touched[1], "split: the right half is the right pad");
    check(split.device.pad_x[1] == normalized_touch(0.5f), "split: the half is stretched to a full pad");
    check(!apply_touch(split, LI_TOUCH_EVENT_DOWN, 4, 0, 0.9f, 0.5f, 1.0f), "split: a second finger on the same pad is refused");
    check(apply_touch(split, LI_TOUCH_EVENT_MOVE, 3, 0, 0.2f, 0.5f, 1.0f) && split.device.pad_touched[1], "split: the pointer stays on its pad when it crosses the middle");
    check(split.device.pad_x[1] == 0, "split: crossing the middle clamps to the pad's edge");
    check(apply_touch(split, LI_TOUCH_EVENT_DOWN, 5, 0, 0.1f, 0.5f, 1.0f) && split.device.pad_touched[0], "split: a finger on the left half is the left pad");
    check(apply_touch(split, LI_TOUCH_EVENT_UP, 3, 0, 0.0f, 0.0f, 0.0f) && !split.device.pad_touched[1], "split: up releases the pointer's pad");
    check(apply_touch(split, LI_TOUCH_EVENT_CANCEL_ALL, 0, 0, 0.0f, 0.0f, 0.0f) && !split.device.pad_touched[0] && split.pointer_pad.empty(), "cancel all releases both pads");
  }

  void test_motion_battery() {
    pad_t pad;
    apply_motion(pad, LI_MOTION_TYPE_GYRO, 0.0f, 90.0f, 0.0f);
    sc::state expected {};
    sc::apply_gyro_milli(expected, 0, 90000, 0);
    check(pad.device.gyro[0] == expected.gyro[0] && pad.device.gyro[1] == expected.gyro[1] && pad.device.gyro[2] == expected.gyro[2], "gyro deg/s goes through the SDL axis conversion");
    apply_motion(pad, LI_MOTION_TYPE_ACCEL, 0.0f, 9.80665f, 0.0f);
    check(pad.device.accel[2] > 16000 && pad.device.accel[2] < 16800, "1 g on SDL y is 1 g on the device's z");

    apply_battery(pad, LI_BATTERY_STATE_DISCHARGING, 40);
    apply_battery(pad, LI_BATTERY_STATE_DISCHARGING, LI_BATTERY_PERCENTAGE_UNKNOWN);
    check(pad.device.battery_percent == 40, "an unknown percentage keeps the last level");
    apply_battery(pad, LI_BATTERY_STATE_CHARGING, 50);
    check(pad.device.charge_state == sc::charge_charging, "charging maps to the charging state");
  }

  void test_control_channel() {
    pad_t pad;
    // The commands of Steam's handshake as seen from Steam on Linux, plus the rest of the captured command set.
    for (const std::uint8_t command : {0x83, 0xAE, 0x87, 0xC1, 0xDC, 0xE2, 0x81, 0x85, 0x8E, 0xF2, 0xED, 0x89, 0x8B, 0x8C}) {
      check(send(pad, command, {0x01}) == feature_result_e::accepted, "every command of Steam's handshake is accepted");
    }

    // Firmware update, calibration and serial writes: a virtual device takes none of them.
    for (const std::uint8_t command : {0x86, 0x90, 0x95, 0x9F, 0xA7, 0xA9, 0xB5, 0xB7, 0xB8, 0xB9, 0xBF, 0xC0}) {
      check(send(pad, command) == feature_result_e::refused, "an unknown command is refused");
    }
    std::uint8_t reply[sc::feature_report_size];
    check(send(pad, 0x83) == feature_result_e::accepted, "attributes after refusals");
    check(send(pad, 0x95, {1, 2, 3}) == feature_result_e::refused, "a firmware-style command is refused");
    check(sc::get_feature(reply, sizeof(reply), pad.features) == sc::feature_report_size && reply[1] == 0, "a refused command leaves no stale reply behind");
    check(pad.features.last_unknown_command == 0x95, "the refused command is recorded");

    const std::uint8_t too_long[] = {sc::features_report_id, sc::cmd_set_settings_values, 60, 9, 0, 0};
    check(set_feature(pad, too_long, sizeof(too_long)) == feature_result_e::malformed, "a length past the data is malformed");
    const std::uint8_t wrong_id[] = {0x02, sc::cmd_get_attributes_values, 0};
    check(set_feature(pad, wrong_id, sizeof(wrong_id)) == feature_result_e::malformed, "feature report 2 is not the control channel");
  }

  void test_attributes() {
    pad_t pad;
    check(pad.features.attrs.firmware_build_time == 0x6AE90BC5u, "a new device reports the build time of Steam's bundled firmware");
    check(pad.features.attrs.firmware_build_time != sc::captured_firmware_build_time, "the captured (outdated) build time is not reported");
    {
      std::uint8_t reply[sc::feature_report_size];
      check(send(pad, sc::cmd_get_attributes_values) == feature_result_e::accepted, "attributes query on a new device");
      const auto length = sc::get_feature(reply, sizeof(reply), pad.features);
      bool found = false;
      // The reply lists (tag u8, value le32) pairs after the command and length bytes.
      for (std::size_t at = 3; at + 5 <= length && at < 3u + reply[2]; at += 5) {
        if (reply[at] == sc::attr_firmware_build_time) {
          std::uint32_t value;
          std::memcpy(&value, reply + at + 1, sizeof(value));
          found = value == 0x6AE90BC5u;
        }
      }
      check(found, "the attributes reply carries Steam's bundled firmware build time");
    }
    std::array<char, 16> build {"build123"}, unit {"FXA2601234567"}, board {"MXA2601234567"};
    const auto applied = apply_attributes(pad, 0xFF, 0x6B000000u, 0x68D2F92Eu, 0x4A, 0, build, unit, board);
    check(applied == attributes_applied_mask, "every field the device has is applied, radio and dongle times are not");
    check(pad.features.attrs.firmware_build_time == 0x6B000000u, "the real firmware build time is reported");
    check(send(pad, sc::cmd_get_string_attribute, {sc::string_attr_unit_serial}) == feature_result_e::accepted, "serial query");
    std::uint8_t reply[sc::feature_report_size];
    static_cast<void>(sc::get_feature(reply, sizeof(reply), pad.features));
    check(std::memcmp(reply + 4, "FXA2601234567", 13) == 0, "the unit serial query returns the real serial");

    pad_t untouched;
    std::array<char, 16> unterminated;
    unterminated.fill('A');
    static_cast<void>(apply_attributes(untouched, LI_SCATTR_SERIALS, 0, 0, 0, 0, build, unterminated, unterminated));
    check(untouched.features.unit_serial[15] == '\0', "an unterminated serial cannot overrun");
  }

  void test_feedback() {
    const std::uint8_t pulse[] = {0x81, 0x01, 0x90, 0x01, 0x00, 0x00, 0x01, 0x00};
    const auto haptic = forwarded_haptic(pulse, sizeof(pulse));
    check(haptic && haptic->length == 8 && haptic->report[0] == 0x81 && haptic->report[2] == 0x90, "a pulse is forwarded verbatim");
    check(!forwarded_haptic(pulse, 5), "a truncated report is dropped");
    std::uint8_t rumble[sc::haptic_report_size + 6] {sc::haptic_rumble_id};
    const auto padded = forwarded_haptic(rumble, sizeof(rumble));
    check(padded && padded->length == sc::haptic_report_size, "transport padding is cut to the descriptor's length");
    const std::uint8_t unknown[] = {0x86, 0, 0, 0};
    check(!forwarded_haptic(unknown, sizeof(unknown)), "0x86 is not forwarded");

    std::array<settings_chunk_t, 3> chunks;
    std::vector<std::uint8_t> triples;
    for (int n = 0; n < 20; ++n) {
      triples.insert(triples.end(), {static_cast<std::uint8_t>(n), 1, 0});
    }
    const auto write = transfer(sc::cmd_set_settings_values, triples);
    check(settings_chunks(write.data(), write.size(), chunks) == 3 && chunks[0].count == 9 && chunks[2].count == 2 && chunks[2].triples[0] == 18, "20 triples travel as 9 + 9 + 2");
    const auto reset = transfer(sc::cmd_load_default_settings, {});
    check(settings_chunks(reset.data(), reset.size(), chunks) == 1 && chunks[0].count == 0, "load defaults travels as one empty chunk");
    const auto other = transfer(sc::cmd_get_attributes_values, {});
    check(settings_chunks(other.data(), other.size(), chunks) == 0, "other commands are not settings");
  }
}  // namespace

int main() {
  test_selection();
  test_buttons();
  test_encode();
  test_touch();
  test_motion_battery();
  test_control_channel();
  test_attributes();
  test_feedback();
  if (failures) {
    std::printf("%d check(s) failed\n", failures);
    return 1;
  }
  std::printf("PASS: sc26 policy\n");
  return 0;
}
