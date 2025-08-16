/**
 * @file src/runtime_config.cpp
 * @brief Definitions for runtime configuration management and autosave.
 */
// standard includes
#include <thread>

// local includes
#include "runtime_config.h"
#include "config.h"
#include "file_handler.h"
#include "logging.h"
#include "rtsp.h"
#include "stream.h"

using namespace std::literals;

namespace runtime_config {

  // Global autosave state instance
  autosave_state_t autosave_state;

  autosave_state_t::autosave_state_t() : current_status_(autosave_status_e::idle) {}

  void autosave_state_t::add_change(const std::string &key, const std::string &value, change_type_e type) {
    pending_changes_.last_change_time = std::chrono::system_clock::now();

    switch (type) {
      case change_type_e::immediate:
        pending_changes_.immediate[key] = value;
        break;
      case change_type_e::post_session:
        pending_changes_.post_session[key] = value;
        break;
      case change_type_e::restart_required:
        pending_changes_.restart_required[key] = value;
        break;
    }

    // Update status based on pending changes
    if (!pending_changes_.restart_required.empty()) {
      current_status_ = autosave_status_e::pending_restart_approval;
    } else if (!pending_changes_.post_session.empty() && has_active_sessions()) {
      current_status_ = autosave_status_e::pending_session_end;
    } else if (!pending_changes_.immediate.empty()) {
      // Will be processed immediately
      current_status_ = autosave_status_e::saving;
    }
  }

  autosave_status_e autosave_state_t::get_status() const {
    return current_status_;
  }

  const pending_changes_t &autosave_state_t::get_pending_changes() const {
    return pending_changes_;
  }

  bool autosave_state_t::apply_immediate_changes() {
    if (pending_changes_.immediate.empty()) {
      return true;
    }

    current_status_ = autosave_status_e::saving;

    try {
      // Apply each immediate change individually
      for (const auto &[key, value] : pending_changes_.immediate) {
        std::unordered_map<std::string, std::string> single_change;
        single_change[key] = value;
        config::apply_config(std::move(single_change));
        BOOST_LOG(info) << "Applied immediate config change: " << key << " = " << value;
      }

      pending_changes_.immediate.clear();
      current_status_ = autosave_status_e::recently_applied;
      last_apply_time_ = std::chrono::system_clock::now();

      // Reset to idle after a brief period
      std::thread([this]() {
        std::this_thread::sleep_for(std::chrono::seconds(3));
        if (current_status_ == autosave_status_e::recently_applied) {
          current_status_ = autosave_status_e::idle;
        }
      }).detach();

      return true;
    } catch (const std::exception &e) {
      BOOST_LOG(error) << "Failed to apply immediate config changes: " << e.what();
      current_status_ = autosave_status_e::idle;
      return false;
    }
  }

  bool autosave_state_t::apply_post_session_changes() {
    if (pending_changes_.post_session.empty()) {
      return true;
    }

    if (has_active_sessions()) {
      BOOST_LOG(info) << "Cannot apply post-session changes: active sessions detected";
      return false;
    }

    current_status_ = autosave_status_e::saving;

    try {
      // Apply all post-session changes
      config::apply_config(std::move(pending_changes_.post_session));
      pending_changes_.post_session.clear();
      
      current_status_ = autosave_status_e::recently_applied;
      last_apply_time_ = std::chrono::system_clock::now();

      // Reset to idle after a brief period
      std::thread([this]() {
        std::this_thread::sleep_for(std::chrono::seconds(3));
        if (current_status_ == autosave_status_e::recently_applied) {
          current_status_ = autosave_status_e::idle;
        }
      }).detach();

      return true;
    } catch (const std::exception &e) {
      BOOST_LOG(error) << "Failed to apply post-session config changes: " << e.what();
      current_status_ = autosave_status_e::idle;
      return false;
    }
  }

  std::unordered_map<std::string, std::string> autosave_state_t::get_restart_required_changes() const {
    return pending_changes_.restart_required;
  }

  bool autosave_state_t::approve_restart_changes() {
    if (pending_changes_.restart_required.empty()) {
      return true;
    }

    try {
      // Save restart-required changes to config file
      std::stringstream config_stream;
      for (const auto &[k, v] : pending_changes_.restart_required) {
        config_stream << k << " = " << v << std::endl;
      }
      
      // Append to existing config
      std::string existing_config = file_handler::read_file(config::sunshine.config_file.c_str());
      config_stream << existing_config;
      file_handler::write_file(config::sunshine.config_file.c_str(), config_stream.str());
      
      pending_changes_.restart_required.clear();
      current_status_ = autosave_status_e::idle;
      
      BOOST_LOG(info) << "Restart-required config changes approved and saved";
      return true;
    } catch (const std::exception &e) {
      BOOST_LOG(error) << "Failed to approve restart changes: " << e.what();
      return false;
    }
  }

  void autosave_state_t::cancel_restart_changes() {
    pending_changes_.restart_required.clear();
    current_status_ = autosave_status_e::idle;
    BOOST_LOG(info) << "Restart-required config changes cancelled";
  }

  void autosave_state_t::cancel_post_session_changes() {
    pending_changes_.post_session.clear();
    if (pending_changes_.restart_required.empty()) {
      current_status_ = autosave_status_e::idle;
    }
    BOOST_LOG(info) << "Post-session config changes cancelled";
  }

  bool autosave_state_t::has_active_sessions() const {
    return rtsp_stream::session_count() > 0;
  }

  void autosave_state_t::set_status(autosave_status_e status) {
    current_status_ = status;
  }

  change_type_e classify_config_option(const std::string &key) {
    // Classification map based on the requirements analysis
    static const std::unordered_map<std::string, change_type_e> classification_map = {
      // Immediate changes (UI preferences, display settings that don't require restart)
      {"locale", change_type_e::immediate},
      {"min_log_level", change_type_e::immediate},
      {"notify_pre_releases", change_type_e::immediate},
      {"sunshine_name", change_type_e::immediate},
      
      // Audio/Video settings that can be applied immediately
      {"audio_sink", change_type_e::immediate},
      {"virtual_sink", change_type_e::immediate},
      {"adapter_name", change_type_e::immediate},
      {"output_name", change_type_e::immediate},
      
      // Session-dependent changes (affects streaming sessions)
      {"qp", change_type_e::post_session},
      {"hevc_mode", change_type_e::post_session},
      {"av1_mode", change_type_e::post_session},
      {"sw_preset", change_type_e::post_session},
      {"sw_tune", change_type_e::post_session},
      {"fec_percentage", change_type_e::post_session},
      {"lan_encryption_mode", change_type_e::post_session},
      {"wan_encryption_mode", change_type_e::post_session},
      {"ping_timeout", change_type_e::post_session},
      
      // Input settings
      {"controller", change_type_e::post_session},
      {"keyboard", change_type_e::post_session},
      {"mouse", change_type_e::post_session},
      {"gamepad", change_type_e::post_session},
      {"ds4_back_as_touchpad_click", change_type_e::post_session},
      {"motion_as_ds4", change_type_e::post_session},
      {"touchpad_as_ds4", change_type_e::post_session},
      {"back_button_timeout", change_type_e::post_session},
      {"key_repeat_delay", change_type_e::post_session},
      {"key_repeat_frequency", change_type_e::post_session},
      {"always_send_scancodes", change_type_e::post_session},
      {"high_resolution_scrolling", change_type_e::post_session},
      {"native_pen_touch", change_type_e::post_session},
      {"keybindings", change_type_e::post_session},
      
      // Restart required (network ports, core services, file paths)
      {"port", change_type_e::restart_required},
      {"address_family", change_type_e::restart_required},
      {"upnp", change_type_e::restart_required},
      {"external_ip", change_type_e::restart_required},
      {"file_apps", change_type_e::restart_required},
      {"file_state", change_type_e::restart_required},
      {"credentials_file", change_type_e::restart_required},
      {"log_path", change_type_e::restart_required},
      {"cert", change_type_e::restart_required},
      {"pkey", change_type_e::restart_required},
      {"origin_web_ui_allowed", change_type_e::restart_required},
      {"global_prep_cmd", change_type_e::restart_required},
    };

    auto it = classification_map.find(key);
    if (it != classification_map.end()) {
      return it->second;
    }
    
    // Default to restart required for unknown options (safer)
    return change_type_e::restart_required;
  }

}  // namespace runtime_config