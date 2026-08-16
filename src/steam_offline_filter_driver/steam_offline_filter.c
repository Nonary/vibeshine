#include "steam_offline_filter.h"

#include <initguid.h>
#include <ntifs.h>
#include <ntstrsafe.h>
#include <wdmsec.h>
#include <fwpmk.h>
#include <fwpsk.h>

#include "steam_offline_filter_ioctl_abi.h"

/* Private keys; no filter condition contains an endpoint, address, or host. */
DEFINE_GUID(
    GUID_STEAM_OFFLINE_CALLOUT_V4,
    0x4c018c90, 0xb0a7, 0x4a5b, 0x8a, 0x6d, 0x7e, 0x3f, 0x7c, 0x8a, 0x4d, 0x11);
DEFINE_GUID(
    GUID_STEAM_OFFLINE_CALLOUT_V6,
    0x4c018c91, 0xb0a7, 0x4a5b, 0x8a, 0x6d, 0x7e, 0x3f, 0x7c, 0x8a, 0x4d, 0x11);
DEFINE_GUID(
    GUID_STEAM_OFFLINE_DEVICE_CLASS,
    0x4c018c92, 0xb0a7, 0x4a5b, 0x8a, 0x6d, 0x7e, 0x3f, 0x7c, 0x8a, 0x4d, 0x11);
DEFINE_GUID(
    GUID_STEAM_OFFLINE_SUBLAYER,
    0x4c018c94, 0xb0a7, 0x4a5b, 0x8a, 0x6d, 0x7e, 0x3f, 0x7c, 0x8a, 0x4d, 0x11);

PDEVICE_OBJECT gSteamOfflineDevice;

typedef struct _STEAM_OFFLINE_PROCESS_ENTRY {
    BOOLEAN inUse;
    BOOLEAN isSteamClient;
    USHORT imageBytes;
    ULONG next;
    HANDLE pid;
    PEPROCESS process;
    ULONGLONG creationTime;
    ULONGLONG generation;
    ULONGLONG registrationId;
    WCHAR imagePath[STEAM_OFFLINE_MAX_IMAGE_PATH_CHARS];
} STEAM_OFFLINE_PROCESS_ENTRY;

/*
 * Image identity is captured at PASSIVE_LEVEL before gProcessLock is taken.
 * Neither process-notify ImageFileName nor SeLocateProcessImageName's
 * UNICODE_STRING backing storage is safe to dereference while the spin lock
 * raises IRQL.  Locked insertion therefore accepts only this fixed,
 * driver-owned snapshot.
 */
typedef struct _STEAM_OFFLINE_CANONICAL_IMAGE {
    USHORT imageBytes;
    BOOLEAN isSteamClient;
    WCHAR imagePath[STEAM_OFFLINE_MAX_IMAGE_PATH_CHARS];
} STEAM_OFFLINE_CANONICAL_IMAGE;

typedef struct _STEAM_OFFLINE_FILE_CONTEXT {
    ULONG magic;
    PEPROCESS ownerProcess;
    HANDLE ownerPid;
    ULONGLONG ownerCreationTime;
} STEAM_OFFLINE_FILE_CONTEXT;

#define STEAM_OFFLINE_FILE_CONTEXT_MAGIC ((ULONG)0x534f4643) /* SOFC */

#define STEAM_OFFLINE_MAX_REGISTRATIONS 16u

typedef struct _STEAM_OFFLINE_REGISTRATION_STATE {
    BOOLEAN inUse;
    ULONGLONG registrationId;
    ULONGLONG generation;
    CHAR seatId[STEAM_OFFLINE_MAX_SEAT_ID_SIZE];
    PFILE_OBJECT fileObject;
    PEPROCESS ownerProcess;
    HANDLE ownerPid;
    ULONGLONG ownerCreationTime;
} STEAM_OFFLINE_REGISTRATION_STATE;

static KSPIN_LOCK gProcessLock;
static STEAM_OFFLINE_PROCESS_ENTRY gProcesses[STEAM_OFFLINE_MAX_PROCESSES];
static ULONG gBuckets[STEAM_OFFLINE_BUCKET_COUNT];
static STEAM_OFFLINE_REGISTRATION_STATE gRegistrations[STEAM_OFFLINE_MAX_REGISTRATIONS];
static volatile LONG64 gNextRegistrationId;
static BOOLEAN gUnloading;
static BOOLEAN gProcessNotifyRegistered;
static volatile LONG64 gLineageInsertFailures;
static volatile LONG64 gLineageRejectedChildren;

static HANDLE gWfpEngine;
static UINT32 gWfpCalloutV4;
static UINT32 gWfpCalloutV6;
static UINT64 gWfpFilterV4;
static UINT64 gWfpFilterV6;
static HANDLE gBfeChangeHandle;
static volatile LONG gBfeReady;
static volatile LONG gWfpReady;
static volatile LONG64 gBfeGeneration;
static volatile LONG64 gWfpPublishedGeneration;
static volatile LONG gWfpStopping;
/* A successful BFE unsubscribe drains this callback.  Keep an explicit
 * ownership count so teardown never mistakes a diagnostic FWPM error for
 * proof that an image callback can no longer execute. */
static volatile LONG gBfeCallbackInFlight;
/* PASSIVE_LEVEL state lock for BFE callbacks and WFP object publication. */
static FAST_MUTEX gWfpStateLock;

static ULONG SteamOfflinePidHash(_In_ HANDLE pid)
{
    return ((ULONG)(ULONG_PTR)pid >> 2) & (STEAM_OFFLINE_BUCKET_COUNT - 1u);
}

static ULONG SteamOfflineFindFreeLocked(void)
{
    ULONG i;
    for (i = 0; i < STEAM_OFFLINE_MAX_PROCESSES; ++i) {
        if (!gProcesses[i].inUse) {
            return i;
        }
    }
    return STEAM_OFFLINE_MAX_PROCESSES;
}

static ULONG SteamOfflineFindPidLocked(_In_ HANDLE pid)
{
    ULONG index = gBuckets[SteamOfflinePidHash(pid)];
    while (index != STEAM_OFFLINE_MAX_PROCESSES) {
        if (gProcesses[index].inUse && gProcesses[index].pid == pid) {
            return index;
        }
        index = gProcesses[index].next;
    }
    return STEAM_OFFLINE_MAX_PROCESSES;
}

static ULONG SteamOfflineFindProcessLocked(_In_ PEPROCESS process)
{
    ULONG i;
    for (i = 0; i < STEAM_OFFLINE_MAX_PROCESSES; ++i) {
        if (gProcesses[i].inUse && gProcesses[i].process == process) {
            return i;
        }
    }
    return STEAM_OFFLINE_MAX_PROCESSES;
}

static ULONG SteamOfflineFindProcessIdentityLocked(_In_ PEPROCESS process, _In_ ULONGLONG creationTime)
{
    ULONG i;
    for (i = 0; i < STEAM_OFFLINE_MAX_PROCESSES; ++i) {
        if (gProcesses[i].inUse && gProcesses[i].process == process &&
            gProcesses[i].creationTime == creationTime) {
            return i;
        }
    }
    return STEAM_OFFLINE_MAX_PROCESSES;
}

static BOOLEAN SteamOfflineHasRegistrationsLocked(void)
{
    ULONG i;
    for (i = 0; i < STEAM_OFFLINE_MAX_REGISTRATIONS; ++i) {
        if (gRegistrations[i].inUse) {
            return TRUE;
        }
    }
    return FALSE;
}

static ULONG SteamOfflineFindRegistrationLocked(_In_ ULONGLONG registrationId)
{
    ULONG i;
    for (i = 0; i < STEAM_OFFLINE_MAX_REGISTRATIONS; ++i) {
        if (gRegistrations[i].inUse && gRegistrations[i].registrationId == registrationId) {
            return i;
        }
    }
    return STEAM_OFFLINE_MAX_REGISTRATIONS;
}

static ULONG SteamOfflineCountRegistrationProcessesLocked(_In_ ULONGLONG registrationId)
{
    ULONG i;
    ULONG count = 0;
    for (i = 0; i < STEAM_OFFLINE_MAX_PROCESSES; ++i) {
        if (gProcesses[i].inUse && gProcesses[i].registrationId == registrationId) {
            ++count;
        }
    }
    return count;
}

static ULONG SteamOfflineFindRegistrationOwnerLocked(_In_ PFILE_OBJECT fileObject)
{
    ULONG i;
    for (i = 0; i < STEAM_OFFLINE_MAX_REGISTRATIONS; ++i) {
        if (gRegistrations[i].inUse && gRegistrations[i].fileObject == fileObject) {
            return i;
        }
    }
    return STEAM_OFFLINE_MAX_REGISTRATIONS;
}

static ULONG SteamOfflineFindFreeRegistrationLocked(void)
{
    ULONG i;
    for (i = 0; i < STEAM_OFFLINE_MAX_REGISTRATIONS; ++i) {
        if (!gRegistrations[i].inUse) {
            return i;
        }
    }
    return STEAM_OFFLINE_MAX_REGISTRATIONS;
}

static BOOLEAN SteamOfflineRegistrationSeatEquals(
    _In_ const STEAM_OFFLINE_REGISTRATION_STATE* state,
    _In_ const CHAR* seatId)
{
    return RtlCompareMemory(state->seatId, seatId, STEAM_OFFLINE_MAX_SEAT_ID_SIZE) ==
        STEAM_OFFLINE_MAX_SEAT_ID_SIZE;
}

static BOOLEAN SteamOfflineIsSafeImagePath(_In_ const UNICODE_STRING* path)
{
    USHORT i;
    USHORT chars;

    if (path == NULL || path->Buffer == NULL || path->Length == 0 ||
        (path->Length & (sizeof(WCHAR) - 1u)) != 0 ||
        path->Length > (STEAM_OFFLINE_MAX_IMAGE_PATH_CHARS - 1u) * sizeof(WCHAR)) {
        return FALSE;
    }

    /* Process-notification paths are kernel supplied, but reject malformed
     * or relative values before storing them as identity metadata. */
    if (path->Buffer[0] != L'\\') {
        return FALSE;
    }
    chars = path->Length / sizeof(WCHAR);
    for (i = 0; i < chars; ++i) {
        if (path->Buffer[i] < 0x20 || path->Buffer[i] == L'\"') {
            return FALSE;
        }
        if (path->Buffer[i] == L'.' && i + 1u < chars && path->Buffer[i + 1u] == L'.') {
            return FALSE;
        }
    }
    return TRUE;
}

_IRQL_requires_max_(DISPATCH_LEVEL)
static WCHAR SteamOfflineFoldAscii(_In_ WCHAR value)
{
    return value >= L'A' && value <= L'Z' ? (WCHAR)(value + (L'a' - L'A')) : value;
}

_IRQL_requires_max_(DISPATCH_LEVEL)
static BOOLEAN SteamOfflineUnicodeCodeUnitEqualsInsensitive(_In_ WCHAR left, _In_ WCHAR right)
{
    if (left == right) {
        return TRUE;
    }
    /* WFP image paths are UTF-16.  Fold only ASCII here; non-ASCII code units
     * must match exactly rather than invoking a pageable Unicode routine at
     * DISPATCH_LEVEL. */
    return left <= 0x7f && right <= 0x7f &&
        SteamOfflineFoldAscii(left) == SteamOfflineFoldAscii(right);
}

_IRQL_requires_max_(DISPATCH_LEVEL)
static BOOLEAN SteamOfflineUnicodeEqualsAsciiInsensitive(
    _In_reads_(unicodeChars) const WCHAR* unicode,
    _In_ USHORT unicodeChars,
    _In_z_ const CHAR* ascii)
{
    USHORT i;
    if (unicode == NULL || ascii == NULL) {
        return FALSE;
    }
    for (i = 0; i < unicodeChars; ++i) {
        const UCHAR expected = (UCHAR)ascii[i];
        if (expected == '\0' || unicode[i] > 0x7f ||
            !SteamOfflineUnicodeCodeUnitEqualsInsensitive(unicode[i], (WCHAR)expected)) {
            return FALSE;
        }
    }
    return ascii[unicodeChars] == '\0';
}

_IRQL_requires_max_(DISPATCH_LEVEL)
static BOOLEAN SteamOfflineIsSteamClientPath(_In_ const UNICODE_STRING* path)
{
    USHORT i;
    USHORT start = 0;
    USHORT chars;
    static const CHAR* const names[] = {
        "steam.exe",
        "steamwebhelper.exe",
        "GameOverlayUI.exe",
        "steamerrorreporter.exe",
        "steamerrorreporter64.exe",
    };
    ULONG nameIndex;

    if (!SteamOfflineIsSafeImagePath(path)) {
        return FALSE;
    }
    chars = path->Length / sizeof(WCHAR);
    for (i = 0; i < chars; ++i) {
        if (path->Buffer[i] == L'\\' || path->Buffer[i] == L'/') {
            start = i + 1u;
        }
    }
    for (nameIndex = 0; nameIndex < RTL_NUMBER_OF(names); ++nameIndex) {
        if (SteamOfflineUnicodeEqualsAsciiInsensitive(path->Buffer + start,
                                                       (USHORT)(chars - start),
                                                       names[nameIndex])) {
            return TRUE;
        }
    }
    return FALSE;
}

static BOOLEAN SteamOfflineIsValidSeatId(_In_reads_(STEAM_OFFLINE_MAX_SEAT_ID_SIZE) const CHAR* seatId)
{
    ULONG i;
    BOOLEAN terminated = FALSE;
    if (seatId == NULL || seatId[0] == '\0') {
        return FALSE;
    }
    for (i = 0; i < STEAM_OFFLINE_MAX_SEAT_ID_SIZE; ++i) {
        UCHAR c = (UCHAR)seatId[i];
        if (terminated) {
            if (c != '\0') {
                return FALSE;
            }
            continue;
        }
        if (c == '\0') {
            terminated = TRUE;
            continue;
        }
        if (c < 0x21 || c > 0x7e || c == '\\' || c == '"') {
            return FALSE;
        }
    }
    return terminated;
}

_IRQL_requires_(PASSIVE_LEVEL)
static NTSTATUS SteamOfflineCanonicalizeImagePath(
    _Out_ STEAM_OFFLINE_CANONICAL_IMAGE* destination,
    _In_ const UNICODE_STRING* source)
{
    if (destination == NULL || !SteamOfflineIsSafeImagePath(source)) {
        return STATUS_INVALID_IMAGE_FORMAT;
    }
    RtlZeroMemory(destination, sizeof(*destination));
    RtlCopyMemory(destination->imagePath, source->Buffer, source->Length);
    destination->imagePath[source->Length / sizeof(WCHAR)] = L'\0';
    destination->imageBytes = source->Length;
    destination->isSteamClient = SteamOfflineIsSteamClientPath(source);
    return STATUS_SUCCESS;
}

_IRQL_requires_max_(DISPATCH_LEVEL)
static BOOLEAN SteamOfflinePathEqualsBlob(
    _In_ const STEAM_OFFLINE_PROCESS_ENTRY* entry,
    _In_ const FWP_BYTE_BLOB* processPath)
{
    const UCHAR* actual;
    USHORT offset;

    if (processPath == NULL || processPath->data == NULL ||
        entry == NULL || processPath->size != entry->imageBytes ||
        (processPath->size & (sizeof(WCHAR) - 1u)) != 0 ||
        processPath->size > (STEAM_OFFLINE_MAX_IMAGE_PATH_CHARS - 1u) * sizeof(WCHAR)) {
        return FALSE;
    }
    actual = (const UCHAR*)processPath->data;
    for (offset = 0; offset < entry->imageBytes; offset += sizeof(WCHAR)) {
        const WCHAR actualCodeUnit = (WCHAR)(actual[offset] | ((USHORT)actual[offset + 1u] << 8));
        if (!SteamOfflineUnicodeCodeUnitEqualsInsensitive(actualCodeUnit,
                                                           entry->imagePath[offset / sizeof(WCHAR)])) {
            return FALSE;
        }
    }
    return TRUE;
}

static NTSTATUS SteamOfflineInsertProcessLocked(
    _In_ PEPROCESS process,
    _In_ HANDLE pid,
    _In_ ULONGLONG creationTime,
    _In_ ULONGLONG generation,
    _In_ ULONGLONG registrationId,
    _In_ const STEAM_OFFLINE_CANONICAL_IMAGE* image,
    _Out_opt_ ULONG* insertedIndex)
{
    ULONG index;
    ULONG bucket;

    if (image == NULL || image->imageBytes == 0 ||
        image->imageBytes > (STEAM_OFFLINE_MAX_IMAGE_PATH_CHARS - 1u) * sizeof(WCHAR) ||
        (image->imageBytes & (sizeof(WCHAR) - 1u)) != 0) {
        return STATUS_INVALID_IMAGE_FORMAT;
    }

    if (SteamOfflineFindPidLocked(pid) != STEAM_OFFLINE_MAX_PROCESSES) {
        return STATUS_OBJECT_NAME_COLLISION;
    }
    if (SteamOfflineCountRegistrationProcessesLocked(registrationId) >=
        STEAM_OFFLINE_MAX_PROCESSES_PER_REGISTRATION) {
        return STATUS_QUOTA_EXCEEDED;
    }
    index = SteamOfflineFindFreeLocked();
    if (index == STEAM_OFFLINE_MAX_PROCESSES) {
        return STATUS_QUOTA_EXCEEDED;
    }
    RtlCopyMemory(gProcesses[index].imagePath, image->imagePath, sizeof(image->imagePath));
    gProcesses[index].imageBytes = image->imageBytes;

    ObReferenceObject(process);
    gProcesses[index].process = process;
    gProcesses[index].pid = pid;
    gProcesses[index].creationTime = creationTime;
    gProcesses[index].generation = generation;
    gProcesses[index].registrationId = registrationId;
    gProcesses[index].isSteamClient = image->isSteamClient;
    gProcesses[index].inUse = TRUE;
    bucket = SteamOfflinePidHash(pid);
    gProcesses[index].next = gBuckets[bucket];
    gBuckets[bucket] = index;
    if (insertedIndex != NULL) {
        *insertedIndex = index;
    }
    return STATUS_SUCCESS;
}

static PEPROCESS SteamOfflineRemoveIndexLocked(_In_ ULONG index)
{
    ULONG bucket;
    ULONG current;
    ULONG previous = STEAM_OFFLINE_MAX_PROCESSES;
    PEPROCESS process;

    if (index >= STEAM_OFFLINE_MAX_PROCESSES || !gProcesses[index].inUse) {
        return NULL;
    }
    bucket = SteamOfflinePidHash(gProcesses[index].pid);
    current = gBuckets[bucket];
    while (current != STEAM_OFFLINE_MAX_PROCESSES && current != index) {
        previous = current;
        current = gProcesses[current].next;
    }
    if (current == index) {
        if (previous == STEAM_OFFLINE_MAX_PROCESSES) {
            gBuckets[bucket] = gProcesses[index].next;
        } else {
            gProcesses[previous].next = gProcesses[index].next;
        }
    }
    process = gProcesses[index].process;
    RtlZeroMemory(&gProcesses[index], sizeof(gProcesses[index]));
    gProcesses[index].next = STEAM_OFFLINE_MAX_PROCESSES;
    return process;
}

static PEPROCESS SteamOfflineRemoveRegistrationEntry(_In_ ULONGLONG registrationId)
{
    KIRQL oldIrql;
    ULONG i;
    PEPROCESS process = NULL;

    KeAcquireSpinLock(&gProcessLock, &oldIrql);
    for (i = 0; i < STEAM_OFFLINE_MAX_PROCESSES; ++i) {
        if (gProcesses[i].inUse && gProcesses[i].registrationId == registrationId) {
            process = SteamOfflineRemoveIndexLocked(i);
            break;
        }
    }
    KeReleaseSpinLock(&gProcessLock, oldIrql);
    return process;
}

static void SteamOfflineRemoveRegistration(_In_ ULONGLONG registrationId)
{
    PEPROCESS process;
    do {
        process = SteamOfflineRemoveRegistrationEntry(registrationId);
        if (process != NULL) {
            ObDereferenceObject(process);
        }
    } while (process != NULL);
}

static void SteamOfflineReleaseRegistrationState(
    _In_opt_ PFILE_OBJECT fileObject,
    _In_opt_ PEPROCESS ownerProcess)
{
    if (fileObject != NULL) {
        ObDereferenceObject(fileObject);
    }
    if (ownerProcess != NULL) {
        ObDereferenceObject(ownerProcess);
    }
}

static NTSTATUS SteamOfflineDetachRegistrationLocked(
    _In_ ULONG registrationIndex,
    _Out_opt_ PFILE_OBJECT* fileObject,
    _Out_opt_ PEPROCESS* ownerProcess)
{
    if (registrationIndex >= STEAM_OFFLINE_MAX_REGISTRATIONS ||
        !gRegistrations[registrationIndex].inUse) {
        return STATUS_NOT_FOUND;
    }
    if (fileObject != NULL) {
        *fileObject = gRegistrations[registrationIndex].fileObject;
    }
    if (ownerProcess != NULL) {
        *ownerProcess = gRegistrations[registrationIndex].ownerProcess;
    }
    RtlZeroMemory(&gRegistrations[registrationIndex], sizeof(gRegistrations[registrationIndex]));
    return STATUS_SUCCESS;
}

static void SteamOfflineProcessNotify(
    _In_ PEPROCESS process,
    _In_ HANDLE processId,
    _In_opt_ PPS_CREATE_NOTIFY_INFO createInfo)
{
    KIRQL oldIrql;
    PEPROCESS creatorProcess = NULL;
    UNICODE_STRING* locatedImage = NULL;
    const UNICODE_STRING* imagePath = NULL;
    STEAM_OFFLINE_CANONICAL_IMAGE canonicalImage;
    ULONGLONG registrationId;
    ULONGLONG registrationGeneration;
    ULONGLONG creatorCreationTime;
    ULONG creatorIndex;
    NTSTATUS status;
    NTSTATUS imageStatus;

    if (createInfo == NULL) {
        KeAcquireSpinLock(&gProcessLock, &oldIrql);
        {
            ULONG index = SteamOfflineFindProcessLocked(process);
            if (index != STEAM_OFFLINE_MAX_PROCESSES) {
                PEPROCESS removed = SteamOfflineRemoveIndexLocked(index);
                KeReleaseSpinLock(&gProcessLock, oldIrql);
                if (removed != NULL) {
                    ObDereferenceObject(removed);
                }
                return;
            }
        }
        KeReleaseSpinLock(&gProcessLock, oldIrql);
        return;
    }

    if (gUnloading) {
        return;
    }
    /* CreatingThreadId.UniqueProcess is the process that actually issued the
     * create request. ParentProcessId can be caller-selected through
     * PROC_THREAD_ATTRIBUTE_PARENT_PROCESS and is not lineage authority. */
    if (createInfo->CreatingThreadId.UniqueProcess == NULL ||
        !NT_SUCCESS(PsLookupProcessByProcessId(
            createInfo->CreatingThreadId.UniqueProcess, &creatorProcess))) {
        return;
    }
    creatorCreationTime = (ULONGLONG)PsGetProcessCreateTimeQuadPart(creatorProcess);
    KeAcquireSpinLock(&gProcessLock, &oldIrql);
    if (gUnloading || !SteamOfflineHasRegistrationsLocked()) {
        KeReleaseSpinLock(&gProcessLock, oldIrql);
        ObDereferenceObject(creatorProcess);
        return;
    }
    creatorIndex = SteamOfflineFindProcessIdentityLocked(creatorProcess, creatorCreationTime);
    if (creatorIndex != STEAM_OFFLINE_MAX_PROCESSES &&
        SteamOfflineFindRegistrationLocked(gProcesses[creatorIndex].registrationId) == STEAM_OFFLINE_MAX_REGISTRATIONS) {
        creatorIndex = STEAM_OFFLINE_MAX_PROCESSES;
    }
    if (creatorIndex == STEAM_OFFLINE_MAX_PROCESSES) {
        KeReleaseSpinLock(&gProcessLock, oldIrql);
        ObDereferenceObject(creatorProcess);
        return;
    }
    registrationId = gProcesses[creatorIndex].registrationId;
    registrationGeneration = gProcesses[creatorIndex].generation;
    KeReleaseSpinLock(&gProcessLock, oldIrql);

    /* FileOpenNameAvailable is not required for identity, but a missing or
     * malformed image cannot safely remain in a registered lineage. */
    /* ImageFileName is only an exact identity when the notify contract says
     * the name was opened.  A partial name must never enter the lineage
     * table; obtain the canonical process image name instead. */
    imagePath = createInfo->FileOpenNameAvailable ? createInfo->ImageFileName : NULL;
    if (imagePath == NULL || !SteamOfflineIsSafeImagePath(imagePath)) {
        if (NT_SUCCESS(SeLocateProcessImageName(process, &locatedImage))) {
            imagePath = locatedImage;
        }
    }
    imageStatus = imagePath == NULL
        ? STATUS_INVALID_IMAGE_FORMAT
        : SteamOfflineCanonicalizeImagePath(&canonicalImage, imagePath);
    if (locatedImage != NULL) {
        /* The fixed snapshot is complete; no image-name pool is retained
         * across the lock acquisition below. */
        ExFreePool(locatedImage);
        locatedImage = NULL;
    }
    if (!NT_SUCCESS(imageStatus)) {
        BOOLEAN rejected = FALSE;
        /* Image lookup can block while registration teardown proceeds.  Do
         * not write CreationStatus based on the earlier snapshot: revalidate
         * the exact creator identity and registration while holding the same
         * lock used by detach before rejecting the child. */
        KeAcquireSpinLock(&gProcessLock, &oldIrql);
        creatorIndex = (!gUnloading && SteamOfflineHasRegistrationsLocked())
            ? SteamOfflineFindProcessIdentityLocked(creatorProcess, creatorCreationTime)
            : STEAM_OFFLINE_MAX_PROCESSES;
        if (creatorIndex != STEAM_OFFLINE_MAX_PROCESSES &&
            SteamOfflineFindRegistrationLocked(gProcesses[creatorIndex].registrationId) !=
                STEAM_OFFLINE_MAX_REGISTRATIONS) {
            InterlockedIncrement64(&gLineageRejectedChildren);
            createInfo->CreationStatus = STATUS_INVALID_IMAGE_FORMAT;
            rejected = TRUE;
        }
        KeReleaseSpinLock(&gProcessLock, oldIrql);
        if (rejected) {
            DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
                "VibeshineSteamOfflineFilter: rejected child %p for invalid image path\n", processId);
        }
        ObDereferenceObject(creatorProcess);
        return;
    }

    KeAcquireSpinLock(&gProcessLock, &oldIrql);
    creatorIndex = (!gUnloading && SteamOfflineHasRegistrationsLocked())
        ? SteamOfflineFindProcessIdentityLocked(creatorProcess, creatorCreationTime)
        : STEAM_OFFLINE_MAX_PROCESSES;
    if (creatorIndex != STEAM_OFFLINE_MAX_PROCESSES &&
        SteamOfflineFindRegistrationLocked(gProcesses[creatorIndex].registrationId) == STEAM_OFFLINE_MAX_REGISTRATIONS) {
        creatorIndex = STEAM_OFFLINE_MAX_PROCESSES;
    }
    if (creatorIndex == STEAM_OFFLINE_MAX_PROCESSES) {
        KeReleaseSpinLock(&gProcessLock, oldIrql);
        ObDereferenceObject(creatorProcess);
        return;
    }
    registrationId = gProcesses[creatorIndex].registrationId;
    registrationGeneration = gProcesses[creatorIndex].generation;
    status = SteamOfflineInsertProcessLocked(
        process,
        processId,
        (ULONGLONG)PsGetProcessCreateTimeQuadPart(process),
        registrationGeneration,
        registrationId,
        &canonicalImage,
        NULL);
    if (!NT_SUCCESS(status)) {
        InterlockedIncrement64(&gLineageInsertFailures);
        InterlockedIncrement64(&gLineageRejectedChildren);
        createInfo->CreationStatus = status;
        DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
            "VibeshineSteamOfflineFilter: rejected child %p status 0x%08X\n", processId, status);
    }
    KeReleaseSpinLock(&gProcessLock, oldIrql);

    ObDereferenceObject(creatorProcess);
}

static BOOLEAN SteamOfflineCallerIsSystem(void)
{
    PACCESS_TOKEN token;
    PTOKEN_USER tokenUser = NULL;
    NTSTATUS status;
    BOOLEAN isSystem = FALSE;

    token = PsReferencePrimaryToken(PsGetCurrentProcess());
    status = SeQueryInformationToken(token, TokenUser, (PVOID*)&tokenUser);
    if (NT_SUCCESS(status) && tokenUser != NULL && SeExports != NULL &&
        SeExports->SeLocalSystemSid != NULL) {
        isSystem = RtlEqualSid(tokenUser->User.Sid, SeExports->SeLocalSystemSid);
    }
    if (tokenUser != NULL) {
        ExFreePool(tokenUser);
    }
    PsDereferencePrimaryToken(token);
    return isSystem;
}

static NTSTATUS SteamOfflineRegisterRoot(
    _In_ const STEAM_OFFLINE_REGISTER_ROOT* request,
    _Out_ STEAM_OFFLINE_REGISTRATION* response,
    _In_ PFILE_OBJECT fileObject,
    _In_ PEPROCESS ownerProcess,
    _In_ HANDLE ownerPid,
    _In_ ULONGLONG ownerCreationTime)
{
    PEPROCESS process = NULL;
    UNICODE_STRING* imagePath = NULL;
    STEAM_OFFLINE_CANONICAL_IMAGE canonicalImage;
    KIRQL oldIrql;
    NTSTATUS status;
    ULONGLONG actualGeneration;
    ULONGLONG registrationId;
    ULONG registrationIndex;
    ULONG i;
    BOOLEAN duplicateIdentity = FALSE;
    ULONGLONG readinessGeneration;

    if (request->version != STEAM_OFFLINE_PROTOCOL_VERSION) {
        return STATUS_REVISION_MISMATCH;
    }
    if (request->rootPid == 0 || request->processCreationTime == 0 || request->generation == 0 ||
        !SteamOfflineIsValidSeatId(request->seatId)) {
        return STATUS_INVALID_PARAMETER;
    }
    status = PsLookupProcessByProcessId(ULongToHandle(request->rootPid), &process);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    actualGeneration = (ULONGLONG)PsGetProcessCreateTimeQuadPart(process);
    if (actualGeneration != request->processCreationTime) {
        ObDereferenceObject(process);
        return STATUS_PROCESS_IS_TERMINATING;
    }
    status = SeLocateProcessImageName(process, &imagePath);
    if (!NT_SUCCESS(status) || imagePath == NULL) {
        if (imagePath != NULL) {
            ExFreePool(imagePath);
        }
        ObDereferenceObject(process);
        return STATUS_INVALID_IMAGE_FORMAT;
    }
    status = SteamOfflineCanonicalizeImagePath(&canonicalImage, imagePath);
    ExFreePool(imagePath);
    imagePath = NULL;
    if (!NT_SUCCESS(status)) {
        ObDereferenceObject(process);
        return status;
    }

    /* Hold publication state across the readiness check and registration
     * commit. A BFE callback cannot advance the generation between the
     * response and the worker's immediate pre-resume status proof. */
    ExAcquireFastMutex(&gWfpStateLock);
    readinessGeneration = (ULONGLONG)InterlockedCompareExchange64(&gBfeGeneration, 0, 0);
    if (InterlockedCompareExchange(&gWfpStopping, 0, 0) != 0 ||
        InterlockedCompareExchange(&gBfeReady, 0, 0) == 0 ||
        InterlockedCompareExchange(&gWfpReady, 0, 0) == 0 ||
        (ULONGLONG)InterlockedCompareExchange64(&gWfpPublishedGeneration, 0, 0) != readinessGeneration) {
        ExReleaseFastMutex(&gWfpStateLock);
        ObDereferenceObject(process);
        return STATUS_DEVICE_NOT_READY;
    }
    KeAcquireSpinLock(&gProcessLock, &oldIrql);
    registrationIndex = SteamOfflineFindFreeRegistrationLocked();
    if (gUnloading || InterlockedCompareExchange(&gBfeReady, 0, 0) == 0 ||
        InterlockedCompareExchange(&gWfpReady, 0, 0) == 0 ||
        registrationIndex == STEAM_OFFLINE_MAX_REGISTRATIONS) {
        status = STATUS_QUOTA_EXCEEDED;
    } else if (SteamOfflineFindRegistrationOwnerLocked(fileObject) != STEAM_OFFLINE_MAX_REGISTRATIONS) {
        status = STATUS_OBJECT_NAME_COLLISION;
    } else {
        for (i = 0; i < STEAM_OFFLINE_MAX_REGISTRATIONS; ++i) {
            if (gRegistrations[i].inUse && gRegistrations[i].generation == request->generation &&
                SteamOfflineRegistrationSeatEquals(&gRegistrations[i], request->seatId)) {
                duplicateIdentity = TRUE;
                break;
            }
        }
        if (duplicateIdentity) {
            status = STATUS_OBJECT_NAME_COLLISION;
        } else {
            registrationId = (ULONGLONG)InterlockedIncrement64(&gNextRegistrationId);
            status = SteamOfflineInsertProcessLocked(
                process,
                ULongToHandle(request->rootPid),
                actualGeneration,
                request->generation,
                registrationId,
                &canonicalImage,
                NULL);
            if (NT_SUCCESS(status)) {
                ObReferenceObject(fileObject);
                ObReferenceObject(ownerProcess);
                gRegistrations[registrationIndex].inUse = TRUE;
                gRegistrations[registrationIndex].registrationId = registrationId;
                gRegistrations[registrationIndex].generation = request->generation;
                RtlCopyMemory(gRegistrations[registrationIndex].seatId, request->seatId,
                    STEAM_OFFLINE_MAX_SEAT_ID_SIZE);
                gRegistrations[registrationIndex].fileObject = fileObject;
                gRegistrations[registrationIndex].ownerProcess = ownerProcess;
                gRegistrations[registrationIndex].ownerPid = ownerPid;
                gRegistrations[registrationIndex].ownerCreationTime = ownerCreationTime;
                response->version = STEAM_OFFLINE_PROTOCOL_VERSION;
                response->reserved = 0;
                response->registrationId = registrationId;
                response->readinessGeneration = readinessGeneration;
            }
        }
    }
    KeReleaseSpinLock(&gProcessLock, oldIrql);
    ExReleaseFastMutex(&gWfpStateLock);

    ObDereferenceObject(process);
    return status;
}

static NTSTATUS SteamOfflineUnregisterRoot(
    _In_ const STEAM_OFFLINE_UNREGISTER_ROOT* request,
    _In_ PFILE_OBJECT fileObject,
    _In_ PEPROCESS ownerProcess,
    _In_ HANDLE ownerPid,
    _In_ ULONGLONG ownerCreationTime)
{
    KIRQL oldIrql;
    ULONG registrationIndex;
    PFILE_OBJECT registeredFile = NULL;
    PEPROCESS registeredOwner = NULL;
    NTSTATUS status;

    if (request->version != STEAM_OFFLINE_PROTOCOL_VERSION || request->reserved != 0 ||
        request->registrationId == 0 || request->generation == 0 ||
        !SteamOfflineIsValidSeatId(request->seatId)) {
        return STATUS_INVALID_PARAMETER;
    }
    KeAcquireSpinLock(&gProcessLock, &oldIrql);
    registrationIndex = SteamOfflineFindRegistrationLocked(request->registrationId);
    if (registrationIndex == STEAM_OFFLINE_MAX_REGISTRATIONS) {
        status = STATUS_NOT_FOUND;
    } else if (gRegistrations[registrationIndex].fileObject != fileObject ||
               gRegistrations[registrationIndex].ownerProcess != ownerProcess ||
               gRegistrations[registrationIndex].ownerPid != ownerPid ||
               gRegistrations[registrationIndex].ownerCreationTime != ownerCreationTime ||
               gRegistrations[registrationIndex].generation != request->generation ||
               !SteamOfflineRegistrationSeatEquals(&gRegistrations[registrationIndex], request->seatId)) {
        status = STATUS_ACCESS_DENIED;
    } else {
        status = SteamOfflineDetachRegistrationLocked(registrationIndex, &registeredFile, &registeredOwner);
    }
    KeReleaseSpinLock(&gProcessLock, oldIrql);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    SteamOfflineRemoveRegistration(request->registrationId);
    SteamOfflineReleaseRegistrationState(registeredFile, registeredOwner);
    return STATUS_SUCCESS;
}

static void SteamOfflineCleanupRegistration(_In_ PFILE_OBJECT fileObject)
{
    for (;;) {
        KIRQL oldIrql;
        ULONG registrationIndex;
        ULONGLONG registrationId = 0;
        PFILE_OBJECT registeredFile = NULL;
        PEPROCESS registeredOwner = NULL;
        NTSTATUS status;

        KeAcquireSpinLock(&gProcessLock, &oldIrql);
        registrationIndex = SteamOfflineFindRegistrationOwnerLocked(fileObject);
        if (registrationIndex != STEAM_OFFLINE_MAX_REGISTRATIONS) {
            registrationId = gRegistrations[registrationIndex].registrationId;
            status = SteamOfflineDetachRegistrationLocked(registrationIndex, &registeredFile, &registeredOwner);
        } else {
            status = STATUS_NOT_FOUND;
        }
        KeReleaseSpinLock(&gProcessLock, oldIrql);
        if (!NT_SUCCESS(status)) {
            return;
        }
        SteamOfflineRemoveRegistration(registrationId);
        SteamOfflineReleaseRegistrationState(registeredFile, registeredOwner);
    }
}

static void SteamOfflineCleanupAllRegistrations(void)
{
    for (;;) {
        KIRQL oldIrql;
        ULONG registrationIndex;
        ULONGLONG registrationId = 0;
        PFILE_OBJECT registeredFile = NULL;
        PEPROCESS registeredOwner = NULL;
        NTSTATUS status;
        ULONG i;

        KeAcquireSpinLock(&gProcessLock, &oldIrql);
        registrationIndex = STEAM_OFFLINE_MAX_REGISTRATIONS;
        for (i = 0; i < STEAM_OFFLINE_MAX_REGISTRATIONS; ++i) {
            if (gRegistrations[i].inUse) {
                registrationIndex = i;
                break;
            }
        }
        if (registrationIndex < STEAM_OFFLINE_MAX_REGISTRATIONS) {
            registrationId = gRegistrations[registrationIndex].registrationId;
            status = SteamOfflineDetachRegistrationLocked(registrationIndex, &registeredFile, &registeredOwner);
        } else {
            status = STATUS_NOT_FOUND;
        }
        KeReleaseSpinLock(&gProcessLock, oldIrql);
        if (!NT_SUCCESS(status)) {
            return;
        }
        SteamOfflineRemoveRegistration(registrationId);
        SteamOfflineReleaseRegistrationState(registeredFile, registeredOwner);
    }
}

_IRQL_requires_max_(DISPATCH_LEVEL)
static void NTAPI SteamOfflineClassify(
    _In_ const FWPS_INCOMING_VALUES0* inFixedValues,
    _In_ const FWPS_INCOMING_METADATA_VALUES0* inMetaValues,
    _Inout_opt_ void* layerData,
    _In_opt_ const void* classifyContext,
    _In_ const FWPS_FILTER* filter,
    _In_ UINT64 flowContext,
    _Inout_ FWPS_CLASSIFY_OUT0* classifyOut)
{
    KIRQL oldIrql;
    ULONG index;
    BOOLEAN block = FALSE;
    BOOLEAN canWrite;

    UNREFERENCED_PARAMETER(inFixedValues);
    UNREFERENCED_PARAMETER(layerData);
    UNREFERENCED_PARAMETER(classifyContext);
    UNREFERENCED_PARAMETER(filter);
    UNREFERENCED_PARAMETER(flowContext);

    if (classifyOut == NULL) {
        return;
    }
    canWrite = (classifyOut->rights & FWPS_RIGHT_ACTION_WRITE) != 0;
    if (canWrite) {
        classifyOut->actionType = FWP_ACTION_CONTINUE;
    }

    if (inMetaValues == NULL ||
        !FWPS_IS_METADATA_FIELD_PRESENT(inMetaValues, FWPS_METADATA_FIELD_PROCESS_ID) ||
        !FWPS_IS_METADATA_FIELD_PRESENT(inMetaValues, FWPS_METADATA_FIELD_PROCESS_PATH) ||
        inMetaValues->processPath == NULL) {
        return;
    }

    if (inMetaValues->processId > MAXULONG) {
        return;
    }
    KeAcquireSpinLock(&gProcessLock, &oldIrql);
    index = SteamOfflineFindPidLocked(ULongToHandle((ULONG)inMetaValues->processId));
    if (index != STEAM_OFFLINE_MAX_PROCESSES && gProcesses[index].isSteamClient &&
        SteamOfflinePathEqualsBlob(&gProcesses[index], inMetaValues->processPath)) {
        block = TRUE;
    }
    KeReleaseSpinLock(&gProcessLock, oldIrql);

    if (block) {
        if (canWrite) {
            classifyOut->actionType = FWP_ACTION_BLOCK;
            classifyOut->rights &= ~FWPS_RIGHT_ACTION_WRITE;
        } else if (classifyOut->actionType == FWP_ACTION_PERMIT) {
            /* A callout without write rights may still veto an existing
             * permit.  Preserve an existing BLOCK and do not claim rights
             * that WFP did not grant us. */
            classifyOut->actionType = FWP_ACTION_BLOCK;
        }
    }
}

static NTSTATUS NTAPI SteamOfflineNotify(
    _In_ FWPS_CALLOUT_NOTIFY_TYPE notifyType,
    _In_ const GUID* filterKey,
    _Inout_ FWPS_FILTER* filter)
{
    UNREFERENCED_PARAMETER(notifyType);
    UNREFERENCED_PARAMETER(filterKey);
    UNREFERENCED_PARAMETER(filter);
    return STATUS_SUCCESS;
}

static void CALLBACK SteamOfflineBfeStateChanged(
    _Inout_ void* context,
    _In_ FWPM_SERVICE_STATE newState)
{
    UNREFERENCED_PARAMETER(context);
    InterlockedIncrement(&gBfeCallbackInFlight);
    /* The callback and every FWPM create/publication operation serialize on
     * the same PASSIVE_LEVEL lock.  A STOP_PENDING/STOPPED notification
     * therefore clears readiness and advances the generation before any new
     * registration can pass admission.  RUNNING deliberately remains
     * WFP-not-ready: the dynamic session/callouts must be rebuilt and
     * republished under this lock before a root can be admitted. */
    InterlockedExchange(&gWfpStopping, 1);
    InterlockedExchange(&gBfeReady, 0);
    InterlockedExchange(&gWfpReady, 0);
    InterlockedIncrement64(&gBfeGeneration);
    ExAcquireFastMutex(&gWfpStateLock);
    /* The callback may have waited behind a publication already in progress;
     * reassert poison after acquiring the lock so that publication cannot
     * overwrite the transition. */
    InterlockedExchange64(&gWfpPublishedGeneration, 0);
    InterlockedExchange(&gBfeReady, 0);
    InterlockedExchange(&gWfpReady, 0);
    InterlockedExchange(&gWfpStopping, 1);
    UNREFERENCED_PARAMETER(newState);
    ExReleaseFastMutex(&gWfpStateLock);
    InterlockedDecrement(&gBfeCallbackInFlight);
}

_IRQL_requires_(PASSIVE_LEVEL)
static NTSTATUS SteamOfflineStopWfp(_Out_opt_ BOOLEAN* ownershipRemaining);

_IRQL_requires_(PASSIVE_LEVEL)
static NTSTATUS SteamOfflineStopWfpLocked(_Out_opt_ HANDLE* detachedBfeHandle);

static BOOLEAN SteamOfflineCallbacksRemain(void);

static NTSTATUS SteamOfflineAddWfpFilter(_In_ const GUID* calloutKey, _In_ const GUID* layerKey, _Out_ UINT64* filterId)
{
    FWPM_FILTER0 filter;
    RtlZeroMemory(&filter, sizeof(filter));
    filter.displayData.name = L"Vibeshine Steam Offline Filter";
    filter.displayData.description = L"Blocks registered Steam client images only";
    filter.flags = FWPM_FILTER_FLAG_PERMIT_IF_CALLOUT_UNREGISTERED;
    filter.layerKey = *layerKey;
    filter.subLayerKey = GUID_STEAM_OFFLINE_SUBLAYER;
    filter.weight.type = FWP_EMPTY;
    /* The classify callback deliberately returns CONTINUE for permits.  A
     * terminating callout treats CONTINUE as an invalid/deny outcome; UNKNOWN
     * lets the callback leave permitted ALE connects to the normal filter
     * engine while BLOCK remains authoritative for matched Steam clients. */
    filter.action.type = FWP_ACTION_CALLOUT_UNKNOWN;
    filter.action.calloutKey = *calloutKey;
    return FwpmFilterAdd0(gWfpEngine, &filter, NULL, filterId);
}

_IRQL_requires_(PASSIVE_LEVEL)
static NTSTATUS SteamOfflineStartWfpLocked(_In_ PDEVICE_OBJECT deviceObject)
{
    FWPM_SESSION0 session;
    FWPM_SUBLAYER0 subLayer;
    FWPM_CALLOUT0 callout;
    FWPS_CALLOUT kernelCallout;
    NTSTATUS status;

    const ULONGLONG startGeneration =
        (ULONGLONG)InterlockedCompareExchange64(&gBfeGeneration, 0, 0);
    InterlockedExchange(&gWfpStopping, 1);
    InterlockedExchange(&gBfeReady, 0);
    InterlockedExchange(&gWfpReady, 0);
    InterlockedExchange64(&gWfpPublishedGeneration, 0);
    if (FwpmBfeStateGet() != FWPM_SERVICE_RUNNING) {
        return STATUS_DEVICE_NOT_READY;
    }
    status = FwpmBfeStateSubscribeChanges(
        deviceObject, SteamOfflineBfeStateChanged, NULL, &gBfeChangeHandle);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    RtlZeroMemory(&session, sizeof(session));
    session.flags = FWPM_SESSION_FLAG_DYNAMIC;
    status = FwpmEngineOpen0(NULL, RPC_C_AUTHN_WINNT, NULL, &session, &gWfpEngine);
    if (!NT_SUCCESS(status)) {
        goto Exit;
    }

    RtlZeroMemory(&subLayer, sizeof(subLayer));
    subLayer.subLayerKey = GUID_STEAM_OFFLINE_SUBLAYER;
    subLayer.displayData.name = L"Vibeshine Steam Offline Isolation";
    subLayer.displayData.description = L"Private sublayer for per-terminal Steam client isolation";
    subLayer.weight = 0x8000;
    status = FwpmSubLayerAdd0(gWfpEngine, &subLayer, NULL);
    if (!NT_SUCCESS(status)) {
        goto Exit;
    }

    RtlZeroMemory(&kernelCallout, sizeof(kernelCallout));
    kernelCallout.calloutKey = GUID_STEAM_OFFLINE_CALLOUT_V4;
    kernelCallout.classifyFn = SteamOfflineClassify;
    kernelCallout.notifyFn = SteamOfflineNotify;
    status = FwpsCalloutRegister(deviceObject, &kernelCallout, &gWfpCalloutV4);
    if (!NT_SUCCESS(status)) {
        goto Exit;
    }
    kernelCallout.calloutKey = GUID_STEAM_OFFLINE_CALLOUT_V6;
    status = FwpsCalloutRegister(deviceObject, &kernelCallout, &gWfpCalloutV6);
    if (!NT_SUCCESS(status)) {
        goto Exit;
    }

    RtlZeroMemory(&callout, sizeof(callout));
    callout.displayData.name = L"Vibeshine Steam Offline Filter V4";
    callout.displayData.description = L"Kernel callout for registered Steam client process identities";
    callout.calloutKey = GUID_STEAM_OFFLINE_CALLOUT_V4;
    callout.applicableLayer = FWPM_LAYER_ALE_AUTH_CONNECT_V4;
    status = FwpmCalloutAdd0(gWfpEngine, &callout, NULL, NULL);
    if (!NT_SUCCESS(status)) {
        goto Exit;
    }
    callout.calloutKey = GUID_STEAM_OFFLINE_CALLOUT_V6;
    callout.displayData.name = L"Vibeshine Steam Offline Filter V6";
    callout.applicableLayer = FWPM_LAYER_ALE_AUTH_CONNECT_V6;
    status = FwpmCalloutAdd0(gWfpEngine, &callout, NULL, NULL);
    if (!NT_SUCCESS(status)) {
        goto Exit;
    }
    status = SteamOfflineAddWfpFilter(
        &GUID_STEAM_OFFLINE_CALLOUT_V4, &FWPM_LAYER_ALE_AUTH_CONNECT_V4, &gWfpFilterV4);
    if (!NT_SUCCESS(status)) {
        goto Exit;
    }
    status = SteamOfflineAddWfpFilter(
        &GUID_STEAM_OFFLINE_CALLOUT_V6, &FWPM_LAYER_ALE_AUTH_CONNECT_V6, &gWfpFilterV6);
    if (NT_SUCCESS(status) &&
        (FwpmBfeStateGet() != FWPM_SERVICE_RUNNING ||
         (ULONGLONG)InterlockedCompareExchange64(&gBfeGeneration, 0, 0) != startGeneration)) {
        status = STATUS_DEVICE_NOT_READY;
    }
    if (NT_SUCCESS(status)) {
        InterlockedExchange64(&gWfpPublishedGeneration, (LONG64)startGeneration);
        InterlockedExchange(&gBfeReady, 1);
        InterlockedExchange(&gWfpReady, 1);
    }

Exit:
    if (NT_SUCCESS(status)) {
        InterlockedExchange(&gWfpStopping, 0);
    }
    return status;
}

_IRQL_requires_(PASSIVE_LEVEL)
static NTSTATUS SteamOfflineStartWfp(_In_ PDEVICE_OBJECT deviceObject)
{
    NTSTATUS status;
    ExAcquireFastMutex(&gWfpStateLock);
    status = SteamOfflineStartWfpLocked(deviceObject);
    ExReleaseFastMutex(&gWfpStateLock);
    if (!NT_SUCCESS(status)) {
        /* Rollback is deliberately outside gWfpStateLock: it detaches the
         * BFE subscription and must unsubscribe only after callbacks can no
         * longer re-enter the state lock. */
        const NTSTATUS cleanupStatus = SteamOfflineStopWfp(NULL);
        if (!NT_SUCCESS(cleanupStatus)) {
            DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
                "VibeshineSteamOfflineFilter: startup WFP rollback diagnostic 0x%08X\n",
                cleanupStatus);
        }
    }
    return status;
}

_IRQL_requires_(PASSIVE_LEVEL)
static NTSTATUS SteamOfflineUnregisterCallout(_Inout_ UINT32* calloutId)
{
    NTSTATUS status = STATUS_SUCCESS;
    LARGE_INTEGER delay;
    ULONG attempt;

    if (calloutId == NULL || *calloutId == 0) {
        return STATUS_SUCCESS;
    }
    delay.QuadPart = -1000000; /* 100 ms; bounded retry, never a busy loop. */
    for (attempt = 0; attempt < 20; ++attempt) {
        status = FwpsCalloutUnregisterById(*calloutId);
        if (NT_SUCCESS(status)) {
            *calloutId = 0;
            return STATUS_SUCCESS;
        }
        if (status != STATUS_DEVICE_BUSY && status != STATUS_PENDING) {
            return status;
        }
        KeDelayExecutionThread(KernelMode, FALSE, &delay);
    }
    return status;
}

_IRQL_requires_(PASSIVE_LEVEL)
static NTSTATUS SteamOfflineStopWfpLocked(_Out_opt_ HANDLE* detachedBfeHandle)
{
    NTSTATUS status;
    NTSTATUS firstFailure = STATUS_SUCCESS;

    if (detachedBfeHandle != NULL) {
        *detachedBfeHandle = NULL;
    }
    InterlockedExchange(&gWfpStopping, 1);
    InterlockedExchange(&gBfeReady, 0);
    InterlockedExchange(&gWfpReady, 0);
    InterlockedExchange64(&gWfpPublishedGeneration, 0);
    if (gWfpEngine != NULL) {
        if (gWfpFilterV6 != 0) {
            status = FwpmFilterDeleteById0(gWfpEngine, gWfpFilterV6);
            if (NT_SUCCESS(status) || status == STATUS_NOT_FOUND) {
                gWfpFilterV6 = 0;
            } else if (NT_SUCCESS(firstFailure)) {
                firstFailure = status;
            }
        }
        if (gWfpFilterV4 != 0) {
            status = FwpmFilterDeleteById0(gWfpEngine, gWfpFilterV4);
            if (NT_SUCCESS(status) || status == STATUS_NOT_FOUND) {
                gWfpFilterV4 = 0;
            } else if (NT_SUCCESS(firstFailure)) {
                firstFailure = status;
            }
        }
        status = FwpmCalloutDeleteByKey0(gWfpEngine, &GUID_STEAM_OFFLINE_CALLOUT_V6);
        if (!NT_SUCCESS(status) && status != STATUS_NOT_FOUND && NT_SUCCESS(firstFailure)) {
            firstFailure = status;
        }
        status = FwpmCalloutDeleteByKey0(gWfpEngine, &GUID_STEAM_OFFLINE_CALLOUT_V4);
        if (!NT_SUCCESS(status) && status != STATUS_NOT_FOUND && NT_SUCCESS(firstFailure)) {
            firstFailure = status;
        }
        status = FwpmSubLayerDeleteByKey0(gWfpEngine, &GUID_STEAM_OFFLINE_SUBLAYER);
        if (!NT_SUCCESS(status) && status != STATUS_NOT_FOUND && NT_SUCCESS(firstFailure)) {
            firstFailure = status;
        }
    }
    if (gWfpCalloutV6 != 0) {
        status = SteamOfflineUnregisterCallout(&gWfpCalloutV6);
        if (!NT_SUCCESS(status) && NT_SUCCESS(firstFailure)) {
            firstFailure = status;
        }
    }
    if (gWfpCalloutV4 != 0) {
        status = SteamOfflineUnregisterCallout(&gWfpCalloutV4);
        if (!NT_SUCCESS(status) && NT_SUCCESS(firstFailure)) {
            firstFailure = status;
        }
    }
    if (gWfpCalloutV6 != 0 || gWfpCalloutV4 != 0) {
        DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
            "VibeshineSteamOfflineFilter: WFP callout unregister failed; driver remains non-unloadable\n");
        return NT_SUCCESS(firstFailure) ? STATUS_DEVICE_BUSY : firstFailure;
    }
    if (gWfpEngine != NULL) {
        status = FwpmEngineClose0(gWfpEngine);
        if (!NT_SUCCESS(status)) {
            if (NT_SUCCESS(firstFailure)) {
                firstFailure = status;
            }
            return firstFailure;
        }
        gWfpEngine = NULL;
        /* Dynamic-session objects are gone with the engine even when an
         * individual delete above supplied only a diagnostic failure.  Do
         * not mistake stale filter IDs for live callback ownership. */
        gWfpFilterV4 = 0;
        gWfpFilterV6 = 0;
    }
    if (gBfeChangeHandle != NULL) {
        if (detachedBfeHandle != NULL) {
            *detachedBfeHandle = gBfeChangeHandle;
            gBfeChangeHandle = NULL;
        } else {
            if (NT_SUCCESS(firstFailure)) {
                firstFailure = STATUS_INVALID_PARAMETER;
            }
            return firstFailure;
        }
    }
    return firstFailure;
}

_IRQL_requires_(PASSIVE_LEVEL)
static NTSTATUS SteamOfflineStopWfp(_Out_opt_ BOOLEAN* ownershipRemaining)
{
    NTSTATUS status;
    NTSTATUS unsubscribeStatus;
    HANDLE detachedBfeHandle = NULL;

    if (ownershipRemaining != NULL) {
        *ownershipRemaining = FALSE;
    }
    ExAcquireFastMutex(&gWfpStateLock);
    status = SteamOfflineStopWfpLocked(&detachedBfeHandle);
    ExReleaseFastMutex(&gWfpStateLock);
    if (detachedBfeHandle != NULL) {
        unsubscribeStatus = FwpmBfeStateUnsubscribeChanges(detachedBfeHandle);
        if (!NT_SUCCESS(unsubscribeStatus)) {
            ExAcquireFastMutex(&gWfpStateLock);
            gBfeChangeHandle = detachedBfeHandle;
            ExReleaseFastMutex(&gWfpStateLock);
            if (NT_SUCCESS(status)) {
                status = unsubscribeStatus;
            }
        } else if (InterlockedCompareExchange(&gBfeCallbackInFlight, 0, 0) != 0 &&
                   NT_SUCCESS(status)) {
            /* Unsubscribe should drain the callback.  Treat a non-zero
             * ownership count as a conservative non-unloadable result even
             * if the engine reported only a diagnostic cleanup error. */
            status = STATUS_DEVICE_BUSY;
        }
    }
    if (ownershipRemaining != NULL) {
        *ownershipRemaining = SteamOfflineCallbacksRemain();
    }
    return status;
}

_IRQL_requires_(PASSIVE_LEVEL)
static NTSTATUS SteamOfflineRemoveProcessNotify(void)
{
    NTSTATUS status = STATUS_SUCCESS;
    LARGE_INTEGER delay;
    ULONG attempt;

    if (!gProcessNotifyRegistered) {
        return STATUS_SUCCESS;
    }
    delay.QuadPart = -1000000; /* 100 ms; bounded retry, never a busy loop. */
    for (attempt = 0; attempt < 20; ++attempt) {
        status = PsSetCreateProcessNotifyRoutineEx(SteamOfflineProcessNotify, TRUE);
        if (NT_SUCCESS(status)) {
            gProcessNotifyRegistered = FALSE;
            return STATUS_SUCCESS;
        }
        if (status != STATUS_DEVICE_BUSY && status != STATUS_PENDING) {
            return status;
        }
        KeDelayExecutionThread(KernelMode, FALSE, &delay);
    }
    return status;
}

static BOOLEAN SteamOfflineCallbacksRemain(void)
{
    return gProcessNotifyRegistered || gWfpCalloutV4 != 0 || gWfpCalloutV6 != 0 ||
        gWfpEngine != NULL || gBfeChangeHandle != NULL ||
        InterlockedCompareExchange(&gBfeCallbackInFlight, 0, 0) != 0;
}

static NTSTATUS SteamOfflineCompleteIrp(_In_ PIRP irp, _In_ NTSTATUS status, _In_ ULONG_PTR information)
{
    irp->IoStatus.Status = status;
    irp->IoStatus.Information = information;
    IoCompleteRequest(irp, IO_NO_INCREMENT);
    return status;
}

static STEAM_OFFLINE_FILE_CONTEXT* SteamOfflineGetFileContext(_In_ PFILE_OBJECT fileObject)
{
    STEAM_OFFLINE_FILE_CONTEXT* context =
        fileObject == NULL ? NULL : (STEAM_OFFLINE_FILE_CONTEXT*)fileObject->FsContext;
    return context != NULL && context->magic == STEAM_OFFLINE_FILE_CONTEXT_MAGIC ? context : NULL;
}

NTSTATUS SteamOfflineCreate(_In_ PDEVICE_OBJECT deviceObject, _Inout_ PIRP irp)
{
    PIO_STACK_LOCATION stack = IoGetCurrentIrpStackLocation(irp);
    STEAM_OFFLINE_FILE_CONTEXT* context;

    UNREFERENCED_PARAMETER(deviceObject);
    context = ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(*context), 'coFS');
    if (context == NULL) {
        return SteamOfflineCompleteIrp(irp, STATUS_INSUFFICIENT_RESOURCES, 0);
    }
    RtlZeroMemory(context, sizeof(*context));
    context->magic = STEAM_OFFLINE_FILE_CONTEXT_MAGIC;
    context->ownerProcess = PsGetCurrentProcess();
    context->ownerPid = PsGetCurrentProcessId();
    context->ownerCreationTime = (ULONGLONG)PsGetProcessCreateTimeQuadPart(context->ownerProcess);
    ObReferenceObject(context->ownerProcess);
    stack->FileObject->FsContext = context;
    return SteamOfflineCompleteIrp(irp, STATUS_SUCCESS, 0);
}

NTSTATUS SteamOfflineClose(_In_ PDEVICE_OBJECT deviceObject, _Inout_ PIRP irp)
{
    PIO_STACK_LOCATION stack = IoGetCurrentIrpStackLocation(irp);
    STEAM_OFFLINE_FILE_CONTEXT* context = SteamOfflineGetFileContext(stack->FileObject);

    UNREFERENCED_PARAMETER(deviceObject);
    SteamOfflineCleanupRegistration(stack->FileObject);
    if (context != NULL) {
        context->magic = 0;
        stack->FileObject->FsContext = NULL;
        ObDereferenceObject(context->ownerProcess);
        ExFreePool(context);
    }
    return SteamOfflineCompleteIrp(irp, STATUS_SUCCESS, 0);
}

NTSTATUS SteamOfflineCleanup(_In_ PDEVICE_OBJECT deviceObject, _Inout_ PIRP irp)
{
    PIO_STACK_LOCATION stack = IoGetCurrentIrpStackLocation(irp);
    UNREFERENCED_PARAMETER(deviceObject);
    SteamOfflineCleanupRegistration(stack->FileObject);
    return SteamOfflineCompleteIrp(irp, STATUS_SUCCESS, 0);
}

NTSTATUS SteamOfflineDeviceControl(_In_ PDEVICE_OBJECT deviceObject, _Inout_ PIRP irp)
{
    PIO_STACK_LOCATION stack;
    STEAM_OFFLINE_FILE_CONTEXT* context;
    NTSTATUS status;
    ULONG_PTR information = 0;
    PVOID buffer;

    UNREFERENCED_PARAMETER(deviceObject);
    stack = IoGetCurrentIrpStackLocation(irp);
    context = SteamOfflineGetFileContext(stack->FileObject);
    buffer = irp->AssociatedIrp.SystemBuffer;
    if (context == NULL || context->ownerProcess != PsGetCurrentProcess() ||
        context->ownerPid != PsGetCurrentProcessId() ||
        context->ownerCreationTime != (ULONGLONG)PsGetProcessCreateTimeQuadPart(context->ownerProcess) ||
        !SteamOfflineCallerIsSystem()) {
        return SteamOfflineCompleteIrp(irp, STATUS_ACCESS_DENIED, 0);
    }
    if (buffer == NULL || stack->Parameters.DeviceIoControl.InputBufferLength > PAGE_SIZE) {
        return SteamOfflineCompleteIrp(irp, STATUS_INVALID_BUFFER_SIZE, 0);
    }

    switch (stack->Parameters.DeviceIoControl.IoControlCode) {
    case STEAM_OFFLINE_REGISTER_ROOT_IOCTL:
        if (stack->Parameters.DeviceIoControl.InputBufferLength != sizeof(STEAM_OFFLINE_REGISTER_ROOT) ||
            stack->Parameters.DeviceIoControl.OutputBufferLength < sizeof(STEAM_OFFLINE_REGISTRATION)) {
            status = STATUS_BUFFER_TOO_SMALL;
            break;
        }
        status = SteamOfflineRegisterRoot(
            (const STEAM_OFFLINE_REGISTER_ROOT*)buffer,
            (STEAM_OFFLINE_REGISTRATION*)buffer,
            stack->FileObject,
            context->ownerProcess,
            context->ownerPid,
            context->ownerCreationTime);
        if (NT_SUCCESS(status)) {
            information = sizeof(STEAM_OFFLINE_REGISTRATION);
        }
        break;
    case STEAM_OFFLINE_UNREGISTER_ROOT_IOCTL:
        if (stack->Parameters.DeviceIoControl.InputBufferLength != sizeof(STEAM_OFFLINE_UNREGISTER_ROOT)) {
            status = STATUS_BUFFER_TOO_SMALL;
            break;
        }
        status = SteamOfflineUnregisterRoot(
            (const STEAM_OFFLINE_UNREGISTER_ROOT*)buffer,
            stack->FileObject,
            context->ownerProcess,
            context->ownerPid,
            context->ownerCreationTime);
        break;
    case STEAM_OFFLINE_STATUS_IOCTL:
        if (stack->Parameters.DeviceIoControl.InputBufferLength != 0 ||
            stack->Parameters.DeviceIoControl.OutputBufferLength < sizeof(STEAM_OFFLINE_STATUS)) {
            status = STATUS_BUFFER_TOO_SMALL;
            break;
        }
        {
            STEAM_OFFLINE_STATUS* output = (STEAM_OFFLINE_STATUS*)buffer;
            ULONGLONG currentGeneration;
            ULONGLONG publishedGeneration;
            BOOLEAN publishedReady;
            ExAcquireFastMutex(&gWfpStateLock);
            currentGeneration = (ULONGLONG)InterlockedCompareExchange64(&gBfeGeneration, 0, 0);
            publishedGeneration = (ULONGLONG)InterlockedCompareExchange64(&gWfpPublishedGeneration, 0, 0);
            publishedReady = InterlockedCompareExchange(&gWfpStopping, 0, 0) == 0 &&
                InterlockedCompareExchange(&gBfeReady, 0, 0) != 0 &&
                InterlockedCompareExchange(&gWfpReady, 0, 0) != 0 &&
                currentGeneration != 0 && currentGeneration == publishedGeneration;
            output->version = STEAM_OFFLINE_PROTOCOL_VERSION;
            output->reserved = 0;
            output->bfeReady = publishedReady;
            output->wfpReady = publishedReady;
            output->bfeGeneration = currentGeneration;
            ExReleaseFastMutex(&gWfpStateLock);
            information = sizeof(*output);
            status = STATUS_SUCCESS;
        }
        break;
    default:
        status = STATUS_INVALID_DEVICE_REQUEST;
        break;
    }
    return SteamOfflineCompleteIrp(irp, status, information);
}

void SteamOfflineUnload(_In_ PDRIVER_OBJECT driverObject)
{
    UNICODE_STRING dosName;
    NTSTATUS status;
    NTSTATUS diagnosticStatus = STATUS_SUCCESS;
    BOOLEAN ownershipRemaining = FALSE;

    gUnloading = TRUE;
    status = SteamOfflineRemoveProcessNotify();
    if (!NT_SUCCESS(status) && NT_SUCCESS(diagnosticStatus)) {
        diagnosticStatus = status;
    }
    /* A failed process-notify removal cannot be made safe by WFP teardown:
     * the process callback itself still owns this image.  WFP/BFE ownership
     * is handled below so a diagnostic WFP failure never skips its cleanup. */
    if (gProcessNotifyRegistered) {
        DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
            "VibeshineSteamOfflineFilter: process-notify ownership remains; refusing image teardown (diagnostic 0x%08X)\n",
            diagnosticStatus);
        InterlockedExchange(&gWfpStopping, 1);
        InterlockedExchange(&gBfeReady, 0);
        InterlockedExchange(&gWfpReady, 0);
        InterlockedExchange64(&gWfpPublishedGeneration, 0);
        gUnloading = FALSE;
        return;
    }
    status = SteamOfflineStopWfp(&ownershipRemaining);
    if (!NT_SUCCESS(status) && NT_SUCCESS(diagnosticStatus)) {
        diagnosticStatus = status;
    }
    if (ownershipRemaining || SteamOfflineCallbacksRemain()) {
        DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
            "VibeshineSteamOfflineFilter: callback ownership remains; refusing image teardown (diagnostic 0x%08X)\n",
            diagnosticStatus);
        gUnloading = FALSE;
        return;
    }
    SteamOfflineCleanupAllRegistrations();
    RtlInitUnicodeString(&dosName, STEAM_OFFLINE_DOS_DEVICE_NAME);
    IoDeleteSymbolicLink(&dosName);
    if (gSteamOfflineDevice != NULL) {
        IoDeleteDevice(gSteamOfflineDevice);
        gSteamOfflineDevice = NULL;
    }
    if (!NT_SUCCESS(diagnosticStatus)) {
        DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
            "VibeshineSteamOfflineFilter: completed teardown after diagnostic cleanup status 0x%08X\n",
            diagnosticStatus);
    }
    gUnloading = FALSE;
    UNREFERENCED_PARAMETER(driverObject);
}

NTSTATUS DriverEntry(_In_ PDRIVER_OBJECT driverObject, _In_ PUNICODE_STRING registryPath)
{
    UNICODE_STRING deviceName;
    UNICODE_STRING dosName;
    UNICODE_STRING sddl;
    NTSTATUS status;
    ULONG i;

    UNREFERENCED_PARAMETER(registryPath);
    KeInitializeSpinLock(&gProcessLock);
    ExInitializeFastMutex(&gWfpStateLock);
    InterlockedExchange64(&gBfeGeneration, 1);
    for (i = 0; i < STEAM_OFFLINE_BUCKET_COUNT; ++i) {
        gBuckets[i] = STEAM_OFFLINE_MAX_PROCESSES;
    }
    for (i = 0; i < STEAM_OFFLINE_MAX_PROCESSES; ++i) {
        gProcesses[i].next = STEAM_OFFLINE_MAX_PROCESSES;
    }

    RtlInitUnicodeString(&deviceName, STEAM_OFFLINE_DEVICE_NAME);
    RtlInitUnicodeString(&dosName, STEAM_OFFLINE_DOS_DEVICE_NAME);
    RtlInitUnicodeString(&sddl, L"D:P(A;;GA;;;SY)");
    status = IoCreateDeviceSecure(
        driverObject,
        0,
        &deviceName,
        FILE_DEVICE_NETWORK,
        FILE_DEVICE_SECURE_OPEN,
        FALSE,
        &sddl,
        &GUID_STEAM_OFFLINE_DEVICE_CLASS,
        &gSteamOfflineDevice);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    gSteamOfflineDevice->Flags |= DO_BUFFERED_IO;
    driverObject->MajorFunction[IRP_MJ_CREATE] = SteamOfflineCreate;
    driverObject->MajorFunction[IRP_MJ_CLOSE] = SteamOfflineClose;
    driverObject->MajorFunction[IRP_MJ_CLEANUP] = SteamOfflineCleanup;
    driverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = SteamOfflineDeviceControl;
    driverObject->DriverUnload = SteamOfflineUnload;

    status = IoCreateSymbolicLink(&dosName, &deviceName);
    if (!NT_SUCCESS(status)) {
        IoDeleteDevice(gSteamOfflineDevice);
        gSteamOfflineDevice = NULL;
        return status;
    }
    status = PsSetCreateProcessNotifyRoutineEx(SteamOfflineProcessNotify, FALSE);
    if (!NT_SUCCESS(status)) {
        IoDeleteSymbolicLink(&dosName);
        IoDeleteDevice(gSteamOfflineDevice);
        gSteamOfflineDevice = NULL;
        return status;
    }
    gProcessNotifyRegistered = TRUE;
    status = SteamOfflineStartWfp(gSteamOfflineDevice);
    if (!NT_SUCCESS(status)) {
        SteamOfflineUnload(driverObject);
        if (SteamOfflineCallbacksRemain()) {
            /* DriverEntry cannot return failure while a callback can still
             * execute from this image: the loader would then attempt to
             * unload code whose unregister path has not completed.  Remove
             * the public name and retain DO_DEVICE_INITIALIZING, leaving a
             * dormant, non-openable, non-unloadable image instead.  The
             * readiness flags are poisoned so SYSTEM admission fails closed
             * until a reboot stages removal and a fresh driver start. */
            DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
                "VibeshineSteamOfflineFilter: startup rollback incomplete; retaining dormant driver until reboot\n");
            driverObject->DriverUnload = NULL;
            if (gSteamOfflineDevice != NULL) {
                NTSTATUS linkStatus = IoDeleteSymbolicLink(&dosName);
                if (!NT_SUCCESS(linkStatus)) {
                    DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
                        "VibeshineSteamOfflineFilter: failed to remove dormant device name 0x%08X\n",
                        linkStatus);
                }
                gSteamOfflineDevice->Flags |= DO_DEVICE_INITIALIZING;
            }
            return STATUS_SUCCESS;
        }
        return status;
    }
    /* A successfully running WFP callout must not be unloaded through a
     * service stop.  The checked unregister path remains available for
     * DriverEntry rollback, while a live service stays resident until reboot;
     * this prevents DriverUnload from returning while callbacks remain. */
    driverObject->DriverUnload = NULL;
    gSteamOfflineDevice->Flags &= ~DO_DEVICE_INITIALIZING;
    return STATUS_SUCCESS;
}
