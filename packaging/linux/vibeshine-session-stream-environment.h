#ifndef VIBESHINE_SESSION_STREAM_ENVIRONMENT_H
#define VIBESHINE_SESSION_STREAM_ENVIRONMENT_H

#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

// Application requests may carry these stream-owned values as literal argv
// data. They never become the capability-bearing broker's environment. The
// broker validates the complete list before dropping identity, then supplies
// it only to the selected user's transient application service.
#define VIBESHINE_STREAM_ENVIRONMENT_MAX_ENTRIES 32U
#define VIBESHINE_STREAM_ENVIRONMENT_MAX_BYTES 16384U

enum vibeshine_stream_environment_type {
  VIBESHINE_STREAM_TEXT,
  VIBESHINE_STREAM_UNSIGNED,
  VIBESHINE_STREAM_SIGNED,
  VIBESHINE_STREAM_DECIMAL,
  VIBESHINE_STREAM_BOOLEAN,
  VIBESHINE_STREAM_AUDIO_LAYOUT,
  VIBESHINE_STREAM_TOGGLE
};

struct vibeshine_stream_environment_field {
  const char *name;
  enum vibeshine_stream_environment_type type;
  size_t maximum_length;
};

// This is an exact allowlist, not a SUNSHINE_ prefix match. In
// particular PATH, HOME, LD_PRELOAD and manager/session settings stay under
// broker policy. ENABLE_HDR_WSI remains the fixed app-wayland-hdr operation.
static const struct vibeshine_stream_environment_field vibeshine_stream_environment_fields[] = {
  {"SUNSHINE_APP_ID", VIBESHINE_STREAM_UNSIGNED, 10},
  {"SUNSHINE_APP_NAME", VIBESHINE_STREAM_TEXT, 4096},
  {"SUNSHINE_CLIENT_WIDTH", VIBESHINE_STREAM_UNSIGNED, 10},
  {"SUNSHINE_CLIENT_HEIGHT", VIBESHINE_STREAM_UNSIGNED, 10},
  {"SUNSHINE_CLIENT_FPS", VIBESHINE_STREAM_DECIMAL, 17},
  {"SUNSHINE_CLIENT_HDR", VIBESHINE_STREAM_BOOLEAN, 5},
  {"SUNSHINE_CLIENT_GCMAP", VIBESHINE_STREAM_SIGNED, 11},
  {"SUNSHINE_CLIENT_HOST_AUDIO", VIBESHINE_STREAM_BOOLEAN, 5},
  {"SUNSHINE_CLIENT_ENABLE_SOPS", VIBESHINE_STREAM_BOOLEAN, 5},
  {"SUNSHINE_CLIENT_AUDIO_CONFIGURATION", VIBESHINE_STREAM_AUDIO_LAYOUT, 3},
  {"SUNSHINE_CLIENT_AUDIO_SURROUND_PARAMS", VIBESHINE_STREAM_TEXT, 128},
  {"PROTON_KEEP_SONY_AUDIO_ENDPOINT_VISIBLE", VIBESHINE_STREAM_TOGGLE, 1},
  {"PROTON_SONY_WINDOWS_DEVICE_NAMES", VIBESHINE_STREAM_TOGGLE, 1}
};

#define VIBESHINE_STREAM_ENVIRONMENT_FIELD_COUNT \
  (sizeof(vibeshine_stream_environment_fields) / sizeof(vibeshine_stream_environment_fields[0]))

static inline bool vibeshine_stream_unsigned_is_safe(const char *value, uint64_t maximum) {
  if (!value || !value[0]) return false;
  uint64_t parsed = 0;
  for (const unsigned char *cursor = (const unsigned char *) value; *cursor; ++cursor) {
    if (*cursor < '0' || *cursor > '9') return false;
    const unsigned int digit = *cursor - '0';
    if (parsed > maximum / 10 ||
        (parsed == maximum / 10 && digit > maximum % 10)) return false;
    parsed = parsed * 10 + digit;
  }
  return true;
}

static inline bool vibeshine_stream_decimal_is_safe(const char *value) {
  // process.cpp uses either %.3f or std::to_string(round(float)), whose
  // compatibility representation has six fractional digits (60.000000).
  uint64_t integer = 0;
  size_t integer_digits = 0, fractional_digits = 0;
  bool positive = false;
  const unsigned char *cursor = (const unsigned char *) value;
  while (*cursor >= '0' && *cursor <= '9') {
    integer = integer * 10 + *cursor++ - '0';
    if (integer > UINT32_MAX || ++integer_digits > 10) return false;
  }
  positive = integer > 0;
  if (*cursor == '.') {
    ++cursor;
    while (*cursor >= '0' && *cursor <= '9') {
      positive = positive || *cursor != '0';
      ++cursor;
      if (++fractional_digits > 6) return false;
    }
    if (!fractional_digits) return false;
  }
  return integer_digits && !*cursor && positive;
}

static inline bool vibeshine_stream_environment_value_is_safe(
    const struct vibeshine_stream_environment_field *field, const char *value) {
  if (!field || !value) return false;
  size_t length = 0;
  for (const unsigned char *cursor = (const unsigned char *) value; *cursor; ++cursor) {
    if (++length > field->maximum_length || *cursor < 0x20 || *cursor == 0x7f) return false;
  }
  switch (field->type) {
    case VIBESHINE_STREAM_TEXT:
      // Application names and surroundParams are exported as
      // text by process.cpp. Empty values and printable UTF-8 remain literal.
      return true;
    case VIBESHINE_STREAM_UNSIGNED:
      return vibeshine_stream_unsigned_is_safe(value, UINT32_MAX);
    case VIBESHINE_STREAM_SIGNED:
      return value[0] == '-'
        ? vibeshine_stream_unsigned_is_safe(value + 1, UINT64_C(2147483648))
        : vibeshine_stream_unsigned_is_safe(value, INT32_MAX);
    case VIBESHINE_STREAM_DECIMAL:
      return vibeshine_stream_decimal_is_safe(value);
    case VIBESHINE_STREAM_BOOLEAN:
      return !strcmp(value, "true") || !strcmp(value, "false");
    case VIBESHINE_STREAM_AUDIO_LAYOUT:
      return !strcmp(value, "2.0") || !strcmp(value, "5.1") || !strcmp(value, "7.1");
    case VIBESHINE_STREAM_TOGGLE:
      return !strcmp(value, "0") || !strcmp(value, "1");
  }
  return false;
}

static inline bool vibeshine_stream_environment_entry_is_safe(const char *entry, size_t *field_index) {
  if (!entry) return false;
  const char *separator = strchr(entry, '=');
  if (!separator) return false;
  const size_t name_length = (size_t) (separator - entry);
  for (size_t index = 0; index < VIBESHINE_STREAM_ENVIRONMENT_FIELD_COUNT; ++index) {
    const struct vibeshine_stream_environment_field *field = &vibeshine_stream_environment_fields[index];
    if (strlen(field->name) == name_length && !memcmp(entry, field->name, name_length)) {
      if (!vibeshine_stream_environment_value_is_safe(field, separator + 1)) return false;
      if (field_index) *field_index = index;
      return true;
    }
  }
  return false;
}

static inline bool vibeshine_stream_environment_is_safe(size_t count, char *const entries[]) {
  if (count > VIBESHINE_STREAM_ENVIRONMENT_MAX_ENTRIES || (count && !entries)) return false;
  bool seen[VIBESHINE_STREAM_ENVIRONMENT_FIELD_COUNT] = {false};
  size_t bytes = 0;
  for (size_t index = 0; index < count; ++index) {
    size_t field_index = 0;
    if (!vibeshine_stream_environment_entry_is_safe(entries[index], &field_index) ||
        seen[field_index]) return false;
    seen[field_index] = true;
    const size_t length = strlen(entries[index]) + 1;
    if (length > VIBESHINE_STREAM_ENVIRONMENT_MAX_BYTES - bytes) return false;
    bytes += length;
  }
  return true;
}

#endif
