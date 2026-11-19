/**
 * @file src/platform/linux/input/inputtino_sc26.h
 * @brief Steam Controller (2026) virtual controller implemented over Linux UHID.
 * @details The device is the wired controller (28de:1302) with the report descriptor captured
 *          from a real unit. Steam opens it through hidraw, runs its handshake on feature
 *          report 1 and drives the pads' haptics through output reports 0x80..0x85, which go
 *          back to a client that replays them on the real controller.
 */
#pragma once

// standard includes
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

// lib includes
#include <inputtino/input.hpp>
#include <inputtino/result.hpp>

// local includes
#include "src/platform/common.h"
#include "src/platform/linux/input/inputtino_sc26_policy.h"

namespace platf::gamepad {

  class sc26_joypad_t final: public inputtino::Joypad {
  public:
    struct state_t;

    struct options_t {
      /// The client advertised LI_CCAP_SC26_BUTTONS.
      bool sc26_explicit {};
      /// The client advertised LI_CCAP_DUAL_TOUCHPAD.
      bool dual_touchpad {};
      /// Unit serial until the real controller's attributes arrive; also the device's uniq.
      std::string serial;
      /// Haptic output reports for a client that replays them (LI_CCAP_STEAM_HAPTIC).
      std::function<void(const sc26::haptic_report_t &)> on_haptic;
      /// Rumble derived from the haptic reports, for a client without LI_CCAP_STEAM_HAPTIC.
      std::function<void(int low_frequency, int high_frequency)> on_rumble;
      /// Settings writes for a client that applies them to the real controller (LI_CCAP_STEAM_SETTINGS).
      std::function<void(const sc26::settings_chunk_t &)> on_settings;
    };

    static inputtino::Result<sc26_joypad_t> create(const inputtino::DeviceDefinition &device, options_t options);

    sc26_joypad_t(sc26_joypad_t &&other) noexcept;
    sc26_joypad_t &operator=(sc26_joypad_t &&other) noexcept;
    sc26_joypad_t(const sc26_joypad_t &) = delete;
    sc26_joypad_t &operator=(const sc26_joypad_t &) = delete;
    ~sc26_joypad_t() override;

    std::vector<std::string> get_nodes() const override;

    void set_pressed_buttons(unsigned int pressed) override;
    void set_triggers(std::int16_t left, std::int16_t right) override;
    void set_stick(STICK_POSITION stick_type, short x, short y) override;

    void touch(std::uint8_t event, std::uint32_t pointer, std::uint8_t touchpad_index, float x, float y, float pressure);
    void set_motion(std::uint8_t type, float x, float y, float z);
    void set_battery(std::uint8_t state, std::uint8_t percentage);
    void set_attributes(const steam_controller_attributes_t &attributes);

  private:
    explicit sc26_joypad_t(std::shared_ptr<state_t> state);

    std::shared_ptr<state_t> state_;
  };

}  // namespace platf::gamepad
