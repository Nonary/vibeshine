/**
 * @file src/system_tray.cpp
 * @brief Definitions for the system tray icon and notification system.
 */
// macros
#if defined SUNSHINE_TRAY && SUNSHINE_TRAY >= 1

  #if defined(_WIN32)
    #define WIN32_LEAN_AND_MEAN
    #include <accctrl.h>
    #include <aclapi.h>
    #include <Windows.h>
    #define TRAY_ICON WEB_DIR "images/sunshine.ico"
    #define TRAY_ICON_PLAYING WEB_DIR "images/sunshine-playing.ico"
    #define TRAY_ICON_PAUSING WEB_DIR "images/sunshine-pausing.ico"
    #define TRAY_ICON_LOCKED WEB_DIR "images/sunshine-locked.ico"
  #elif defined(__linux__) || defined(linux) || defined(__linux)
    #define TRAY_ICON SUNSHINE_TRAY_PREFIX "-tray"
    #define TRAY_ICON_PLAYING SUNSHINE_TRAY_PREFIX "-playing"
    #define TRAY_ICON_PAUSING SUNSHINE_TRAY_PREFIX "-pausing"
    #define TRAY_ICON_LOCKED SUNSHINE_TRAY_PREFIX "-locked"
  #elif defined(__APPLE__) || defined(__MACH__)
    #define TRAY_ICON WEB_DIR "images/logo-sunshine-16.png"
    #define TRAY_ICON_PLAYING WEB_DIR "images/sunshine-playing-16.png"
    #define TRAY_ICON_PAUSING WEB_DIR "images/sunshine-pausing-16.png"
    #define TRAY_ICON_LOCKED WEB_DIR "images/sunshine-locked-16.png"
    #include <dispatch/dispatch.h>
  #endif

  // standard includes
  #include <atomic>
  #include <csignal>
  #include <condition_variable>
  #include <cwchar>
  #include <functional>
  #include <mutex>
  #include <queue>
  #include <string>
  #include <thread>
  #include <utility>

  // lib includes
  #include <boost/filesystem.hpp>
  #include <tray/src/tray.h>

  // local includes
  #include "confighttp.h"
  #include "logging.h"
  #include "platform/common.h"
  #include "platform/windows/service_constants.h"
  #include "process.h"
  #include "src/entry_handler.h"
  #include "update.h"

using namespace std::literals;

// system_tray namespace
namespace system_tray {
  static std::atomic<bool> tray_initialized = false;
  static std::thread tray_thread;
#ifdef _WIN32
  static std::mutex tray_action_mutex;
  static std::condition_variable tray_action_cv;
  static std::queue<std::function<void()>> tray_actions;
  static std::atomic<bool> tray_shutdown_requested = false;
#endif

  static void tray_log_bridge(enum tray_log_level level, const char *message) {
    if (!message) {
      return;
    }
    switch (level) {
      case TRAY_LOG_DEBUG:
        BOOST_LOG(debug) << message;
        break;
      case TRAY_LOG_INFO:
        BOOST_LOG(info) << message;
        break;
      case TRAY_LOG_WARNING:
        BOOST_LOG(warning) << message;
        break;
      case TRAY_LOG_ERROR:
        BOOST_LOG(error) << message;
        break;
    }
  }

  void tray_open_ui_cb([[maybe_unused]] struct tray_menu *item) {
    BOOST_LOG(info) << "Opening UI from system tray"sv;
    launch_ui();
  }

  void tray_restart_cb([[maybe_unused]] struct tray_menu *item) {
    BOOST_LOG(info) << "Restarting from system tray"sv;

    platf::restart();
  }

  void tray_quit_cb([[maybe_unused]] struct tray_menu *item) {
    BOOST_LOG(info) << "Quitting from system tray"sv;

  #ifdef _WIN32
    // If we're running in a service, return a special status to
    // tell it to terminate too, otherwise it will just respawn us.
    if (GetEnvironmentVariableW(platf::service_launch::launched_by_service_env_var, nullptr, 0) != 0) {
      lifetime::exit_sunshine(ERROR_SHUTDOWN_IN_PROGRESS, true);
      return;
    }
  #endif

    lifetime::exit_sunshine(0, true);
  }

  // Tray menu
  static struct tray tray = {
    .icon = TRAY_ICON,
    .tooltip = PROJECT_NAME,
    .menu =
      (struct tray_menu[]) {
        // todo - use boost/locale to translate menu strings
        {.text = "Open Sunshine", .cb = tray_open_ui_cb},
        {.text = "-"},
        {.text = "Check for Update", .cb = [](tray_menu *) {
           BOOST_LOG(info) << "Manual update check requested from tray"sv;
           update::trigger_check(true);
         }},
        {.text = "Restart", .cb = tray_restart_cb},
        {.text = "Quit", .cb = tray_quit_cb},
        {.text = nullptr}
      },
    .iconPathCount = 4,
    .allIconPaths = {TRAY_ICON, TRAY_ICON_LOCKED, TRAY_ICON_PLAYING, TRAY_ICON_PAUSING},
  };

  // Persistent storage for tooltip/notification strings to avoid dangling pointers.
  static std::string s_tooltip;
  static std::string s_notification_title;
  static std::string s_notification_text;

  static void apply_tray_state(
    const char *icon,
    std::string tooltip,
    std::string notification_title = {},
    std::string notification_text = {},
    const char *notification_icon = nullptr,
    void (*notification_cb)() = nullptr
  ) {
    s_tooltip = std::move(tooltip);
    s_notification_title = std::move(notification_title);
    s_notification_text = std::move(notification_text);

    tray.icon = icon;
    tray.tooltip = s_tooltip.empty() ? nullptr : s_tooltip.c_str();
    tray.notification_title = s_notification_title.empty() ? nullptr : s_notification_title.c_str();
    tray.notification_text = s_notification_text.empty() ? nullptr : s_notification_text.c_str();
    tray.notification_icon = notification_icon;
    tray.notification_cb = notification_cb;
    tray_update(&tray);
  }

#ifdef _WIN32
  static void clear_pending_quit_messages() {
    MSG msg {};
    while (PeekMessageA(&msg, nullptr, WM_QUIT, WM_QUIT, PM_REMOVE)) {}
  }

  static void run_pending_tray_actions() {
    std::queue<std::function<void()>> pending_actions;
    {
      std::lock_guard lock(tray_action_mutex);
      pending_actions.swap(tray_actions);
    }

    while (!pending_actions.empty()) {
      auto action = std::move(pending_actions.front());
      pending_actions.pop();
      action();
    }
  }
#endif

  template<typename Fn>
  static void run_on_tray_thread(Fn &&fn) {
    if (!tray_initialized.load()) {
      return;
    }

#ifdef _WIN32
    {
      std::lock_guard lock(tray_action_mutex);
      tray_actions.emplace(std::forward<Fn>(fn));
    }
    tray_action_cv.notify_one();
#else
    fn();
#endif
  }

  int system_tray() {
  #ifdef _WIN32
    // If we're running as SYSTEM, Explorer.exe will not have permission to open our thread handle
    // to monitor for thread termination. If Explorer fails to open our thread, our tray icon
    // will persist forever if we terminate unexpectedly. To avoid this, we will modify our thread
    // DACL to add an ACE that allows SYNCHRONIZE access to Everyone.
    {
      PACL old_dacl;
      PSECURITY_DESCRIPTOR sd;
      auto error = GetSecurityInfo(GetCurrentThread(), SE_KERNEL_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, &old_dacl, nullptr, &sd);
      if (error != ERROR_SUCCESS) {
        BOOST_LOG(warning) << "GetSecurityInfo() failed: "sv << error;
        return 1;
      }

      auto free_sd = util::fail_guard([sd]() {
        LocalFree(sd);
      });

      SID_IDENTIFIER_AUTHORITY sid_authority = SECURITY_WORLD_SID_AUTHORITY;
      PSID world_sid;
      if (!AllocateAndInitializeSid(&sid_authority, 1, SECURITY_WORLD_RID, 0, 0, 0, 0, 0, 0, 0, &world_sid)) {
        error = GetLastError();
        BOOST_LOG(warning) << "AllocateAndInitializeSid() failed: "sv << error;
        return 1;
      }

      auto free_sid = util::fail_guard([world_sid]() {
        FreeSid(world_sid);
      });

      EXPLICIT_ACCESS ea {};
      ea.grfAccessPermissions = SYNCHRONIZE;
      ea.grfAccessMode = GRANT_ACCESS;
      ea.grfInheritance = NO_INHERITANCE;
      ea.Trustee.TrusteeForm = TRUSTEE_IS_SID;
      ea.Trustee.ptstrName = (LPSTR) world_sid;

      PACL new_dacl;
      error = SetEntriesInAcl(1, &ea, old_dacl, &new_dacl);
      if (error != ERROR_SUCCESS) {
        BOOST_LOG(warning) << "SetEntriesInAcl() failed: "sv << error;
        return 1;
      }

      auto free_new_dacl = util::fail_guard([new_dacl]() {
        LocalFree(new_dacl);
      });

      error = SetSecurityInfo(GetCurrentThread(), SE_KERNEL_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, new_dacl, nullptr);
      if (error != ERROR_SUCCESS) {
        BOOST_LOG(warning) << "SetSecurityInfo() failed: "sv << error;
        return 1;
      }
    }

    // Wait for the shell to be initialized before registering the tray icon.
    // This ensures the tray icon works reliably after a logoff/logon cycle.
    while (GetShellWindow() == nullptr) {
      Sleep(1000);
    }

    auto wait_for_default_desktop = []() {
      constexpr int attempts = 60;
      for (int attempt = 0; attempt < attempts; ++attempt) {
        HDESK desktop = OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS | DESKTOP_ENUMERATE);
        if (desktop != nullptr) {
          auto close_desktop = util::fail_guard([desktop]() {
            CloseDesktop(desktop);
          });

          WCHAR desktop_name[256] = {};
          DWORD required_length = 0;
          if (GetUserObjectInformationW(desktop, UOI_NAME, desktop_name, sizeof(desktop_name), &required_length)) {
            if (_wcsicmp(desktop_name, L"Default") == 0) {
              return true;
            }
          }
        }

        Sleep(1000);
      }

      return false;
    };

    if (!wait_for_default_desktop()) {
      BOOST_LOG(warning) << "Timed out waiting for interactive desktop; system tray may not appear"sv;
    } else {
      BOOST_LOG(debug) << "Interactive desktop ready for tray initialization"sv;
    }
  #endif

    int attempt = 0;
    int tray_init_result = -1;
    while (tray_init_result < 0 && attempt < 30) {
#ifdef _WIN32
      if (tray_shutdown_requested.load()) {
        return 0;
      }
#endif
      tray_init_result = tray_init(&tray);
      if (tray_init_result >= 0) {
        break;
      }
#ifdef _WIN32
      clear_pending_quit_messages();
#endif
      BOOST_LOG(warning) << "Failed to create system tray (attempt "sv << attempt + 1 << ')';
      std::this_thread::sleep_for(2s);
      ++attempt;
    }

    if (tray_init_result < 0) {
      BOOST_LOG(warning) << "Failed to create system tray after retries"sv;
      return 1;
    } else {
      BOOST_LOG(info) << "System tray created"sv;
    }

    tray_initialized = true;
#ifdef _WIN32
    while (!tray_shutdown_requested.load()) {
      run_pending_tray_actions();
      if (tray_loop(0) < 0) {
        break;
      }

      std::unique_lock lock(tray_action_mutex);
      tray_action_cv.wait_for(lock, 50ms, []() {
        return tray_shutdown_requested.load() || !tray_actions.empty();
      });
    }

    tray_exit();
    clear_pending_quit_messages();
#else
    while (tray_loop(1) == 0) {
      BOOST_LOG(debug) << "System tray loop"sv;
    }
#endif

    tray_initialized = false;
    return 0;
  }

  void run_tray() {
    // create the system tray
    tray_set_log_callback(&tray_log_bridge);
  #if defined(__APPLE__) || defined(__MACH__)
    // macOS requires that UI elements be created on the main thread
    // creating tray using dispatch queue does not work, although the code doesn't actually throw any (visible) errors

    // dispatch_async(dispatch_get_main_queue(), ^{
    //   system_tray();
    // });

    BOOST_LOG(info) << "system_tray() is not yet implemented for this platform."sv;
  #else  // Windows, Linux
    if (tray_thread.joinable()) {
      return;
    }
#ifdef _WIN32
    tray_shutdown_requested = false;
#endif
    tray_thread = std::thread(system_tray);
  #endif
  }

  int end_tray() {
    tray_initialized = false;
#ifdef _WIN32
    tray_shutdown_requested = true;
    tray_action_cv.notify_one();
#else
    if (tray_thread.joinable()) {
      tray_exit();
    }
#endif
    if (tray_thread.joinable()) {
      // end_tray() can run on the tray thread itself: a tray menu action
      // Keep self-join protection for any tray-thread caller. Joining our own
      // thread throws std::system_error(resource_deadlock_would_occur); detach
      // in that case and let the tray loop unwind after the callback returns.
      // The main-thread shutdown path still joins.
      if (tray_thread.get_id() == std::this_thread::get_id()) {
        tray_thread.detach();
      } else {
        tray_thread.join();
      }
    }
#ifdef _WIN32
    std::lock_guard lock(tray_action_mutex);
    std::queue<std::function<void()>> empty;
    tray_actions.swap(empty);
#endif
    return 0;
  }

  void update_tray_playing(std::string app_name) {
    run_on_tray_thread([app_name = std::move(app_name)]() {
      const auto notification_text = "Streaming started for " + app_name;
      apply_tray_state(TRAY_ICON_PLAYING, PROJECT_NAME);
      apply_tray_state(TRAY_ICON_PLAYING, notification_text, "Stream Started", notification_text, TRAY_ICON_PLAYING);
    });
  }

  void update_tray_pausing(std::string app_name) {
    run_on_tray_thread([app_name = std::move(app_name)]() {
      const auto notification_text = "Streaming paused for " + app_name;
      apply_tray_state(TRAY_ICON_PAUSING, PROJECT_NAME);
      apply_tray_state(TRAY_ICON_PAUSING, notification_text, "Stream Paused", notification_text, TRAY_ICON_PAUSING);
    });
  }

  void update_tray_stopped(std::string app_name) {
    run_on_tray_thread([app_name = std::move(app_name)]() {
      auto notification_text = "Application " + app_name + " successfully stopped";
      apply_tray_state(TRAY_ICON, PROJECT_NAME);
      apply_tray_state(TRAY_ICON, PROJECT_NAME, "Application Stopped", std::move(notification_text), TRAY_ICON);
    });
  }

  void update_tray_require_pin() {
    run_on_tray_thread([]() {
      apply_tray_state(TRAY_ICON, PROJECT_NAME);
      apply_tray_state(
        TRAY_ICON,
        PROJECT_NAME,
        "Incoming Pairing Request",
        "Click here to complete the pairing process",
        TRAY_ICON_LOCKED,
        []() {
          launch_ui("/clients");
        }
      );
    });
  }

  void update_tray_vigem_missing() {
    run_on_tray_thread([]() {
      apply_tray_state(TRAY_ICON, PROJECT_NAME);
      apply_tray_state(
        TRAY_ICON,
        PROJECT_NAME,
        "Gamepad Input Unavailable",
        "ViGEm is not installed. Click for setup info",
        TRAY_ICON,
        []() {
          // Open Dashboard for more information
          launch_ui("/");
        }
      );
    });
  }

  void tray_notify(const char *title, const char *text, void (*cb)()) {
    const auto notification_title = title ? std::string {title} : std::string {};
    const auto notification_text = text ? std::string {text} : std::string {};

    run_on_tray_thread([notification_title, notification_text, cb]() {
      apply_tray_state(TRAY_ICON, PROJECT_NAME);
      apply_tray_state(TRAY_ICON, PROJECT_NAME, notification_title, notification_text, TRAY_ICON, cb);
    });
  }

}  // namespace system_tray
#endif
