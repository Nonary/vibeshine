#pragma once

#include <ntddk.h>

#define STEAM_OFFLINE_DEVICE_NAME L"\\Device\\VibeshineSteamOfflineFilter"
#define STEAM_OFFLINE_DOS_DEVICE_NAME L"\\DosDevices\\VibeshineSteamOfflineFilter"

#define STEAM_OFFLINE_MAX_PROCESSES 1024u
#define STEAM_OFFLINE_MAX_PROCESSES_PER_REGISTRATION 64u
#define STEAM_OFFLINE_MAX_IMAGE_PATH_CHARS 520u
#define STEAM_OFFLINE_BUCKET_COUNT 64u

/* IOCTL/control-device and WFP registration are deliberately separate. */
extern PDEVICE_OBJECT gSteamOfflineDevice;

DRIVER_INITIALIZE DriverEntry;
DRIVER_UNLOAD SteamOfflineUnload;

_Dispatch_type_(IRP_MJ_CREATE)
DRIVER_DISPATCH SteamOfflineCreate;
_Dispatch_type_(IRP_MJ_CLOSE)
DRIVER_DISPATCH SteamOfflineClose;
_Dispatch_type_(IRP_MJ_CLEANUP)
DRIVER_DISPATCH SteamOfflineCleanup;
_Dispatch_type_(IRP_MJ_DEVICE_CONTROL)
DRIVER_DISPATCH SteamOfflineDeviceControl;
