#include "moufiltr.h"
#include <ntdef.h>
#include <kbdmou.h>
#include <Ntddmou.h>
#include <initguid.h>






#ifdef ALLOC_PRAGMA
#pragma alloc_text (INIT, DriverEntry)
#pragma alloc_text (INIT, DriverInit)
#pragma alloc_text (INIT, CreateControlDevice)
#pragma alloc_text (PAGE, EvtDeviceAdd)
#pragma alloc_text (PAGE, EvtIoInternalDeviceControl)
#pragma alloc_text (PAGE, IOcontrol)
#pragma alloc_text (PAGE, DeviceCleanup)
#pragma alloc_text (PAGE, WriteDelay)
#endif

using milliseconds = double;

struct {
    bool initialized;
    WDFCOLLECTION device_collection;
    WDFWAITLOCK collection_lock;
    milliseconds tick_interval;
	MOUSE_INPUT_DATA currentPacket;
} global = {};


extern "C" PULONG InitSafeBootMode;
bool toggle = false;


#pragma float_control(pop)

__declspec(guard(ignore))
VOID
FilterCallback(
    IN PDEVICE_OBJECT DeviceObject,
    IN PMOUSE_INPUT_DATA InputDataStart,
    IN PMOUSE_INPUT_DATA InputDataEnd,
    IN OUT PULONG InputDataConsumed
)
/*++

Routine Description:

    Called when there are mouse packets to report to the RIT.

Arguments:

    DeviceObject - Context passed during the connect IOCTL

    InputDataStart - First packet to be reported

    InputDataEnd - One past the last packet to be reported.  Total number of
                   packets is equal to InputDataEnd - InputDataStart

    InputDataConsumed - Set to the total number of packets consumed by the RIT
                        (via the function pointer we replaced in the connect
                        IOCTL)

--*/
{
    PDEVICE_EXTENSION   devExt;
    WDFDEVICE   hDevice;
	PMOUSE_INPUT_DATA currentPacket = InputDataStart;
    hDevice = WdfWdmDeviceGetWdfDeviceHandle(DeviceObject);

    devExt = FilterGetData(hDevice);
    auto num_packets = InputDataEnd - InputDataStart;
    global.currentPacket = *InputDataStart;
        //
    // UpperConnectData must be called at DISPATCH
    //
    (*(PSERVICE_CALLBACK_ROUTINE)devExt->UpperConnectData.ClassService)(
        devExt->UpperConnectData.ClassDeviceObject,
        InputDataStart,
        InputDataEnd,
        InputDataConsumed
        );
}


#pragma warning(push)
#pragma warning(disable:28118) // this callback will run at IRQL=PASSIVE_LEVEL
_Use_decl_annotations_
VOID
IOcontrol(
    WDFQUEUE         Queue,
    WDFREQUEST       Request,
    size_t           OutputBufferLength,
    size_t           InputBufferLength,
    ULONG            IoControlCode
)
/*++
Routine Description:
    This event is called when the framework receives IRP_MJ_DEVICE_CONTROL
    requests from the system.
Arguments:
    Queue - Handle to the framework queue object that is associated
            with the I/O request.
    Request - Handle to a framework request object.
    OutputBufferLength - length of the request's output buffer,
                        if an output buffer is available.
    InputBufferLength - length of the request's input buffer,
                        if an input buffer is available.
    IoControlCode - the driver-defined or system-defined I/O control code
                    (IOCTL) that is associated with the request.
Return Value:
   VOID
--*/
{
    NTSTATUS status = STATUS_SUCCESS;
    void* outputBuffer;
    size_t buffer_length = sizeof(MOUSE_INPUT_DATA);
    size_t bytes_out = 0;

    UNREFERENCED_PARAMETER(Queue);
    UNREFERENCED_PARAMETER(InputBufferLength);
    PAGED_CODE();

    DebugPrint(("Ioctl received into filter control object.\n"));

    if (!global.initialized) {
        WdfRequestCompleteWithInformation(Request, STATUS_CANCELLED, 0);
        return;
    }

    switch (IoControlCode) {
    case IOCTL_GET_CURRENT_PACKET:
        if (OutputBufferLength < buffer_length) {
            status = STATUS_BUFFER_TOO_SMALL;
            WdfRequestCompleteWithInformation(Request, status, 0);
        }
        else {
            status = WdfRequestRetrieveOutputBuffer(Request, buffer_length, &outputBuffer, &buffer_length);

            if (NT_SUCCESS(status)) {
                size_t copySize = buffer_length;
                RtlCopyMemory(outputBuffer, &global.currentPacket, copySize);
                bytes_out = copySize;
            }
            //fun fact: if this is just wdfrequestcomplete, the request won't work 
			WdfRequestCompleteWithInformation(Request, status, bytes_out);
        }
        break;

    default:
        break;
    }
#pragma warning(pop)
}

VOID
DriverInit(WDFDRIVER driver)
{
    NTSTATUS status;

    status = CreateControlDevice(driver);

    if (!NT_SUCCESS(status)) {
        DebugPrint(("CreateControlDevice failed with status 0x%x\n", status));
        return;
    }

    status = WdfCollectionCreate(
        WDF_NO_OBJECT_ATTRIBUTES,
        &global.device_collection
    );

    if (!NT_SUCCESS(status)) {
        DebugPrint(("WdfCollectionCreate failed with status 0x%x\n", status));
        return;
    }

    status = WdfWaitLockCreate(
        WDF_NO_OBJECT_ATTRIBUTES,
        &global.collection_lock
    );

    if (!NT_SUCCESS(status)) {
        DebugPrint(("WdfWaitLockCreate failed with status 0x%x\n", status));
        return;
    }

    LARGE_INTEGER freq;
    KeQueryPerformanceCounter(&freq);
    global.initialized = true;
}


VOID
EvtDeviceCleanup(WDFOBJECT hDevice)
{
    PAGED_CODE();
    DebugPrint(("Removing device from collection\n"));

    WdfWaitLockAcquire(global.collection_lock, NULL);
    WdfCollectionRemove(global.device_collection, hDevice);
    WdfWaitLockRelease(global.collection_lock);
}

NTSTATUS
DriverEntry(
    IN  PDRIVER_OBJECT  DriverObject,
    IN  PUNICODE_STRING RegistryPath
)
/*++
Routine Description:

     Installable driver initialization entry point.
    This entry point is called directly by the I/O system.

--*/
{

    WDF_DRIVER_CONFIG config;
    NTSTATUS status;
    WDFDRIVER driver;

    WDF_DRIVER_CONFIG_INIT(
        &config,
        EvtDeviceAdd
    );

    //
    // Create a framework driver object to represent our driver.
    //
    status = WdfDriverCreate(DriverObject,
        RegistryPath,
        WDF_NO_OBJECT_ATTRIBUTES,
        &config,
        &driver);

    __debugbreak();

    if (NT_SUCCESS(status)) {
       DriverInit(driver);
    }
    else {
        DebugPrint(("WdfDriverCreate failed with status 0x%x\n", status));
    }

    return status;
}


NTSTATUS
CreateControlDevice(WDFDRIVER Driver)
/*++
Routine Description:
    This routine is called to create a control device object so that application
    can talk to the filter driver directly instead of going through the entire
    device stack. This kind of control device object is useful if the filter
    driver is underneath another driver which prevents ioctls not known to it
    or if the driver's dispatch routine is owned by some other (port/class)
    driver and it doesn't allow any custom ioctls.
Arguments:
    Driver - Handle to wdf driver object.
Return Value:
    WDF status code
--*/
{
    PWDFDEVICE_INIT             pInit = NULL;
    WDFDEVICE                   controlDevice = NULL;
    WDF_IO_QUEUE_CONFIG         ioQueueConfig;
    NTSTATUS                    status;
    WDFQUEUE                    queue;
    DECLARE_CONST_UNICODE_STRING(ntDeviceName, NTDEVICE_NAME);
    DECLARE_CONST_UNICODE_STRING(symbolicLinkName, SYMBOLIC_NAME_STRING);

    DebugPrint(("Creating Control Device\n"));

	//alloc a WDFDEVICE_INIT structure
    pInit = WdfControlDeviceInitAllocate(
        Driver,
        &SDDL_DEVOBJ_SYS_ALL_ADM_RWX_WORLD_RW_RES_R
    );

    if (pInit == NULL) {
        status = STATUS_INSUFFICIENT_RESOURCES;
        goto Error;
    }

    //
    // Set exclusive to false so that more than one app can talk to the
    // control device simultaneously.
    //
    WdfDeviceInitSetExclusive(pInit, FALSE);

    status = WdfDeviceInitAssignName(pInit, &ntDeviceName);

    if (!NT_SUCCESS(status)) {
        goto Error;
    }

    status = WdfDeviceCreate(&pInit,
        WDF_NO_OBJECT_ATTRIBUTES,
        &controlDevice);

    if (!NT_SUCCESS(status)) {
        goto Error;
    }

    status = WdfDeviceCreateSymbolicLink(controlDevice, &symbolicLinkName);

    if (!NT_SUCCESS(status)) {
        goto Error;
    }

    // Configure the q for ioctls

    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&ioQueueConfig,
        WdfIoQueueDispatchSequential);

    ioQueueConfig.EvtIoDeviceControl = IOcontrol;

    status = WdfIoQueueCreate(controlDevice,
        &ioQueueConfig,
        WDF_NO_OBJECT_ATTRIBUTES,
        &queue // pointer to default queue
    );
    if (!NT_SUCCESS(status)) {
        goto Error;
    }

    //
    // Control devices must notify WDF when they are done initializing.   I/O is
    // rejected until this call is made.
    //
    WdfControlFinishInitializing(controlDevice);

    return STATUS_SUCCESS;

Error:

    if (pInit != NULL) WdfDeviceInitFree(pInit);

    if (controlDevice != NULL) {
        WdfObjectDelete(controlDevice);
    }

    DebugPrint(("CreateControlDevice failed with status code 0x%x\n", status));

    return status;
}

NTSTATUS
EvtDeviceAdd(
    IN WDFDRIVER        Driver,
    IN PWDFDEVICE_INIT  DeviceInit
)
/*++
Routine Description:

    EvtDeviceAdd is called by the framework in response to AddDevice
    call from the PnP manager. Here you can query the device properties
    using WdfFdoInitWdmGetPhysicalDevice/IoGetDeviceProperty and based
    on that, decide to create a filter device object and attach to the
    function stack.

    If you are not interested in filtering this particular instance of the
    device, you can just return STATUS_SUCCESS without creating a framework
    device.

Arguments:

    Driver - Handle to a framework driver object created in DriverEntry

    DeviceInit - Pointer to a framework-allocated WDFDEVICE_INIT structure.

Return Value:

    NTSTATUS

--*/
{
    WDF_OBJECT_ATTRIBUTES deviceAttributes;
    NTSTATUS status;
    WDFDEVICE hDevice;
    WDF_IO_QUEUE_CONFIG ioQueueConfig;

    UNREFERENCED_PARAMETER(Driver);

    PAGED_CODE();

    DebugPrint(("Enter FilterEvtDeviceAdd \n"));

    if (!global.initialized) {
        return STATUS_SUCCESS;
    }

    //
    // Tell the framework that you are filter driver. Framework
    // takes care of inherting all the device flags & characterstics
    // from the lower device you are attaching to.
    //
    WdfFdoInitSetFilter(DeviceInit);

    WdfDeviceInitSetDeviceType(DeviceInit, FILE_DEVICE_MOUSE);

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&deviceAttributes,
        DEVICE_EXTENSION);

    deviceAttributes.EvtCleanupCallback = EvtDeviceCleanup;

    status = WdfDeviceCreate(&DeviceInit, &deviceAttributes, &hDevice);
    if (!NT_SUCCESS(status)) {
        DebugPrint(("WdfDeviceCreate failed with status code 0x%x\n", status));
        return status;
    }

    //
    // get device id from bus driver from raw accel
    //
    DEVICE_OBJECT* pdo = WdfDeviceWdmGetPhysicalDevice(hDevice);

    KEVENT ke;
    KeInitializeEvent(&ke, NotificationEvent, FALSE);
    IO_STATUS_BLOCK iosb = {};
    PIRP Irp = IoBuildSynchronousFsdRequest(IRP_MJ_PNP,
        pdo, NULL, 0, NULL, &ke, &iosb);
    Irp->IoStatus.Status = STATUS_NOT_SUPPORTED;
    PIO_STACK_LOCATION stack = IoGetNextIrpStackLocation(Irp);
    stack->MinorFunction = IRP_MN_QUERY_ID;
    stack->Parameters.QueryId.IdType = BusQueryDeviceID;

    NTSTATUS tmp = IoCallDriver(pdo, Irp);

    if (tmp == STATUS_PENDING) {
        KeWaitForSingleObject(&ke, Executive, KernelMode, FALSE, NULL);
        tmp = iosb.Status;
    }

    auto* devExt = FilterGetData(hDevice);

    if (NT_SUCCESS(tmp)) {
        auto* id_ptr = reinterpret_cast<WCHAR*>(iosb.Information);
        wcsncpy(devExt->dev_id, id_ptr, MAX_DEV_ID_LEN);
        ExFreePool(id_ptr);
    }
    else {
        DebugPrint(("IoCallDriver failed with status 0x%x\n", tmp));
        *devExt->dev_id = L'\0';
    }

    WdfWaitLockAcquire(global.collection_lock, NULL);
    tmp = WdfCollectionAdd(global.device_collection, hDevice);

    if (!NT_SUCCESS(tmp)) {
        DebugPrint(("WdfCollectionAdd failed with status 0x%x\n", tmp));
    }

    WdfWaitLockRelease(global.collection_lock);

    //
    // Configure the default queue to be Parallel. Do not use sequential queue
    // if this driver is going to be filtering PS2 ports because it can lead to
    // deadlock. The PS2 port driver sends a request to the top of the stack when it
    // receives an ioctl request and waits for it to be completed. If you use a
    // a sequential queue, this request will be stuck in the queue because of the 
    // outstanding ioctl request sent earlier to the port driver.
    //
    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&ioQueueConfig,
        WdfIoQueueDispatchParallel);

    //
    // Framework by default creates non-power managed queues for
    // filter drivers.
    //
    ioQueueConfig.EvtIoInternalDeviceControl = EvtIoInternalDeviceControl;

    status = WdfIoQueueCreate(hDevice,
        &ioQueueConfig,
        WDF_NO_OBJECT_ATTRIBUTES,
        WDF_NO_HANDLE // pointer to default queue
    );
    if (!NT_SUCCESS(status)) {
        DebugPrint(("WdfIoQueueCreate failed 0x%x\n", status));
        return status;
    }

    return status;
}


VOID
EvtIoInternalDeviceControl(
    IN WDFQUEUE      Queue,
    IN WDFREQUEST    Request,
    IN size_t        OutputBufferLength,
    IN size_t        InputBufferLength,
    IN ULONG         IoControlCode
)
/*++

Routine Description:

    This routine is the dispatch routine for internal device control requests.
    There are two specific control codes that are of interest:

    IOCTL_INTERNAL_MOUSE_CONNECT:
        Store the old context and function pointer and replace it with our own.
        This makes life much simpler than intercepting IRPs sent by the RIT and
        modifying them on the way back up.

    IOCTL_INTERNAL_I8042_HOOK_MOUSE:
        Add in the necessary function pointers and context values so that we can
        alter how the ps/2 mouse is initialized.

    NOTE:  Handling IOCTL_INTERNAL_I8042_HOOK_MOUSE is *NOT* necessary if
           all you want to do is filter MOUSE_INPUT_DATAs.  You can remove
           the handling code and all related device extension fields and
           functions to conserve space.


--*/
{

    PDEVICE_EXTENSION devExt;
    PCONNECT_DATA connectData;
    NTSTATUS status = STATUS_SUCCESS;
    WDFDEVICE hDevice;
    size_t length;

    UNREFERENCED_PARAMETER(OutputBufferLength);
    UNREFERENCED_PARAMETER(InputBufferLength);

    PAGED_CODE();

    hDevice = WdfIoQueueGetDevice(Queue);
    devExt = FilterGetData(hDevice);

    switch (IoControlCode) {

        //
        // Connect a mouse class device driver to the port driver.
        //
    case IOCTL_INTERNAL_MOUSE_CONNECT: {
        //
        // Only allow one connection.
        //
        if (devExt->UpperConnectData.ClassService != NULL) {
            status = STATUS_SHARING_VIOLATION;
            break;
        }

        //
        // Copy the connection parameters to the device extension.
        //
        status = WdfRequestRetrieveInputBuffer(Request,
            sizeof(CONNECT_DATA),
            reinterpret_cast<PVOID*>(&connectData),
            &length);
        if (!NT_SUCCESS(status)) {
            DebugPrint(("WdfRequestRetrieveInputBuffer failed %x\n", status));
            break;
        }

        devExt->UpperConnectData = *connectData;

        //
        // Hook into the report chain.  Everytime a mouse packet is reported to
        // the system, the callback will be called
        //

        connectData->ClassDeviceObject = WdfDeviceWdmGetDeviceObject(hDevice);
        connectData->ClassService = FilterCallback;

        break;
    }
                                     //
                                     // Disconnect a mouse class device driver from the port driver.
                                     //
    case IOCTL_INTERNAL_MOUSE_DISCONNECT:
        //
        // Clear the connection parameters in the device extension.
        //
        // devExt->UpperConnectData.ClassDeviceObject = NULL;
        // devExt->UpperConnectData.ClassService = NULL;
        status = STATUS_NOT_IMPLEMENTED;
        break;
    default:
        break;
    }

    if (!NT_SUCCESS(status)) {
        WdfRequestComplete(Request, status);
        return;
    }

    DispatchPassThrough(Request, WdfDeviceGetIoTarget(hDevice));
}


VOID
DispatchPassThrough(
    _In_ WDFREQUEST Request,
    _In_ WDFIOTARGET Target
)
/*++
Routine Description:

    Passes a request on to the lower driver.


--*/
{
    //
    // Pass the IRP to the target
    //

    WDF_REQUEST_SEND_OPTIONS options;
    BOOLEAN ret;
    NTSTATUS status = STATUS_SUCCESS;

    //
    // We are not interested in post processing the IRP so 
    // fire and forget.
    //
    WDF_REQUEST_SEND_OPTIONS_INIT(&options,
        WDF_REQUEST_SEND_OPTION_SEND_AND_FORGET);

    ret = WdfRequestSend(Request, Target, &options);

    if (ret == FALSE) {
        status = WdfRequestGetStatus(Request);
        DebugPrint(("WdfRequestSend failed: 0x%x\n", status));
        WdfRequestComplete(Request, status);
    }

    return;
}
