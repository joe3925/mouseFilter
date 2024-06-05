#pragma once
#include <ntddk.h>
#include <kbdmou.h>
#include <ntddmou.h>
#include <ntdd8042.h>
#include <wdf.h>
#if DBG
#define DebugPrint(_x_) DbgPrint _x_
#else
#define DebugPrint(_x_)
#endif

#define NTDEVICE_NAME         L"\\Device\\moufiltr"
#define SYMBOLIC_NAME_STRING  L"\\DosDevices\\moufiltr"

using counter_t = long long;
constexpr size_t MAX_DEV_ID_LEN = 200;
constexpr ULONG IOCTL_GET_CURRENT_PACKET = (ULONG)CTL_CODE(0x8888u, 0x88a, METHOD_BUFFERED, FILE_ANY_ACCESS);

typedef struct _DEVICE_EXTENSION {
    bool enable;
    bool keep_time;
    bool set_extra_info;
    double dpi_factor;
    counter_t counter;
    CONNECT_DATA UpperConnectData;
    WCHAR dev_id[MAX_DEV_ID_LEN];
} DEVICE_EXTENSION, * PDEVICE_EXTENSION;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(DEVICE_EXTENSION, FilterGetData)

EXTERN_C_START

DRIVER_INITIALIZE DriverEntry;

EVT_WDF_DRIVER_DEVICE_ADD EvtDeviceAdd;
EVT_WDF_IO_QUEUE_IO_INTERNAL_DEVICE_CONTROL EvtIoInternalDeviceControl;
EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL IOcontrol;
EVT_WDF_OBJECT_CONTEXT_CLEANUP DeviceCleanup;

VOID WriteDelay(VOID);
VOID DriverInit(WDFDRIVER);
NTSTATUS CreateControlDevice(WDFDRIVER);

EXTERN_C_END

VOID FilterCallback(
    IN PDEVICE_OBJECT DeviceObject,
    IN PMOUSE_INPUT_DATA InputDataStart,
    IN PMOUSE_INPUT_DATA InputDataEnd,
    IN OUT PULONG InputDataConsumed
);

VOID DispatchPassThrough(
    _In_ WDFREQUEST Request,
    _In_ WDFIOTARGET Target
);
