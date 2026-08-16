/**
 * @file src/nvhttp.h
 * @brief Declarations for the nvhttp (GameStream) server.
 */
// macros
#pragma once

// standard includes
#include <chrono>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

// lib includes
#include <boost/property_tree/ptree.hpp>
#include <nlohmann/json.hpp>
#include <Simple-Web-Server/server_https.hpp>

// local includes
#include "crypto.h"
#include "thread_safe.h"

/**
 * @brief Contains all the functions and variables related to the nvhttp (GameStream) server.
 */
namespace nvhttp {

  /**
   * @brief The protocol version.
   * @details The version of the GameStream protocol we are mocking.
   * @note The negative 4th number indicates to Moonlight that this is Sunshine.
   */
  constexpr auto VERSION = "7.1.431.-1";

  /**
   * @brief The GFE version we are replicating.
   */
  constexpr auto GFE_VERSION = "3.23.0.74";

  /**
   * @brief The HTTP port, as a difference from the config port.
   */
  constexpr auto PORT_HTTP = 0;

  /**
   * @brief The HTTPS port, as a difference from the config port.
   */
  constexpr auto PORT_HTTPS = -5;

  /**
   * @brief Start the nvhttp server.
   * @examples
   * nvhttp::start();
   * @examples_end
   */
  void start();

  /**
   * @brief Setup the nvhttp server.
   * @param pkey
   * @param cert
   */
  void setup(const std::string &pkey, const std::string &cert);

  // Remote Input has no retained resource; its catalogue ownership ends with
  // the exact transport generation that created it.
  void notify_remote_input_transport_lost(std::string_view client_uuid, std::uint64_t generation);

  class SunshineHTTPS: public SimpleWeb::HTTPS {
  public:
    SunshineHTTPS(boost::asio::io_context &io_context, boost::asio::ssl::context &ctx):
        SimpleWeb::HTTPS(io_context, ctx) {
    }

    virtual ~SunshineHTTPS() {
      // Gracefully shutdown the TLS connection
      SimpleWeb::error_code ec;
      shutdown(ec);
    }
  };

  enum class PAIR_PHASE {
    NONE,  ///< Sunshine is not in a pairing phase
    GETSERVERCERT,  ///< Sunshine is in the get server certificate phase
    CLIENTCHALLENGE,  ///< Sunshine is in the client challenge phase
    SERVERCHALLENGERESP,  ///< Sunshine is in the server challenge response phase
    CLIENTPAIRINGSECRET  ///< Sunshine is in the client pairing secret phase
  };

  struct pair_session_t {
    std::chrono::steady_clock::time_point created_at = std::chrono::steady_clock::now();

    struct {
      std::string uniqueID = {};
      std::string cert = {};
      std::string name = {};
    } client;

    std::unique_ptr<crypto::aes_t> cipher_key = {};
    std::vector<uint8_t> clienthash = {};

    std::string serversecret = {};
    std::string serverchallenge = {};

    struct {
      util::Either<
        std::shared_ptr<typename SimpleWeb::ServerBase<SimpleWeb::HTTP>::Response>,
        std::shared_ptr<typename SimpleWeb::ServerBase<SunshineHTTPS>::Response>>
        response;
      std::string salt = {};
    } async_insert_pin;

    /**
     * @brief used as a security measure to prevent out of order calls
     */
    PAIR_PHASE last_phase = PAIR_PHASE::NONE;
  };

  /**
   * @brief removes the temporary pairing session
   * @param sess
   */
  void remove_session(const pair_session_t &sess);

  /**
   * @brief Pair, phase 1
   *
   * Moonlight will send a salt and client certificate, we'll also need the user provided pin.
   *
   * PIN and SALT will be used to derive a shared AES key that needs to be stored
   * in order to be used to decrypt_symmetric in the next phases.
   *
   * At this stage we only have to send back our public certificate.
   */
  void getservercert(pair_session_t &sess, boost::property_tree::ptree &tree, const std::string &pin);

  /**
   * @brief Pair, phase 2
   *
   * Using the AES key that we generated in phase 1 we have to decrypt the client challenge,
   *
   * We generate a SHA256 hash with the following:
   *  - Decrypted challenge
   *  - Server certificate signature
   *  - Server secret: a randomly generated secret
   *
   * The hash + server_challenge will then be AES encrypted and sent as the `challengeresponse` in the returned XML
   */
  void clientchallenge(pair_session_t &sess, boost::property_tree::ptree &tree, const std::string &challenge);

  /**
   * @brief Pair, phase 3
   *
   * Moonlight will send back a `serverchallengeresp`: an AES encrypted client hash,
   * we have to send back the `pairingsecret`:
   * using our private key we have to sign the certificate_signature + server_secret (generated in phase 2)
   */
  void serverchallengeresp(pair_session_t &sess, boost::property_tree::ptree &tree, const std::string &encrypted_response);

  /**
   * @brief Pair, phase 4 (final)
   *
   * We now have to use everything we exchanged before in order to verify and finally pair the clients
   *
   * We'll check the client_hash obtained at phase 3, it should contain the following:
   *   - The original server_challenge
   *   - The signature of the X509 client_cert
   *   - The unencrypted client_pairing_secret
   * We'll check that SHA256(server_challenge + client_public_cert_signature + client_secret) == client_hash
   *
   * Then using the client certificate public key we should be able to verify that
   * the client secret has been signed by Moonlight
   */
  void clientpairingsecret(pair_session_t &sess, std::shared_ptr<safe::queue_t<crypto::x509_t>> &add_cert, boost::property_tree::ptree &tree, const std::string &client_pairing_secret);

  /**
   * @brief Compare the user supplied pin to the Moonlight pin.
   * @param pin The user supplied pin.
   * @param name The user supplied name.
   * @return `true` if the pin is correct, `false` otherwise.
   * @examples
   * bool pin_status = nvhttp::pin("1234", "laptop");
   * @examples_end
   */
  bool pin(std::string pin, std::string name);

  /**
   * @brief Pick the client label used for display-facing behavior.
   */
  std::string display_client_name_for_session(const std::string &paired_name, const std::string &device_name, const std::string &host_name);

  /**
   * @brief Remove single client.
   * @param uuid The UUID of the client to remove.
   * @examples
   * nvhttp::unpair_client("4D7BB2DD-5704-A405-B41C-891A022932E1");
   * @examples_end
   */
  bool unpair_client(std::string_view uuid);

  /**
   * @brief Enable or disable a client.
   * @param uuid The UUID of the client.
   * @param enabled Whether the client should be enabled.
   * @return true if the client was found and updated.
   */
  bool set_client_enabled(std::string_view uuid, bool enabled);
  bool has_client_uuid(std::string_view uuid);
  std::string get_cert_by_uuid(std::string_view uuid);

  /**
   * @brief Get all paired clients.
   * @return The list of all paired clients.
   * @examples
   * nlohmann::json clients = nvhttp::get_all_clients();
   * @examples_end
   */
  nlohmann::json get_all_clients();

  nlohmann::json get_remote_display_layout();
  bool set_remote_display_layout(const nlohmann::json &layout, std::string &error);

  /**
   * @brief Record a client's last seen time (seconds since epoch).
   */
  void mark_client_last_seen(const std::string &uuid);

  /**
   * @brief Update stored settings for a paired client.
   * @return True if the client was found and updated.
   */
  bool update_device_info(
    const std::string &uuid,
    const std::string &name,
    const std::string &display_mode,
    const std::string &output_name_override,
    bool always_use_virtual_display,
    const std::string &virtual_display_mode,
    const std::string &virtual_display_layout,
    std::optional<std::unordered_map<std::string, std::string>> config_overrides,
    bool prefer_10bit_sdr,
    std::optional<bool> terminal_session_enabled,
    std::optional<bool> steam_offline_isolation,
    std::optional<std::string> hdr_profile
  );

  /**
   * @brief Disconnect any active sessions for a paired client.
   * @return True if one or more sessions were stopped.
   */
  bool disconnect_client(const std::string &uuid);

  /**
   * @brief Whether a paired client is opted into 10-bit SDR instead of HDR.
   */
  bool get_client_prefer_10bit_sdr(const std::string &uuid);

  /**
   * @brief Whether a paired client launches configured apps in a private Windows seat.
   */
  bool get_client_terminal_session_enabled(const std::string &uuid);

  /** Whether persistent or retained one-shot terminal ownership is active. */
  bool get_client_terminal_session_active(const std::string &uuid);

  /**
   * @brief Get a copy of a client's runtime config overrides.
   */
  std::unordered_map<std::string, std::string> get_client_config_overrides(const std::string &uuid);

  /**
   * @brief Encoder capabilities safe to expose to the browser WebRTC UI.
   * @details Resolves the same selected-adapter capability view used for HTTP
   *          protocol advertisement. A false `probe_complete` means the host
   *          has not safely verified an encoder for the current capture target.
   */
  struct web_stream_capabilities_t {
    bool probe_complete {false};
    bool h264 {false};
    bool hevc {false};
    bool av1 {false};
    bool hevc_hdr {false};
    bool av1_hdr {false};
  };

  web_stream_capabilities_t get_web_stream_capabilities();

  /**
   * @brief Serialize shared stream start and final teardown across RTSP and WebRTC.
   *
   * Acquire after the RTSP launch-request mutex, before protocol capture locks.
   * Starts hold it from their first idle-state observation through pending-owner
   * publication; final teardown holds it through its shared cleanup decision.
   */
  std::mutex &stream_lifecycle_mutex();

  /**
   * @brief Persist a per-client HDR color profile selection (Windows only).
   * @return True if the client was found and updated.
   */
  bool set_client_hdr_profile(const std::string &uuid, const std::string &hdr_profile);

  /**
   * @brief Remove all paired clients.
   * @examples
   * nvhttp::erase_all_clients();
   * @examples_end
   */
  void erase_all_clients();

  /**
   * @brief Persist current nvhttp-related state (paired clients, update subsystem markers, etc.).
   * @note Exposed so subsystems (e.g. update) can trigger a save after mutating persisted fields.
   */
  void save_state();
}  // namespace nvhttp
