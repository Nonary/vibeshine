/** @file src/steam_big_picture_controls_protocol.h
 * @brief Wire formats for Vibeshine's stream controls inside Steam Big Picture.
 *
 * Three parties exchange messages:
 *
 *   sunshine.exe (service module)  <-- named pipe -->  sunshine_steam_ui_helper.exe  <-- CDP -->  Steam
 *                                                      (injects big_picture_controls.js)
 *
 * Pipe framing: every message is a 4-byte little-endian length followed by that many bytes of
 * UTF-8 JSON. The JSON is an object with a "type" string. Frames larger than max_frame_bytes or
 * empty frames are protocol errors and close the connection.
 *
 * Service -> helper
 *   {"type":"config","protocol":1,"script":"<absolute path>","features":{"power_menu":true,"header_icon":true},
 *    "inject_options":{...}}                                  sent once after hello, and again on change
 *   {"type":"status","status":<status payload, see below>}  whenever the stream state changes
 *   {"type":"check","id":<n>}                               run the script's check-only mode
 *
 * Helper -> service
 *   {"type":"hello","protocol":1,"pid":<n>}
 *   {"type":"state","steam":"<steam_state>","detail":"..."}  connection state, see steam_state below
 *   {"type":"report","script_version":<n>,"react":"...","steam_build":"...","enabled":true,
 *    "errors":<n>,"features":{"<name>":"<value>"}}            after each injection
 *   {"type":"feature","name":"...","value":"..."}            live feature change from the script
 *   {"type":"error","where":"...","message":"..."}           error caught inside Steam
 *   {"type":"action","action":"end_stream"|"disconnect","client":"<client id>"}
 *   {"type":"check_result","id":<n>,"report":{...}}
 *   {"type":"log","level":"info"|"warning"|"error","message":"..."}
 *
 * Script <-> helper, inside Steam (assets/steam/big_picture_controls.js)
 *   The helper sets window.__vibeshineOptions = <inject_options> and evaluates the script. inject_options:
 *     {"checkOnly":false,"features":{"power_menu":true,"header_icon":true},
 *      "simulateMissing":[...],"simulateCrash":[...],"maxErrors":8}       the last three for fault tests
 *   The evaluation returns the script's report, which the helper forwards as a "report" message:
 *     {"protocol":1,"version":<script version>,"react":"...","enabled":true,"errors":<n>,
 *      "features":{"power_menu":"...","header_icon":"...","header_menu":"...","focusable":"...",
 *                  "header_styles":"..."}}                              values start with ok, fallback or off
 *   The helper must refuse a script whose "protocol" differs from protocol::version.
 *   Status is delivered with window.VibeshineBPM.setStatus(<status payload>). The script sends
 *   {"type":"action"|"feature"|"error"|"disabled", ...} through the window.__vibeshineBridge binding,
 *   with the same fields as the helper -> service messages above ("disabled" carries a "reason").
 *
 * Status payload (built by policy::status_payload)
 *   {"version":1,"appRunning":true,"appName":"...",
 *    "clients":[{"id":"...","name":"...","kind":"moonlight"|"browser"}],
 *    "streams":[{"width":n,"height":n,"fps":n,"codec":"...","hdr":false}],
 *    "can":{"endStream":true,"disconnect":true},
 *    "strings":{"endStream":"...","stopStreamingTo":"... %1$s ...", ...}}
 */
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace platf::steam::big_picture::protocol {
  constexpr int version = 1;
  constexpr std::size_t frame_header_bytes = 4;
  constexpr std::size_t max_frame_bytes = 64 * 1024;
  // Client ids are paired-client uuids or WebRTC session ids; both are short.
  constexpr std::size_t max_client_id_bytes = 128;

  namespace message {
    // Service -> helper
    constexpr std::string_view config = "config";
    constexpr std::string_view status = "status";
    constexpr std::string_view check = "check";
    // Helper -> service
    constexpr std::string_view hello = "hello";
    constexpr std::string_view state = "state";
    constexpr std::string_view report = "report";
    constexpr std::string_view feature = "feature";
    constexpr std::string_view error = "error";
    constexpr std::string_view action = "action";
    constexpr std::string_view check_result = "check_result";
    constexpr std::string_view log = "log";
  }  // namespace message

  namespace action {
    constexpr std::string_view end_stream = "end_stream";
    constexpr std::string_view disconnect = "disconnect";
  }  // namespace action

  // Values of the "steam" field in helper "state" messages.
  namespace steam_state {
    constexpr std::string_view not_running = "not_running";
    constexpr std::string_view port_closed = "port_closed";
    constexpr std::string_view port_foreign = "port_foreign";
    constexpr std::string_view connecting = "connecting";
    constexpr std::string_view injected = "injected";
    constexpr std::string_view breaker_open = "breaker_open";
  }  // namespace steam_state

  /** Prefix @p json with its 4-byte little-endian length. Returns nullopt if the payload can't be framed. */
  inline std::optional<std::string> encode_frame(std::string_view json) {
    if (json.empty() || json.size() > max_frame_bytes) {
      return std::nullopt;
    }
    std::string frame;
    frame.reserve(frame_header_bytes + json.size());
    const auto length = static_cast<std::uint32_t>(json.size());
    for (std::size_t i = 0; i < frame_header_bytes; ++i) {
      frame.push_back(static_cast<char>((length >> (8 * i)) & 0xFFU));
    }
    frame.append(json);
    return frame;
  }

  /** Read the payload length from a frame header. Returns nullopt for empty or oversized frames. */
  inline std::optional<std::size_t> decode_frame_length(const std::array<std::uint8_t, frame_header_bytes> &header) {
    std::uint32_t length = 0;
    for (std::size_t i = 0; i < frame_header_bytes; ++i) {
      length |= static_cast<std::uint32_t>(header[i]) << (8 * i);
    }
    if (length == 0 || length > max_frame_bytes) {
      return std::nullopt;
    }
    return length;
  }
}  // namespace platf::steam::big_picture::protocol
