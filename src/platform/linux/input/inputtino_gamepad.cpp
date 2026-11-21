/**
 * @file src/platform/linux/input/inputtino_gamepad.cpp
 * @brief Definitions for inputtino gamepad input handling.
 */
// lib includes
#include <boost/locale.hpp>
#include <inputtino/input.hpp>
#include <libevdev/libevdev.h>
#include <unistd.h>

// standard includes
#include <algorithm>
#include <cerrno>
#include <cstring>

// local includes
#include "inputtino_common.h"
#include "ds5_haptics.h"
#include "inputtino_gamepad.h"
#include "inputtino_seat.h"
#include "src/config.h"
#include "src/logging.h"
#include "src/platform/common.h"
#include "src/utility.h"

using namespace std::literals;

namespace platf::gamepad {

  enum GamepadStatus {
    UHID_NOT_AVAILABLE = 0,  ///< UHID is not available
    UINPUT_NOT_AVAILABLE,  ///< UINPUT is not available
    XINPUT_NOT_AVAILABLE,  ///< XINPUT is not available
    GAMEPAD_STATUS  ///< Helper to indicate the number of status
  };

  auto create_xbox_one() {
    return inputtino::XboxOneJoypad::create({.name = inputtino_name_for_seat("Sunshine X-Box One (virtual) pad"sv),
                                             // https://github.com/torvalds/linux/blob/master/drivers/input/joystick/xpad.c#L147
                                             .vendor_id = 0x045E,
                                             .product_id = 0x02EA,
                                             .version = 0x0408});
  }

  auto create_switch() {
    return inputtino::SwitchJoypad::create({.name = inputtino_name_for_seat("Sunshine Nintendo (virtual) pad"sv),
                                            // https://github.com/torvalds/linux/blob/master/drivers/hid/hid-ids.h#L981
                                            .vendor_id = 0x057e,
                                            .product_id = 0x2009,
                                            .version = 0x8111});
  }

  auto create_ds4(int globalIndex) {
    std::string device_mac;
    if (globalIndex >= 0 && globalIndex <= 255) {
      device_mac = std::format("02:00:00:04:00:{:02x}", globalIndex);
    }

    return ds4_joypad_t::create({
      .name = inputtino_name_for_seat("Sunshine PS4 (virtual) pad"sv),
      .vendor_id = 0x054C,
      .product_id = 0x05C4,
      .version = 0x0100,
      .device_uniq = device_mac,
    });
  }

  auto create_ds5(int globalIndex) {
    std::string device_mac = "";  // Inputtino checks empty() to generate a random MAC

    if (!config::input.ds5_inputtino_randomize_mac && globalIndex >= 0 && globalIndex <= 255) {
      // Generate private virtual device MAC based on gamepad globalIndex between 0 (00) and 255 (ff)
      device_mac = std::format("02:00:00:00:00:{:02x}", globalIndex);
    }

    return inputtino::PS5Joypad::create({.name = inputtino_name_for_seat("Sunshine PS5 (virtual) pad"sv), .vendor_id = 0x054C, .product_id = 0x0CE6, .version = 0x8111, .device_phys = device_mac, .device_uniq = device_mac});
  }

  auto create_sc26(const gamepad_id_t &id, const gamepad_arrival_t &metadata, feedback_queue_t feedback_queue, std::shared_ptr<joypad_state> gamepad) {
    sc26_joypad_t::options_t options;
    options.sc26_explicit = (metadata.capabilities & LI_CCAP_SC26_BUTTONS) != 0;
    options.dual_touchpad = (metadata.capabilities & LI_CCAP_DUAL_TOUCHPAD) != 0;
    // One serial per slot, so Steam tells several virtual controllers apart and keeps each
    // one's settings; the real controller's serial replaces it once its attributes arrive.
    options.serial = std::format("LVGSC26{:04}", std::clamp(id.globalIndex, 0, 9999));

    const auto idx = id.clientRelativeIndex;
    if (metadata.capabilities & LI_CCAP_STEAM_HAPTIC) {
      // Every report as it came: Steam re-sends the same click to keep a buzz going.
      options.on_haptic = [feedback_queue, idx](const sc26::haptic_report_t &haptic) {
        static_cast<void>(platf::raise_gamepad_feedback(*feedback_queue, gamepad_feedback_msg_t::make_steam_haptic(idx, haptic.length, haptic.report)));
      };
    } else {
      options.on_rumble = [feedback_queue, idx, gamepad](int low_freq, int high_freq) {
        static_cast<void>(platf::raise_gamepad_feedback_deduped(*feedback_queue, gamepad_feedback_msg_t::make_rumble(idx, low_freq, high_freq), gamepad->last_rumble));
      };
    }
    if (metadata.capabilities & LI_CCAP_STEAM_SETTINGS) {
      options.on_settings = [feedback_queue, idx](const sc26::settings_chunk_t &chunk) {
        if (!platf::raise_gamepad_feedback(*feedback_queue, gamepad_feedback_msg_t::make_steam_settings(idx, chunk.command, chunk.count, chunk.triples))) {
          BOOST_LOG(warning) << "Steam Controller: feedback queue full, dropping a settings write ("sv << int(chunk.count) << " triples)"sv;
        }
      };
    }

    return sc26_joypad_t::create({
                                   .name = inputtino_name_for_seat("Valve Software Steam Controller"sv),
                                   .vendor_id = lvg::sc26_usb::vendor_id,
                                   .product_id = lvg::sc26_usb::product_id,
                                   .version = lvg::sc26_usb::version,
                                 },
                                 std::move(options));
  }

  int alloc(input_raw_t *raw, const gamepad_id_t &id, const gamepad_arrival_t &metadata, feedback_queue_t feedback_queue) {
    ControllerType selectedGamepadType;

    if (config::input.gamepad == "xone"sv) {
      BOOST_LOG(info) << "Gamepad " << id.globalIndex << " will be Xbox One controller (manual selection)"sv;
      selectedGamepadType = XboxOneWired;
    } else if (config::input.gamepad == "ds4"sv) {
      BOOST_LOG(info) << "Gamepad " << id.globalIndex << " will be DualShock 4 controller (manual selection)"sv;
      selectedGamepadType = DualShock4Wired;
    } else if (config::input.gamepad == "ds5"sv) {
      BOOST_LOG(info) << "Gamepad " << id.globalIndex << " will be DualSense 5 controller (manual selection)"sv;
      selectedGamepadType = DualSenseWired;
    } else if (config::input.gamepad == "switch"sv) {
      BOOST_LOG(info) << "Gamepad " << id.globalIndex << " will be Nintendo Pro controller (manual selection)"sv;
      selectedGamepadType = SwitchProWired;
    } else if (config::input.gamepad == "steam"sv) {
      BOOST_LOG(info) << "Gamepad " << id.globalIndex << " will be Steam Controller (manual selection)"sv;
      selectedGamepadType = SteamControllerWired;
    } else if (metadata.type == LI_CTYPE_XBOX) {
      BOOST_LOG(info) << "Gamepad " << id.globalIndex << " will be Xbox One controller (auto-selected by client-reported type)"sv;
      selectedGamepadType = XboxOneWired;
    } else if (metadata.type == LI_CTYPE_PS) {
      BOOST_LOG(info) << "Gamepad " << id.globalIndex << " will be DualShock 5 controller (auto-selected by client-reported type)"sv;
      selectedGamepadType = DualSenseWired;
    } else if (metadata.type == LI_CTYPE_NINTENDO) {
      BOOST_LOG(info) << "Gamepad " << id.globalIndex << " will be Nintendo Pro controller (auto-selected by client-reported type)"sv;
      selectedGamepadType = SwitchProWired;
    } else if (sc26::is_steam_controller_client(metadata.type, metadata.capabilities)) {
      BOOST_LOG(info) << "Gamepad " << id.globalIndex << " will be Steam Controller (auto-selected by client-reported type)"sv;
      selectedGamepadType = SteamControllerWired;
    } else if (config::input.motion_as_ds4 && (metadata.capabilities & (LI_CCAP_ACCEL | LI_CCAP_GYRO))) {
      BOOST_LOG(info) << "Gamepad " << id.globalIndex << " will be DualShock 5 controller (auto-selected by motion sensor presence)"sv;
      selectedGamepadType = DualSenseWired;
    } else if (config::input.touchpad_as_ds4 && (metadata.capabilities & LI_CCAP_TOUCHPAD)) {
      BOOST_LOG(info) << "Gamepad " << id.globalIndex << " will be DualShock 5 controller (auto-selected by touchpad presence)"sv;
      selectedGamepadType = DualSenseWired;
    } else {
      BOOST_LOG(info) << "Gamepad " << id.globalIndex << " will be Xbox One controller (default)"sv;
      selectedGamepadType = XboxOneWired;
    }

    if (selectedGamepadType == XboxOneWired || selectedGamepadType == SwitchProWired) {
      if (metadata.capabilities & (LI_CCAP_ACCEL | LI_CCAP_GYRO)) {
        BOOST_LOG(warning) << "Gamepad " << id.globalIndex << " has motion sensors, but they are not usable when emulating a joypad different from DS5"sv;
      }
      if (metadata.capabilities & LI_CCAP_TOUCHPAD) {
        BOOST_LOG(warning) << "Gamepad " << id.globalIndex << " has a touchpad, but it is not usable when emulating a joypad different from DS5"sv;
      }
      if (metadata.capabilities & LI_CCAP_RGB_LED) {
        BOOST_LOG(warning) << "Gamepad " << id.globalIndex << " has an RGB LED, but it is not usable when emulating a joypad different from DS5"sv;
      }
    } else if (selectedGamepadType == DualShock4Wired || selectedGamepadType == DualSenseWired) {
      if (!(metadata.capabilities & (LI_CCAP_ACCEL | LI_CCAP_GYRO))) {
        BOOST_LOG(warning) << "Gamepad " << id.globalIndex << " is emulating a PlayStation controller, but the client gamepad doesn't have motion sensors active"sv;
      }
      if (!(metadata.capabilities & LI_CCAP_TOUCHPAD)) {
        BOOST_LOG(warning) << "Gamepad " << id.globalIndex << " is emulating a PlayStation controller, but the client gamepad doesn't have a touchpad"sv;
      }
    }

    auto gamepad = std::make_shared<joypad_state>(joypad_state {});
    auto on_rumble_fn = [feedback_queue, idx = id.clientRelativeIndex, gamepad](int low_freq, int high_freq) {
      // Coalesced admission, deduplicated against the last rumble this backend delivered; the
      // cache follows acceptance, so a message the mailbox refused is retried by the next
      // callback instead of being suppressed as already delivered.
      static_cast<void>(platf::raise_gamepad_feedback_deduped(*feedback_queue, gamepad_feedback_msg_t::make_rumble(idx, low_freq, high_freq), gamepad->last_rumble));
    };

    switch (selectedGamepadType) {
      case XboxOneWired:
        {
          auto xOne = create_xbox_one();
          if (xOne) {
            (*xOne).set_on_rumble(on_rumble_fn);
            gamepad->joypad = std::make_unique<joypads_t>(std::move(*xOne));
            raw->gamepads[id.globalIndex] = std::move(gamepad);
            return 0;
          } else {
            BOOST_LOG(warning) << "Unable to create virtual Xbox One controller: " << xOne.getErrorMessage();
            return -1;
          }
        }
      case SwitchProWired:
        {
          auto switchPro = create_switch();
          if (switchPro) {
            (*switchPro).set_on_rumble(on_rumble_fn);
            gamepad->joypad = std::make_unique<joypads_t>(std::move(*switchPro));
            raw->gamepads[id.globalIndex] = std::move(gamepad);
            return 0;
          } else {
            BOOST_LOG(warning) << "Unable to create virtual Switch Pro controller: " << switchPro.getErrorMessage();
            return -1;
          }
        }
      case DualShock4Wired:
        {
          auto ds4 = create_ds4(id.globalIndex);
          if (ds4) {
            // Match the neutral resting orientation used by the Windows DS4 path.
            (*ds4).set_motion(ds4_joypad_t::motion_type_e::acceleration, 0.0f, 9.80665f, 0.0f);
            (*ds4).set_on_rumble(on_rumble_fn);
            (*ds4).set_on_led([feedback_queue, idx = id.clientRelativeIndex, gamepad](int r, int g, int b) {
              // Coalesced admission; the duplicate cache follows acceptance.
              static_cast<void>(platf::raise_gamepad_feedback_deduped(*feedback_queue, gamepad_feedback_msg_t::make_rgb_led(idx, r, g, b), gamepad->last_rgb_led));
            });
            static_cast<void>(platf::raise_gamepad_feedback(*feedback_queue, gamepad_feedback_msg_t::make_motion_event_state(id.clientRelativeIndex, LI_MOTION_TYPE_ACCEL, 100)));
            static_cast<void>(platf::raise_gamepad_feedback(*feedback_queue, gamepad_feedback_msg_t::make_motion_event_state(id.clientRelativeIndex, LI_MOTION_TYPE_GYRO, 100)));
            gamepad->joypad = std::make_unique<joypads_t>(std::move(*ds4));
            raw->gamepads[id.globalIndex] = std::move(gamepad);
            return 0;
          }
          BOOST_LOG(warning) << "Unable to create virtual DualShock 4 controller: " << ds4.getErrorMessage();
          return -1;
        }
      case SteamControllerWired:
        {
          auto sc26_pad = create_sc26(id, metadata, feedback_queue, gamepad);
          if (sc26_pad) {
            // The client streams motion only when asked; the state report carries it at 250 Hz.
            static_cast<void>(platf::raise_gamepad_feedback(*feedback_queue, gamepad_feedback_msg_t::make_motion_event_state(id.clientRelativeIndex, LI_MOTION_TYPE_ACCEL, sc26::motion_rate_hz)));
            static_cast<void>(platf::raise_gamepad_feedback(*feedback_queue, gamepad_feedback_msg_t::make_motion_event_state(id.clientRelativeIndex, LI_MOTION_TYPE_GYRO, sc26::motion_rate_hz)));
            gamepad->joypad = std::make_unique<joypads_t>(std::move(*sc26_pad));
            raw->gamepads[id.globalIndex] = std::move(gamepad);
            BOOST_LOG(warning) << "Gamepad " << id.globalIndex
                               << " (Steam Controller): do not start a Steam Controller firmware update from Steam while streaming. "sv
                               << "It would target this virtual controller, which refuses it; update the real controller with it connected directly to a PC running Steam."sv;
            return 0;
          }
          BOOST_LOG(warning) << "Unable to create virtual Steam Controller: " << sc26_pad.getErrorMessage() << "; falling back to DualSense"sv;
        }
        [[fallthrough]];
      case DualSenseWired:
        {
          auto ds5 = create_ds5(id.globalIndex);
          if (ds5) {
            (*ds5).set_on_rumble(on_rumble_fn);
            if (metadata.capabilities & LI_CCAP_HAPTICS_PCM) {
              auto packetizer = std::make_shared<ds5_haptics_packetizer>();
              (*ds5).set_on_haptics([feedback_queue, idx = id.clientRelativeIndex, packetizer](const unsigned char *pcm, std::size_t bytes) {
                packetizer->push(pcm, bytes, [&](std::uint32_t sequence, const auto &samples) {
                  gamepad_feedback_msg_t msg {};
                  msg.type = gamepad_feedback_e::haptics_pcm;
                  msg.id = idx;
                  msg.data.haptics.sequence = sequence;
                  msg.data.haptics.samples = samples;
                  // PCM may be dropped under load; never purge queued trigger,
                  // LED, or stop-rumble messages to make room for audio.
                  feedback_queue->try_raise(std::move(msg));
                });
              });
            }
            (*ds5).set_on_led([feedback_queue, idx = id.clientRelativeIndex, gamepad](int r, int g, int b) {
              // Coalesced admission; the duplicate cache follows acceptance.
              static_cast<void>(platf::raise_gamepad_feedback_deduped(*feedback_queue, gamepad_feedback_msg_t::make_rgb_led(idx, r, g, b), gamepad->last_rgb_led));
            });

            (*ds5).set_on_trigger_effect([feedback_queue, idx = id.clientRelativeIndex](const inputtino::PS5Joypad::TriggerEffect &trigger_effect) {
              // A protected state key per trigger-effect flag: coalesced, never dropped for a full mailbox.
              static_cast<void>(platf::raise_gamepad_feedback(*feedback_queue, gamepad_feedback_msg_t::make_adaptive_triggers(idx, trigger_effect.event_flags, trigger_effect.type_left, trigger_effect.type_right, trigger_effect.left, trigger_effect.right)));
            });

            // Activate the motion sensors (coalesced: one pending request per type)
            static_cast<void>(platf::raise_gamepad_feedback(*feedback_queue, gamepad_feedback_msg_t::make_motion_event_state(id.clientRelativeIndex, LI_MOTION_TYPE_ACCEL, 100)));
            static_cast<void>(platf::raise_gamepad_feedback(*feedback_queue, gamepad_feedback_msg_t::make_motion_event_state(id.clientRelativeIndex, LI_MOTION_TYPE_GYRO, 100)));

            gamepad->joypad = std::make_unique<joypads_t>(std::move(*ds5));
            raw->gamepads[id.globalIndex] = std::move(gamepad);
            return 0;
          } else {
            BOOST_LOG(warning) << "Unable to create virtual DualShock 5 controller: " << ds5.getErrorMessage();
            return -1;
          }
        }
    }
    return -1;
  }

  void free(input_raw_t *raw, int nr) {
    // This will call the destructor which in turn will stop the background threads for rumble and LED (and ultimately remove the joypad device)
    raw->gamepads[nr]->joypad.reset();
    raw->gamepads[nr].reset();
  }

  void update(input_raw_t *raw, int nr, const gamepad_state_t &gamepad_state) {
    auto gamepad = raw->gamepads[nr];
    if (!gamepad) {
      return;
    }

    auto button_flags = gamepad_state.buttonFlags;
    if (config::input.ds4_back_as_touchpad_click && std::holds_alternative<ds4_joypad_t>(*gamepad->joypad) && (button_flags & BACK)) {
      button_flags |= TOUCHPAD_BUTTON;
    }

    std::visit([gamepad_state, button_flags](inputtino::Joypad &gc) {
      gc.set_pressed_buttons(button_flags);
      gc.set_stick(inputtino::Joypad::LS, gamepad_state.lsX, gamepad_state.lsY);
      gc.set_stick(inputtino::Joypad::RS, gamepad_state.rsX, gamepad_state.rsY);
      gc.set_triggers(gamepad_state.lt, gamepad_state.rt);
    },
               *gamepad->joypad);
  }

  void touch(input_raw_t *raw, const gamepad_touch_t &touch) {
    auto gamepad = raw->gamepads[touch.id.globalIndex];
    if (!gamepad) {
      return;
    }
    if (std::holds_alternative<sc26_joypad_t>(*gamepad->joypad)) {
      std::get<sc26_joypad_t>(*gamepad->joypad).touch(touch.eventType, touch.pointerId, touch.touchpadIndex, touch.x, touch.y, touch.pressure);
    } else if (std::holds_alternative<ds4_joypad_t>(*gamepad->joypad)) {
      auto &ds4 = std::get<ds4_joypad_t>(*gamepad->joypad);
      if (touch.pressure > 0.5) {
        ds4.place_finger(touch.pointerId, touch.x * ds4_joypad_t::touchpad_width, touch.y * ds4_joypad_t::touchpad_height);
      } else {
        ds4.release_finger(touch.pointerId);
      }
    } else if (std::holds_alternative<inputtino::PS5Joypad>(*gamepad->joypad)) {
      if (touch.pressure > 0.5) {
        std::get<inputtino::PS5Joypad>(*gamepad->joypad).place_finger(touch.pointerId, touch.x * inputtino::PS5Joypad::touchpad_width, touch.y * inputtino::PS5Joypad::touchpad_height);
      } else {
        std::get<inputtino::PS5Joypad>(*gamepad->joypad).release_finger(touch.pointerId);
      }
    }
  }

  void motion(input_raw_t *raw, const gamepad_motion_t &motion) {
    auto gamepad = raw->gamepads[motion.id.globalIndex];
    if (!gamepad) {
      return;
    }
    if (std::holds_alternative<sc26_joypad_t>(*gamepad->joypad)) {
      std::get<sc26_joypad_t>(*gamepad->joypad).set_motion(motion.motionType, motion.x, motion.y, motion.z);
    } else if (std::holds_alternative<ds4_joypad_t>(*gamepad->joypad)) {
      auto &ds4 = std::get<ds4_joypad_t>(*gamepad->joypad);
      switch (motion.motionType) {
        case LI_MOTION_TYPE_ACCEL:
          ds4.set_motion(ds4_joypad_t::motion_type_e::acceleration, motion.x, motion.y, motion.z);
          break;
        case LI_MOTION_TYPE_GYRO:
          ds4.set_motion(ds4_joypad_t::motion_type_e::gyroscope, motion.x, motion.y, motion.z);
          break;
      }
    } else if (std::holds_alternative<inputtino::PS5Joypad>(*gamepad->joypad)) {
      switch (motion.motionType) {
        case LI_MOTION_TYPE_ACCEL:
          std::get<inputtino::PS5Joypad>(*gamepad->joypad).set_motion(inputtino::PS5Joypad::ACCELERATION, motion.x, motion.y, motion.z);
          break;
        case LI_MOTION_TYPE_GYRO:
          std::get<inputtino::PS5Joypad>(*gamepad->joypad).set_motion(inputtino::PS5Joypad::GYROSCOPE, deg2rad(motion.x), deg2rad(motion.y), deg2rad(motion.z));
          break;
      }
    }
  }

  void battery(input_raw_t *raw, const gamepad_battery_t &battery) {
    auto gamepad = raw->gamepads[battery.id.globalIndex];
    if (!gamepad) {
      return;
    }
    if (std::holds_alternative<sc26_joypad_t>(*gamepad->joypad)) {
      std::get<sc26_joypad_t>(*gamepad->joypad).set_battery(battery.state, battery.percentage);
    } else if (std::holds_alternative<ds4_joypad_t>(*gamepad->joypad)) {
      ds4_joypad_t::battery_state_e state;
      switch (battery.state) {
        case LI_BATTERY_STATE_CHARGING:
          state = ds4_joypad_t::battery_state_e::charging;
          break;
        case LI_BATTERY_STATE_DISCHARGING:
          state = ds4_joypad_t::battery_state_e::discharging;
          break;
        case LI_BATTERY_STATE_FULL:
          state = ds4_joypad_t::battery_state_e::full;
          break;
        default:
          return;
      }
      if (battery.percentage != LI_BATTERY_PERCENTAGE_UNKNOWN) {
        std::get<ds4_joypad_t>(*gamepad->joypad).set_battery(state, battery.percentage);
      }
    } else if (std::holds_alternative<inputtino::PS5Joypad>(*gamepad->joypad)) {
      inputtino::PS5Joypad::BATTERY_STATE state;
      switch (battery.state) {
        case LI_BATTERY_STATE_CHARGING:
          state = inputtino::PS5Joypad::BATTERY_CHARGHING;
          break;
        case LI_BATTERY_STATE_DISCHARGING:
          state = inputtino::PS5Joypad::BATTERY_DISCHARGING;
          break;
        case LI_BATTERY_STATE_FULL:
          state = inputtino::PS5Joypad::BATTERY_FULL;
          break;
        case LI_BATTERY_STATE_UNKNOWN:
        case LI_BATTERY_STATE_NOT_PRESENT:
        default:
          return;
      }
      if (battery.percentage != LI_BATTERY_PERCENTAGE_UNKNOWN) {
        std::get<inputtino::PS5Joypad>(*gamepad->joypad).set_battery(state, battery.percentage);
      }
    }
  }

  void attributes(input_raw_t *raw, const steam_controller_attributes_t &attributes) {
    if (attributes.id.globalIndex < 0 || attributes.id.globalIndex >= static_cast<int>(raw->gamepads.size())) {
      return;
    }
    auto gamepad = raw->gamepads[attributes.id.globalIndex];
    if (!gamepad || !std::holds_alternative<sc26_joypad_t>(*gamepad->joypad)) {
      return;
    }
    std::get<sc26_joypad_t>(*gamepad->joypad).set_attributes(attributes);
  }

  std::vector<supported_gamepad_t> &supported_gamepads(input_t *input) {
    if (!input) {
      static std::vector gps {
        supported_gamepad_t {"auto", true, ""},
        supported_gamepad_t {"xone", false, ""},
        supported_gamepad_t {"ds4", false, ""},
        supported_gamepad_t {"ds5", false, ""},
        supported_gamepad_t {"switch", false, ""},
        supported_gamepad_t {"steam", false, ""},
      };

      return gps;
    }

    auto ds4 = create_ds4(-1);
    auto ds5 = create_ds5(-1);  // Index -1 will result in a random MAC virtual device, which is fine for probing
    auto switchPro = create_switch();
    auto xOne = create_xbox_one();
    // Probing by creating a device would make a Steam Controller appear in Steam for a moment;
    // the profile needs nothing but /dev/uhid.
    const bool uhid_usable = access("/dev/uhid", R_OK | W_OK) == 0;
    const std::string uhid_error = uhid_usable ? std::string {} : std::string {"/dev/uhid: "} + std::strerror(errno);

    static std::vector gps {
      supported_gamepad_t {"auto", true, ""},
      supported_gamepad_t {"xone", static_cast<bool>(xOne), !xOne ? xOne.getErrorMessage() : ""},
      supported_gamepad_t {"ds4", static_cast<bool>(ds4), !ds4 ? ds4.getErrorMessage() : ""},
      supported_gamepad_t {"ds5", static_cast<bool>(ds5), !ds5 ? ds5.getErrorMessage() : ""},
      supported_gamepad_t {"switch", static_cast<bool>(switchPro), !switchPro ? switchPro.getErrorMessage() : ""},
      supported_gamepad_t {"steam", uhid_usable, uhid_error},
    };

    for (auto &[name, is_enabled, reason_disabled] : gps) {
      if (!is_enabled) {
        BOOST_LOG(warning) << "Gamepad " << name << " is disabled due to " << reason_disabled;
      }
    }

    return gps;
  }
}  // namespace platf::gamepad
