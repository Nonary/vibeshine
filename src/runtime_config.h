/**
 * @file src/runtime_config.h
 * @brief Declarations for runtime configuration management and autosave.
 */
#pragma once

// standard includes
#include <chrono>
#include <map>
#include <string>
#include <unordered_map>

namespace runtime_config {

  /**
   * @brief Configuration change classification.
   */
  enum class change_type_e {
    immediate,          ///< Apply instantly
    post_session,       ///< Apply when all sessions end
    restart_required    ///< Requires a full application restart
  };

  /**
   * @brief Autosave status.
   */
  enum class autosave_status_e {
    idle,                    ///< No pending changes
    saving,                  ///< Currently saving changes
    recently_applied,        ///< Recently applied immediate changes
    pending_session_end,     ///< Waiting for sessions to end
    pending_restart_approval ///< Waiting for user to approve restart
  };

  /**
   * @brief Pending configuration changes.
   */
  struct pending_changes_t {
    std::unordered_map<std::string, std::string> immediate;
    std::unordered_map<std::string, std::string> post_session;
    std::unordered_map<std::string, std::string> restart_required;
    std::chrono::system_clock::time_point last_change_time;
  };

  /**
   * @brief Autosave state manager.
   */
  class autosave_state_t {
  public:
    autosave_state_t();

    /**
     * @brief Add a configuration change.
     * @param key Configuration key.
     * @param value Configuration value.
     * @param type Change type classification.
     */
    void add_change(const std::string &key, const std::string &value, change_type_e type);

    /**
     * @brief Get current autosave status.
     * @return Current status.
     */
    autosave_status_e get_status() const;

    /**
     * @brief Get pending changes.
     * @return Reference to pending changes.
     */
    const pending_changes_t &get_pending_changes() const;

    /**
     * @brief Apply immediate changes.
     * @return True if successful.
     */
    bool apply_immediate_changes();

    /**
     * @brief Apply post-session changes.
     * @return True if successful.
     */
    bool apply_post_session_changes();

    /**
     * @brief Get restart-required changes for user approval.
     * @return Map of changes requiring restart.
     */
    std::unordered_map<std::string, std::string> get_restart_required_changes() const;

    /**
     * @brief Approve restart-required changes.
     * @return True if successful.
     */
    bool approve_restart_changes();

    /**
     * @brief Cancel restart-required changes.
     */
    void cancel_restart_changes();

    /**
     * @brief Cancel post-session changes.
     */
    void cancel_post_session_changes();

    /**
     * @brief Check if there are active streaming sessions.
     * @return True if sessions are active.
     */
    bool has_active_sessions() const;

    /**
     * @brief Set status (for internal state management).
     * @param status New status.
     */
    void set_status(autosave_status_e status);

  private:
    pending_changes_t pending_changes_;
    autosave_status_e current_status_;
    std::chrono::system_clock::time_point last_apply_time_;
  };

  /**
   * @brief Classify a configuration option.
   * @param key Configuration key.
   * @return Change type for the given key.
   */
  change_type_e classify_config_option(const std::string &key);

  // Global autosave state instance
  extern autosave_state_t autosave_state;

}  // namespace runtime_config