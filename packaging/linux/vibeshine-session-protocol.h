#ifndef VIBESHINE_SESSION_PROTOCOL_H
#define VIBESHINE_SESSION_PROTOCOL_H

#include <stdint.h>

#define VIBESHINE_SESSION_BROKER_SOCKET "/run/vibeshine/session-broker.sock"
#define VIBESHINE_SESSION_PROTOCOL_MAGIC UINT32_C(0x56534252)
#define VIBESHINE_SESSION_PROTOCOL_VERSION UINT16_C(1)
/* Complete activation/restore transactions need up to seven properties for
 * each output in the bounded 64-output topology, plus the operation verb.
 * The independent message byte limit still applies to every request. */
#define VIBESHINE_SESSION_PROTOCOL_MAX_DISPLAY_PROPERTIES (UINT32_C(64) * UINT32_C(7))
#define VIBESHINE_SESSION_PROTOCOL_MAX_ARGUMENTS (VIBESHINE_SESSION_PROTOCOL_MAX_DISPLAY_PROPERTIES + UINT32_C(1))
#define VIBESHINE_SESSION_PROTOCOL_MAX_MESSAGE UINT32_C(131072)
#define VIBESHINE_SESSION_PROTOCOL_OUTPUT_CHUNK UINT32_C(16384)
/* No terminal broker acknowledgement was received; mutation completion is unknown. */
#define VIBESHINE_SESSION_COMPLETION_UNKNOWN 125
/* KScreen requests are cancelled and reaped by the broker before reporting exit. */
#define VIBESHINE_SESSION_DISPLAY_TIMEOUT_MS UINT64_C(5000)

enum vibeshine_session_message_type {
  VIBESHINE_SESSION_REQUEST = 1,
  VIBESHINE_SESSION_STDOUT = 2,
  VIBESHINE_SESSION_STDERR = 3,
  VIBESHINE_SESSION_EXIT = 4,
};

/*
 * This protocol is local to one machine and intentionally uses native byte
 * order. Fixed-width fields and an exact version still make every message
 * self-describing and reject ABI or implementation drift instead of silently
 * reinterpreting it.
 */
struct vibeshine_session_message {
  uint32_t magic;
  uint16_t version;
  uint16_t type;
  uint32_t payload_length;
  uint32_t argument_count;
  uint64_t generation;
  int32_t status;
  uint32_t reserved;
};

_Static_assert(sizeof(struct vibeshine_session_message) == 32,
               "session broker protocol header must remain fixed-size");

#endif
