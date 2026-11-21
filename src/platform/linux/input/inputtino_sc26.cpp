/**
 * @file src/platform/linux/input/inputtino_sc26.cpp
 * @brief Steam Controller (2026) virtual controller implemented over Linux UHID.
 */
#include "inputtino_sc26.h"

// standard includes
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <linux/input.h>
#include <linux/uhid.h>
#include <mutex>
#include <poll.h>
#include <set>
#include <sys/eventfd.h>
#include <sys/timerfd.h>
#include <thread>
#include <unistd.h>
#include <utility>

// local includes
#include "src/logging.h"

using namespace std::literals;

namespace platf::gamepad {
  namespace sc = lvg::sc26_usb;

  namespace {
    std::uint64_t monotonic_us() {
      timespec now {};
      clock_gettime(CLOCK_MONOTONIC, &now);
      return static_cast<std::uint64_t>(now.tv_sec) * 1000000u + static_cast<std::uint64_t>(now.tv_nsec) / 1000u;
    }

    bool write_uhid_event(const int fd, const uhid_event &event) {
      ssize_t written;
      do {
        written = write(fd, &event, sizeof(event));
      } while (written < 0 && errno == EINTR);
      return written == static_cast<ssize_t>(sizeof(event));
    }

    void close_fd(int &fd) {
      if (fd >= 0) {
        close(fd);
        fd = -1;
      }
    }

    template<typename Report>
    uhid_event input_event(const Report &report) {
      static_assert(sizeof(Report) <= UHID_DATA_MAX);
      uhid_event event {};
      event.type = UHID_INPUT2;
      event.u.input2.size = sizeof(report);
      std::memcpy(event.u.input2.data, &report, sizeof(report));
      return event;
    }
  }  // namespace

  struct sc26_joypad_t::state_t {
    int uhid_fd {-1};
    int timer_fd {-1};
    int wake_fd {-1};
    std::thread thread;
    std::atomic_bool stop {false};
    std::string uniq;
    options_t options;

    // Everything below is shared between the input thread and the device thread.
    std::mutex mutex;
    sc26::pad_t pad;
    std::uint32_t buttons {};
    std::int16_t left_x {}, left_y {}, right_x {}, right_y {};
    std::uint8_t left_trigger {}, right_trigger {};
    std::set<std::uint8_t> refused_commands;
    bool handshake_seen {};

    // Device thread only.
    std::uint64_t last_battery_us {};
    sc::rumble rumble {};
    std::uint64_t last_haptic_us {};

    ~state_t() {
      close_fd(timer_fd);
      close_fd(wake_fd);
      if (uhid_fd >= 0) {
        uhid_event event {};
        event.type = UHID_DESTROY;
        write_uhid_event(uhid_fd, event);
        close_fd(uhid_fd);
      }
    }

    void send(const uhid_event &event) const {
      write_uhid_event(uhid_fd, event);
    }

    void tick(const std::uint64_t now_us) {
      sc::input_report report;
      std::optional<sc::battery_report> battery;
      {
        std::lock_guard lock {mutex};
        report = sc26::encode_input(pad, buttons, left_x, left_y, right_x, right_y, left_trigger, right_trigger, now_us);
        if (now_us - last_battery_us >= sc26::battery_interval_us) {
          last_battery_us = now_us;
          battery = sc::encode_battery(pad.device);
        }
      }
      send(input_event(report));
      if (battery) {
        send(input_event(*battery));
      }

      // Steam keeps a rumble going by re-sending it; once it stops, so does the motor.
      if (options.on_rumble && (rumble.left || rumble.right) && now_us - last_haptic_us >= sc26::synthesized_rumble_timeout_us) {
        rumble = {};
        options.on_rumble(0, 0);
      }
    }

    void output_report(const std::uint8_t *data, const std::size_t size) {
      if (data == nullptr || size == 0) {
        return;
      }

      // The unit answers a zero-repeat pulse (haptic stop) with an ack per side.
      std::uint8_t ack[6];
      if (sc::encode_haptic_ack(data, size, ack, sizeof(ack)) == sizeof(ack)) {
        uhid_event event {};
        event.type = UHID_INPUT2;
        event.u.input2.size = sizeof(ack);
        std::memcpy(event.u.input2.data, ack, sizeof(ack));
        send(event);
      }

      if (options.on_haptic) {
        if (const auto haptic = sc26::forwarded_haptic(data, size)) {
          options.on_haptic(*haptic);
        }
      } else if (options.on_rumble && sc::decode_haptic_output(data, size, rumble)) {
        last_haptic_us = monotonic_us();
        options.on_rumble(rumble.left, rumble.right);
      }
    }

    void get_report(const uhid_event &request) {
      uhid_event reply {};
      reply.type = UHID_GET_REPORT_REPLY;
      reply.u.get_report_reply.id = request.u.get_report.id;
      if (request.u.get_report.rtype == UHID_FEATURE_REPORT && request.u.get_report.rnum == sc::features_report_id) {
        std::lock_guard lock {mutex};
        reply.u.get_report_reply.size = static_cast<std::uint16_t>(
          sc::get_feature(reply.u.get_report_reply.data, sizeof(reply.u.get_report_reply.data), pad.features)
        );
      }
      if (reply.u.get_report_reply.size == 0) {
        reply.u.get_report_reply.err = EIO;
      }
      send(reply);
    }

    void set_report(const uhid_event &request) {
      const auto &in = request.u.set_report;
      uhid_event reply {};
      reply.type = UHID_SET_REPORT_REPLY;
      reply.u.set_report_reply.id = in.id;
      const std::size_t size = std::min<std::size_t>(in.size, sizeof(in.data));

      if (in.rtype == UHID_OUTPUT_REPORT && size > 0 && sc::is_output_report(in.data[0])) {
        send(reply);
        output_report(in.data, size);
        return;
      }
      if (in.rtype != UHID_FEATURE_REPORT || in.rnum != sc::features_report_id) {
        reply.u.set_report_reply.err = EIO;
        send(reply);
        return;
      }

      sc26::feature_result_e result;
      bool first_refusal = false;
      bool first_handshake = false;
      std::array<sc26::settings_chunk_t, 3> chunks;
      std::size_t chunk_count = 0;
      {
        std::lock_guard lock {mutex};
        result = sc26::set_feature(pad, in.data, size);
        if (result == sc26::feature_result_e::refused) {
          first_refusal = refused_commands.insert(in.data[1]).second;
        } else if (result == sc26::feature_result_e::accepted) {
          if (in.data[1] == sc::cmd_get_attributes_values && !handshake_seen) {
            handshake_seen = first_handshake = true;
          }
          if (options.on_settings) {
            chunk_count = sc26::settings_chunks(in.data, size, chunks);
          }
        }
      }

      if (result == sc26::feature_result_e::refused) {
        reply.u.set_report_reply.err = EIO;
      } else if (result == sc26::feature_result_e::malformed) {
        reply.u.set_report_reply.err = EINVAL;
      }
      send(reply);

      if (first_refusal) {
        BOOST_LOG(warning) << "Steam Controller "sv << uniq << ": refused control command 0x"sv << std::hex << int(in.data[1]) << std::dec
                           << ". A virtual controller takes no firmware update or calibration; update the real controller with it connected directly to a PC running Steam."sv;
      }
      if (first_handshake) {
        BOOST_LOG(info) << "Steam Controller "sv << uniq << ": the host started the Steam handshake"sv;
      }
      for (std::size_t n = 0; n < chunk_count; ++n) {
        options.on_settings(chunks[n]);
      }
    }

    void handle(const uhid_event &event) {
      switch (event.type) {
        case UHID_GET_REPORT:
          get_report(event);
          break;
        case UHID_SET_REPORT:
          set_report(event);
          break;
        case UHID_OUTPUT:
          output_report(event.u.output.data, std::min<std::size_t>(event.u.output.size, sizeof(event.u.output.data)));
          break;
        default:
          break;
      }
    }

    void run() {
      std::array<pollfd, 3> fds {{
        {.fd = uhid_fd, .events = POLLIN, .revents = 0},
        {.fd = timer_fd, .events = POLLIN, .revents = 0},
        {.fd = wake_fd, .events = POLLIN, .revents = 0},
      }};
      while (!stop.load(std::memory_order_relaxed)) {
        const auto result = poll(fds.data(), fds.size(), -1);
        if (result < 0) {
          if (errno == EINTR) {
            continue;
          }
          BOOST_LOG(error) << "Steam Controller "sv << uniq << ": poll failed: "sv << std::strerror(errno);
          break;
        }
        if (fds[2].revents) {
          break;
        }
        if (fds[0].revents & (POLLERR | POLLHUP | POLLNVAL)) {
          BOOST_LOG(error) << "Steam Controller "sv << uniq << ": /dev/uhid closed"sv;
          break;
        }
        if (fds[0].revents & POLLIN) {
          uhid_event event {};
          ssize_t bytes;
          do {
            bytes = read(uhid_fd, &event, sizeof(event));
          } while (bytes < 0 && errno == EINTR);
          if (bytes == static_cast<ssize_t>(sizeof(event))) {
            handle(event);
          }
        }
        if (fds[1].revents & POLLIN) {
          std::uint64_t expirations;
          if (read(timer_fd, &expirations, sizeof(expirations)) == sizeof(expirations)) {
            // A late wake-up sends one report, not a burst of catch-up reports.
            tick(monotonic_us());
          }
        }
      }
    }
  };

  inputtino::Result<sc26_joypad_t> sc26_joypad_t::create(const inputtino::DeviceDefinition &device, options_t options) {
    auto state = std::make_shared<state_t>();
    state->options = std::move(options);
    state->uniq = state->options.serial.empty() ? std::string {"LVGSC260000"} : state->options.serial;
    state->pad.sc26_explicit = state->options.sc26_explicit;
    state->pad.dual_touchpad = state->options.dual_touchpad;
    state->pad.features.unit_serial.fill('\0');
    std::copy_n(state->uniq.begin(), std::min<std::size_t>(state->uniq.size(), state->pad.features.unit_serial.size() - 1), state->pad.features.unit_serial.begin());

    state->uhid_fd = open("/dev/uhid", O_RDWR | O_CLOEXEC);
    if (state->uhid_fd < 0) {
      return inputtino::Error(std::string {"/dev/uhid: "} + std::strerror(errno));
    }
    state->timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
    state->wake_fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (state->timer_fd < 0 || state->wake_fd < 0) {
      const std::string error = std::strerror(errno);
      close_fd(state->uhid_fd);  // Nothing was created yet, so no UHID_DESTROY.
      return inputtino::Error(error);
    }

    uhid_event event {};
    event.type = UHID_CREATE2;
    auto &create = event.u.create2;
    create.bus = BUS_USB;
    create.vendor = device.vendor_id;
    create.product = device.product_id;
    create.version = device.version;
    create.country = 0;
    static_assert(sc::report_descriptor_size <= sizeof(create.rd_data));
    create.rd_size = static_cast<std::uint16_t>(sc::report_descriptor_size);
    std::memcpy(create.rd_data, sc::report_descriptor, sc::report_descriptor_size);
    const auto copy_text = [](std::string_view text, unsigned char *target, std::size_t capacity) {
      const auto length = std::min(text.size(), capacity - 1);
      std::memcpy(target, text.data(), length);
      target[length] = '\0';
    };
    copy_text(device.name, create.name, sizeof(create.name));
    copy_text(device.device_phys.empty() ? "VIBESHINE_UHID_SC26"sv : std::string_view {device.device_phys}, create.phys, sizeof(create.phys));
    copy_text(state->uniq, create.uniq, sizeof(create.uniq));
    if (!write_uhid_event(state->uhid_fd, event)) {
      const std::string error = std::strerror(errno);
      close_fd(state->uhid_fd);
      return inputtino::Error(error);
    }

    // An even 4 ms clock; the first report goes out at once.
    itimerspec period {};
    period.it_interval.tv_nsec = sc26::report_interval_us * 1000;
    period.it_value.tv_nsec = 1;
    if (timerfd_settime(state->timer_fd, 0, &period, nullptr) < 0) {
      return inputtino::Error(std::string {"timerfd: "} + std::strerror(errno));
    }

    state->thread = std::thread([raw = state.get()] {
      raw->run();
    });
    return sc26_joypad_t {std::move(state)};
  }

  sc26_joypad_t::sc26_joypad_t(std::shared_ptr<state_t> state):
      state_(std::move(state)) {
  }

  sc26_joypad_t::sc26_joypad_t(sc26_joypad_t &&other) noexcept:
      state_(std::exchange(other.state_, nullptr)) {
  }

  sc26_joypad_t &sc26_joypad_t::operator=(sc26_joypad_t &&other) noexcept {
    if (this != &other) {
      sc26_joypad_t previous {std::move(*this)};
      state_ = std::exchange(other.state_, nullptr);
    }
    return *this;
  }

  sc26_joypad_t::~sc26_joypad_t() {
    if (!state_) {
      return;
    }
    state_->stop = true;
    const std::uint64_t one = 1;
    // An eventfd write only fails when the counter would overflow, and then the thread is woken already.
    [[maybe_unused]] const auto written = write(state_->wake_fd, &one, sizeof(one));
    if (state_->thread.joinable()) {
      state_->thread.join();
    }
    // state_t's destructor sends UHID_DESTROY and closes the descriptors.
  }

  std::vector<std::string> sc26_joypad_t::get_nodes() const {
    std::vector<std::string> nodes;
    if (!state_) {
      return nodes;
    }
    std::error_code error;
    for (const auto &entry : std::filesystem::directory_iterator("/sys/class/hidraw", error)) {
      std::ifstream uevent(entry.path() / "device" / "uevent");
      std::string line;
      while (std::getline(uevent, line)) {
        if (line == "HID_UNIQ=" + state_->uniq) {
          nodes.emplace_back((std::filesystem::path {"/dev"} / entry.path().filename()).string());
          break;
        }
      }
    }
    return nodes;
  }

  void sc26_joypad_t::set_pressed_buttons(const unsigned int pressed) {
    std::lock_guard lock {state_->mutex};
    state_->buttons = pressed;
  }

  void sc26_joypad_t::set_triggers(const std::int16_t left, const std::int16_t right) {
    std::lock_guard lock {state_->mutex};
    state_->left_trigger = static_cast<std::uint8_t>(std::clamp<int>(left, 0, 255));
    state_->right_trigger = static_cast<std::uint8_t>(std::clamp<int>(right, 0, 255));
  }

  void sc26_joypad_t::set_stick(const STICK_POSITION stick_type, const short x, const short y) {
    std::lock_guard lock {state_->mutex};
    if (stick_type == LS) {
      state_->left_x = x;
      state_->left_y = y;
    } else {
      state_->right_x = x;
      state_->right_y = y;
    }
  }

  void sc26_joypad_t::touch(const std::uint8_t event, const std::uint32_t pointer, const std::uint8_t touchpad_index, const float x, const float y, const float pressure) {
    std::lock_guard lock {state_->mutex};
    sc26::apply_touch(state_->pad, event, pointer, touchpad_index, x, y, pressure);
  }

  void sc26_joypad_t::set_motion(const std::uint8_t type, const float x, const float y, const float z) {
    std::lock_guard lock {state_->mutex};
    sc26::apply_motion(state_->pad, type, x, y, z);
  }

  void sc26_joypad_t::set_battery(const std::uint8_t state, const std::uint8_t percentage) {
    std::lock_guard lock {state_->mutex};
    sc26::apply_battery(state_->pad, state, percentage);
  }

  void sc26_joypad_t::set_attributes(const steam_controller_attributes_t &attributes) {
    std::uint32_t applied;
    bool handshake_seen;
    {
      std::lock_guard lock {state_->mutex};
      applied = sc26::apply_attributes(
        state_->pad,
        attributes.valid_mask,
        attributes.firmware_build_time,
        attributes.bootloader_build_time,
        attributes.board_revision,
        attributes.capabilities,
        attributes.build_id,
        attributes.unit_serial,
        attributes.board_serial
      );
      handshake_seen = state_->handshake_seen;
    }
    BOOST_LOG(info) << "Steam Controller "sv << state_->uniq << ": applied the real controller's attributes (0x"sv << std::hex << applied << std::dec
                    << ", firmware build time "sv << attributes.firmware_build_time << ')';
    if (handshake_seen) {
      BOOST_LOG(info) << "Steam Controller "sv << state_->uniq << ": the attributes arrived after Steam's handshake; Steam shows them after it reopens the controller"sv;
    }
  }
}  // namespace platf::gamepad
