#pragma once

/*
 * C representation of src/steam_offline_filter_ioctl.h.
 *
 * The user-mode header remains the ABI source of truth.  Keep these
 * definitions byte-for-byte equivalent: this header exists only because the
 * WDM driver is compiled as C and cannot include the C++ namespace header.
 */

#include <ntdef.h>

#define STEAM_OFFLINE_PROTOCOL_VERSION ((ULONG)1)
#define STEAM_OFFLINE_DEVICE_TYPE ((ULONG)0x00000012) /* FILE_DEVICE_NETWORK */
#define STEAM_OFFLINE_METHOD_BUFFERED ((ULONG)0)
#define STEAM_OFFLINE_FILE_WRITE_DATA ((ULONG)2)
#define STEAM_OFFLINE_CTL_CODE(function, access) \
    ((STEAM_OFFLINE_DEVICE_TYPE << 16) | ((access) << 14) | ((function) << 2) | \
     STEAM_OFFLINE_METHOD_BUFFERED)

#define STEAM_OFFLINE_REGISTER_ROOT_IOCTL \
    STEAM_OFFLINE_CTL_CODE(0x801, STEAM_OFFLINE_FILE_WRITE_DATA)
#define STEAM_OFFLINE_UNREGISTER_ROOT_IOCTL \
    STEAM_OFFLINE_CTL_CODE(0x802, STEAM_OFFLINE_FILE_WRITE_DATA)
#define STEAM_OFFLINE_STATUS_IOCTL \
    STEAM_OFFLINE_CTL_CODE(0x803, STEAM_OFFLINE_FILE_WRITE_DATA)
#define STEAM_OFFLINE_MAX_SEAT_ID_SIZE 64u

#pragma pack(push, 1)
typedef struct _STEAM_OFFLINE_REGISTER_ROOT {
    ULONG version;
    ULONG rootPid;
    ULONGLONG processCreationTime;
    ULONGLONG generation;
    CHAR seatId[STEAM_OFFLINE_MAX_SEAT_ID_SIZE];
} STEAM_OFFLINE_REGISTER_ROOT;

typedef struct _STEAM_OFFLINE_REGISTRATION {
    ULONG version;
    ULONG reserved;
    ULONGLONG registrationId;
    ULONGLONG readinessGeneration;
} STEAM_OFFLINE_REGISTRATION;

typedef struct _STEAM_OFFLINE_UNREGISTER_ROOT {
    ULONG version;
    ULONG reserved;
    ULONGLONG registrationId;
    ULONGLONG generation;
    CHAR seatId[STEAM_OFFLINE_MAX_SEAT_ID_SIZE];
} STEAM_OFFLINE_UNREGISTER_ROOT;

typedef struct _STEAM_OFFLINE_STATUS {
    ULONG version;
    ULONG reserved;
    ULONG bfeReady;
    ULONG wfpReady;
    ULONGLONG bfeGeneration;
} STEAM_OFFLINE_STATUS;
#pragma pack(pop)

/* ABI assertions are intentionally independent of the C++ header. */
C_ASSERT(FIELD_OFFSET(STEAM_OFFLINE_REGISTER_ROOT, processCreationTime) == 8);
C_ASSERT(FIELD_OFFSET(STEAM_OFFLINE_REGISTER_ROOT, generation) == 16);
C_ASSERT(FIELD_OFFSET(STEAM_OFFLINE_REGISTER_ROOT, seatId) == 24);
C_ASSERT(sizeof(STEAM_OFFLINE_REGISTER_ROOT) == 88);
C_ASSERT(sizeof(STEAM_OFFLINE_REGISTRATION) == 24);
C_ASSERT(FIELD_OFFSET(STEAM_OFFLINE_UNREGISTER_ROOT, generation) == 16);
C_ASSERT(FIELD_OFFSET(STEAM_OFFLINE_UNREGISTER_ROOT, seatId) == 24);
C_ASSERT(sizeof(STEAM_OFFLINE_UNREGISTER_ROOT) == 88);
C_ASSERT(sizeof(STEAM_OFFLINE_STATUS) == 24);
