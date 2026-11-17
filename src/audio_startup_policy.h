/** Session-boundary discrete source settings and shared-context admission. */
#pragma once
#include "audio_usb_source.h"

#include <functional>
#include <atomic>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>

namespace audio::startup {
  struct settings_t {
    std::string source {"off"};
    int bitrate {600000};
    usb_source_config_t usb {};
    std::string usb_evidence_root {};
    unsigned usb_session_seconds=0; // Internal test builds only; ignored in product configuration.
  };

  // Readers take value snapshots. Replacement is deferred until sessions end.
  class settings_store_t {
  public:
    settings_t get() const { const auto pinned=pinned_.load();return pinned?*pinned:defaults_; }
    bool publish(settings_t settings) {
      std::shared_ptr<const settings_t> empty;
      return pinned_.compare_exchange_strong(empty,std::make_shared<const settings_t>(std::move(settings)));
    }
    void replace(settings_t settings) { pinned_.store(std::make_shared<const settings_t>(std::move(settings))); }
    bool preserve_on_reload(std::unordered_map<std::string, std::string> &values) const {
      const auto snapshot=this->pinned_.load();
      if (!snapshot) return false;
      bool changed = false;
      const auto preserve = [&](const char *key, const std::string &value, const std::string &default_value) {
        const auto existing = values.find(key);
        if (existing == values.end() ? value != default_value : existing->second != value) changed = true;
        values.insert_or_assign(key, value);
      };
      preserve("audio_714_source", snapshot->source, defaults_.source);
      preserve("audio_714_bitrate", std::to_string(snapshot->bitrate), std::to_string(defaults_.bitrate));
#ifdef VIBE_AUDIO_TEST_INSTRUMENTATION
      preserve("audio_usb_pipe",snapshot->usb.pipe,defaults_.usb.pipe);
      preserve("audio_usb_bridge_executable",snapshot->usb.bridge_executable,defaults_.usb.bridge_executable);
      preserve("audio_usb_bridge_sha256",snapshot->usb.bridge_sha256,defaults_.usb.bridge_sha256);
      preserve("audio_usb_endpoint",snapshot->usb.endpoint,defaults_.usb.endpoint);
      preserve("audio_usb_instance",snapshot->usb.usb_instance,defaults_.usb.usb_instance);
      preserve("audio_usb_controller_instance",snapshot->usb.controller_instance,defaults_.usb.controller_instance);
      preserve("audio_usb_serial",snapshot->usb.serial,defaults_.usb.serial);
      preserve("audio_usb_service",snapshot->usb.service,defaults_.usb.service);
      preserve("audio_usb_port",std::to_string(snapshot->usb.port),std::to_string(defaults_.usb.port));
#endif
      preserve("audio_usb_sample_format",snapshot->usb.sample_format,defaults_.usb.sample_format);
#ifdef VIBE_AUDIO_TEST_INSTRUMENTATION
      preserve("audio_usb_evidence_root",snapshot->usb_evidence_root,defaults_.usb_evidence_root);
      preserve("audio_usb_session_seconds",std::to_string(snapshot->usb_session_seconds),std::to_string(defaults_.usb_session_seconds));
#endif
      return changed;
    }
  private:
    settings_t defaults_;
    std::atomic<std::shared_ptr<const settings_t>> pinned_;
  };

  inline bool shared_context_allowed(const settings_t &settings, int requested_channels = 0) {
    return settings.source == "off" && requested_channels != 12;
  }

  // This is the callback selection used by NVHTTP's real lazy display hold.
  inline std::function<std::shared_ptr<void>()> snapshot_callback(
      const settings_t &settings, int requested_channels, std::function<std::shared_ptr<void>()> capture) {
    return shared_context_allowed(settings, requested_channels) ? std::move(capture) : std::function<std::shared_ptr<void>()> {};
  }
}  // namespace audio::startup
