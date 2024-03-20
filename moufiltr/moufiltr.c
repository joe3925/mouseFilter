/*--         
Copyright (c) 2008  Microsoft Corporation

Module Name:

    moufiltr.c

Abstract:

Environment:

    Kernel mode only- Framework Version 

Notes:


--*/

#include "moufiltr.h"
#include <ntdef.h>
#include <kbdmou.h>
#include <Ntddmou.h>
#include <initguid.h>

#ifdef ALLOC_PRAGMA
#pragma alloc_text (INIT, DriverEntry)
#pragma alloc_text (PAGE, MouFilter_EvtIoInternalDeviceControl)
#endif

#pragma warning(push)
#pragma warning(disable:4055) // type case from PVOID to PSERVICE_CALLBACK_ROUTINE
#pragma warning(disable:4152) // function/data pointer conversion in expression
#define TEST CTL_CODE(0x8888u, 0x888, METHOD_BUFFERED, FILE_ANY_ACCESS);


int CreateControlDevice(WDFDRIVER);
int MouFilterDeviceCreate(IN WDFDRIVER hdriver, IN PWDFDEVICE_INIT  DeviceInit);
void DeviceCleanup(WDFOBJECT hDevice);
void mouFiltrIoctl(
    WDFQUEUE         Queue,
    WDFREQUEST       Request,
    size_t           OutputBufferLength,
    size_t           InputBufferLength,
    ULONG            IoControlCode
);
int EvtShutdownNotification(WDFDRIVER driver);
void MyEvtDriverUnload(_In_ WDFDRIVER Driver);
void EvtIoWrite(WDFQUEUE Queue, WDFREQUEST Request, size_t Length);
void EvtIoRead(WDFQUEUE Queue, WDFREQUEST Request, size_t Length);
void IO_deviceCreate(WDFDRIVER hDriver);

struct {
    WDFCOLLECTION device_collection;
    WDFWAITLOCK collection_lock;
} global;

DEFINE_GUID(GUID_DEVINTERFACE_MOUSE, 0x378de44c, 0x56ef, 0x11d1,
    0xbc, 0x8c, 0x00, 0xa0, 0xc9, 0x14, 0x05, 0xdd);
DECLARE_CONST_UNICODE_STRING(
    SDDL_DEVOBJ_SYS_ALL_ADM_RWX_WORLD_RW_RES_R,
    L"D:P(A;;GA;;;SY)(A;;GRGWGX;;;BA)(A;;GRGW;;;WD)(A;;GR;;;RC)"
);

WDFCOLLECTION collection;
NTSTATUS
DriverEntry (
    IN  PDRIVER_OBJECT  DriverObject,
    IN  PUNICODE_STRING RegistryPath
    )
/*++
Routine Description:

     Installable driver initialization entry point.
    This entry point is called directly by the I/O system.

--*/
{
    WDF_DRIVER_CONFIG               config;
    NTSTATUS                                status;
    WDFDRIVER hDriver;
    WDF_OBJECT_ATTRIBUTES deviceAttributes;
    DebugPrint(("Mouse Filter Driver Sample - Driver Framework Edition.\n"));
    DebugPrint(("Built %s %s\n", __DATE__, __TIME__));
    
    // Initialize driver config to control the attributes that
    // are global to the driver. Note that framework by default
    // provides a driver unload routine. If you create any resources
    // in the DriverEntry and want to be cleaned in driver unload,
    // you can override that by manually setting the EvtDriverUnload in the
    // config structure. In general xxx_CONFIG_INIT macros are provided to
    // initialize most commonly used members.

    WDF_DRIVER_CONFIG_INIT(&config, MouFilterDeviceCreate);

    config.EvtDriverUnload = MyEvtDriverUnload;

    WDF_OBJECT_ATTRIBUTES_INIT(&deviceAttributes);

    //
    // Create a framework driver object to represent our driver.
    //
    status = WdfDriverCreate(DriverObject,
                            RegistryPath,
                            &deviceAttributes,
                            &config,
                            &hDriver); // hDriver optional
    if (!NT_SUCCESS(status)) {
        DebugPrint( ("WdfDriverCreate failed with status 0x%x\n", status));

    }
    if (hDriver == INVALID_DRIVER_HANDLE || hDriver == 0x0) {
       DebugPrint(("WdfDriverCreate failed with status 0x%x\n", status));
       return 0;
    }
    DbgPrint(status);
    IO_deviceCreate(hDriver);
    if (!NT_SUCCESS(status)) {
        DebugPrint(("Failed to create device with status: 0x%x\n", status));
    }
    else {
        DebugPrint(("device created 0x%x\n", status));
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
    TRAP();
    return status; 
}

 NTSTATUS MouFilterDeviceCreate(
    IN WDFDRIVER        Driver,
    IN PWDFDEVICE_INIT  DeviceInit
) {
    NTSTATUS status = STATUS_SUCCESS;
    WDF_IO_QUEUE_CONFIG ioQueueConfig;
    WDFQUEUE ioReadQueue = NULL;
    WDFDEVICE hDevice;
    UNICODE_STRING deviceName;
    UNICODE_STRING symLinkName;
    WDF_OBJECT_ATTRIBUTES deviceAttributes;



    RtlInitUnicodeString(&deviceName, MY_MOUSE_FILTER_DEVICE_NAME);
    RtlInitUnicodeString(&symLinkName, MY_MOUSE_FILTER_SYMLINK_NAME);


    DebugPrint(("entered device create\n"));
    status = WdfDeviceInitAssignName(
        DeviceInit,
        &deviceName
    );

    WdfFdoInitSetFilter(DeviceInit);

    WdfDeviceInitSetDeviceType(DeviceInit, FILE_DEVICE_MOUSE);

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&deviceAttributes,
        DEVICE_EXTENSION);

    deviceAttributes.EvtCleanupCallback = DeviceCleanup;


    status = WdfDeviceCreate(&DeviceInit, &deviceAttributes, &hDevice);
    if (!NT_SUCCESS(status)) {
        DebugPrint(("WdfDeviceCreate failed with status code 0x%x\n", status));
        return status;
    }


    DEVICE_OBJECT* pdo = WdfDeviceWdmGetPhysicalDevice(hDevice);

    KEVENT ke;
    KeInitializeEvent(&ke, NotificationEvent, FALSE);
    IO_STATUS_BLOCK iosb = { 0 };
    PIRP Irp = IoBuildSynchronousFsdRequest(IRP_MJ_PNP,
        pdo, NULL, 0, NULL, &ke, &iosb);
    Irp->IoStatus.Status = STATUS_NOT_SUPPORTED;
    PIO_STACK_LOCATION stack = IoGetNextIrpStackLocation(Irp);
    stack->MinorFunction = IRP_MN_QUERY_ID;
    stack->Parameters.QueryId.IdType = BusQueryDeviceID;

    NTSTATUS tmp = IofCallDriver(pdo, Irp);

    if (tmp == STATUS_PENDING) {
        KeWaitForSingleObject(&ke, Executive, KernelMode, FALSE, NULL);
        tmp = iosb.Status;
    }

    PDEVICE_EXTENSION devExt = FilterGetData(hDevice); // Assuming MY_DRIVER_EXTENSION is the type

    if (NT_SUCCESS(tmp)) {
        WCHAR* id_ptr = (WCHAR*)(iosb.Information);
        wcsncpy(devExt->dev_id, id_ptr, 200);
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

    PAGED_CODE();

    

    // Initialize the device attributes
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&deviceAttributes, DEVICE_EXTENSION);

    // Create a framework device object
    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&ioQueueConfig, WdfIoQueueDispatchParallel);


    // Configure the default queue
    ioQueueConfig.EvtIoInternalDeviceControl = MouFilter_EvtIoInternalDeviceControl;
    // Create an I/O queue
    status = WdfIoQueueCreate(hDevice, &ioQueueConfig, WDF_NO_OBJECT_ATTRIBUTES, WDF_NO_HANDLE);
    if (!NT_SUCCESS(status)) {
        DebugPrint(("Default WdfIoQueueCreate failed 0x%x\n", status));
        WdfObjectDelete(hDevice); // Clean up the device object
        return status;
    }
    if (!NT_SUCCESS(status)) {
    }
    return status;
}
void MyEvtDriverUnload(_In_ WDFDRIVER Driver)
{
    UNREFERENCED_PARAMETER(Driver);

    // Perform cleanup tasks here
    DebugPrint(("Driver is unloading\n"));

    // For example, if you have any global resources allocated in DriverEntry,
    // free them here.

    // If you have created any WDFDEVICE objects, they will be deleted
    // automatically by the framework, so you typically don't need to
    // delete them here unless you need to perform specific actions.

    // Other cleanup tasks as needed...
}
//nothing needs to be done when the user shutsdown the computer
int EvtShutdownNotification(WDFDRIVER driver) {
    return 0;
}

void
DeviceCleanup(WDFOBJECT hDevice)
{
    PAGED_CODE();
    DebugPrint(("Removing device from collection\n"));

}
void EvtIoRead(WDFQUEUE Queue, WDFREQUEST Request, size_t Length)
{
    TRAP();
    // Handle read requests
}

void EvtIoWrite(WDFQUEUE Queue, WDFREQUEST Request, size_t Length)
{
    TRAP();
    // Handle write requests
}

void IO_deviceCreate(WDFDRIVER hDriver) {
    PWDFDEVICE_INIT             pInit = NULL;
    WDFDEVICE                   controlDevice = NULL;
    WDF_IO_QUEUE_CONFIG         ioQueueConfig;
    NTSTATUS                    status;
    WDFQUEUE                    queue;

    DebugPrint(("Creating Control Device\n"));

    //
    //
    // In order to create a control device, we first need to allocate a
    // WDFDEVICE_INIT structure and set all properties.
    //
    pInit = WdfControlDeviceInitAllocate(
        hDriver,
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

    status = WdfDeviceInitAssignName(pInit, &MY_MOUSE_FILTER_DEVICE_NAME);

    if (!NT_SUCCESS(status)) {
        goto Error;
    }

    status = WdfDeviceCreate(&pInit,
        WDF_NO_OBJECT_ATTRIBUTES,
        &controlDevice);

    if (!NT_SUCCESS(status)) {
        goto Error;
    }

    //
    // Create a symbolic link for the control object so that usermode can open
    // the device.
    //

    status = WdfDeviceCreateSymbolicLink(controlDevice, &MY_MOUSE_FILTER_SYMLINK_NAME);

    if (!NT_SUCCESS(status)) {
        goto Error;
    }

    //
    // Configure the default queue associated with the control device object
    // to be Serial so that request passed to RawaccelControl are serialized.
    //

    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&ioQueueConfig,
        WdfIoQueueDispatchSequential);

    ioQueueConfig.EvtIoDeviceControl = mouFiltrIoctl;

    //
    // Framework by default creates non-power managed queues for
    // filter drivers.
    //
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
        //
        // Release the reference on the newly created object, since
        // we couldn't initialize it.
        //
        WdfObjectDelete(controlDevice);
    }

    DebugPrint(("CreateControlDevice failed with status code 0x%x\n", status));

    return status;
}

void mouFiltrIoctl(
    WDFQUEUE         Queue,
    WDFREQUEST       Request,
    size_t           OutputBufferLength,
    size_t           InputBufferLength,
    ULONG            IoControlCode
)

{
    TRAP();

    switch (IoControlCode) {

    case CTL_CODE(0x888u, 0x888, METHOD_BUFFERED, FILE_ANY_ACCESS):
        TRAP();
    default:
        break;
    }
}
VOID
MouFilter_DispatchPassThrough(
    _In_ WDFREQUEST Request,
    _In_ WDFIOTARGET Target
    )
/*++
Routine Description:

    Passes a request on to the lower driver.


--*/
{
    DbgPrint("Started pass through");
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
        status = WdfRequestGetStatus (Request);
        DebugPrint( ("WdfRequestSend failed: 0x%x\n", status));
        WdfRequestComplete(Request, status);
    }

    return;
}           

VOID
MouFilter_EvtIoInternalDeviceControl(
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
    
    PDEVICE_EXTENSION           devExt;
    PCONNECT_DATA               connectData;
    PINTERNAL_I8042_HOOK_MOUSE  hookMouse;
    NTSTATUS                   status = STATUS_SUCCESS;
    WDFDEVICE                 hDevice;
    size_t                           length; 

    UNREFERENCED_PARAMETER(OutputBufferLength);
    UNREFERENCED_PARAMETER(InputBufferLength);

    PAGED_CODE();

    hDevice = WdfIoQueueGetDevice(Queue);
    devExt = FilterGetData(hDevice);
    TRAP();
    switch (IoControlCode) {

    //
    // Connect a mouse class device driver to the port driver.
    //
    case IOCTL_INTERNAL_MOUSE_CONNECT:
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
                            &connectData,
                            &length);
        if(!NT_SUCCESS(status)){
            DebugPrint(("WdfRequestRetrieveInputBuffer failed %x\n", status));
            break;
        }

        
        devExt->UpperConnectData = *connectData;

        //
        // Hook into the report chain.  Everytime a mouse packet is reported to
        // the system, MouFilter_ServiceCallback will be called
        //
        connectData->ClassDeviceObject = WdfDeviceWdmGetDeviceObject(hDevice);
        connectData->ClassService = MouFilter_ServiceCallback;
        TRAP();


        break;

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
        TRAP();

        break;

    //
    // Attach this driver to the initialization and byte processing of the 
    // i8042 (ie PS/2) mouse.  This is only necessary if you want to do PS/2
    // specific functions, otherwise hooking the CONNECT_DATA is sufficient
    //
    /*case IOCTL_INTERNAL_I8042_HOOK_MOUSE:
        TRAP();
          DebugPrint(("hook mouse received!\n"));
        
        // Get the input buffer from the request
        // (Parameters.DeviceIoControl.Type3InputBuffer)
        //
        status = WdfRequestRetrieveInputBuffer(Request,
                            sizeof(INTERNAL_I8042_HOOK_MOUSE),
                            &hookMouse,
                            &length);
        if(!NT_SUCCESS(status)){
            DebugPrint(("WdfRequestRetrieveInputBuffer failed %x\n", status));
            break;
        }
      
        //
        // Set isr routine and context and record any values from above this driver
        //
        devExt->UpperContext = hookMouse->Context;
        hookMouse->Context = (PVOID) devExt;

        if (hookMouse->IsrRoutine) {
            devExt->UpperIsrHook = hookMouse->IsrRoutine;
        }
        hookMouse->IsrRoutine = (PI8042_MOUSE_ISR) MouFilter_IsrHook;



        //
        // Store all of the other functions we might need in the future
        //
        devExt->IsrWritePort = hookMouse->IsrWritePort;
        devExt->CallContext = hookMouse->CallContext;
        devExt->QueueMousePacket = hookMouse->QueueMousePacket;

        status = STATUS_SUCCESS;
        TRAP();

        break;

    //
    // Might want to capture this in the future.  For now, then pass it down
    // the stack.  These queries must be successful for the RIT to communicate
    // with the mouse.
    //
    */
    case IOCTL_MOUSE_QUERY_ATTRIBUTES:
    default:
        break;
    }

    if (!NT_SUCCESS(status)) {
        WdfRequestComplete(Request, status);
        return ;
    }

    MouFilter_DispatchPassThrough(Request,WdfDeviceGetIoTarget(hDevice));
    TRAP();

}


BOOLEAN
MouFilter_IsrHook (
    PVOID         DeviceExtension, 
    PMOUSE_INPUT_DATA       CurrentInput, 
    POUTPUT_PACKET          CurrentOutput,
    UCHAR                   StatusByte,
    PUCHAR                  DataByte,
    PBOOLEAN                ContinueProcessing,
    PMOUSE_STATE            MouseState,
    PMOUSE_RESET_SUBSTATE   ResetSubState
)
/*++

Remarks:
    i8042prt specific code, if you are writing a packet only filter driver, you
    can remove this function

Arguments:

    DeviceExtension - Our context passed during IOCTL_INTERNAL_I8042_HOOK_MOUSE
    
    CurrentInput - Current input packet being formulated by processing all the
                    interrupts

    CurrentOutput - Current list of bytes being written to the mouse or the
                    i8042 port.
                    
    StatusByte    - Byte read from I/O port 60 when the interrupt occurred                                            
    
    DataByte      - Byte read from I/O port 64 when the interrupt occurred. 
                    This value can be modified and i8042prt will use this value
                    if ContinueProcessing is TRUE

    ContinueProcessing - If TRUE, i8042prt will proceed with normal processing of
                         the interrupt.  If FALSE, i8042prt will return from the
                         interrupt after this function returns.  Also, if FALSE,
                         it is this functions responsibilityt to report the input
                         packet via the function provided in the hook IOCTL or via
                         queueing a DPC within this driver and calling the
                         service callback function acquired from the connect IOCTL
                                             
Return Value:

    Status is returned.

  --+*/
{
    PDEVICE_EXTENSION   devExt;
    BOOLEAN             retVal = TRUE;

    devExt = DeviceExtension;
    
    if (devExt->UpperIsrHook) {
        retVal = (*devExt->UpperIsrHook) (devExt->UpperContext,
                            CurrentInput,
                            CurrentOutput,
                            StatusByte,
                            DataByte,
                            ContinueProcessing,
                            MouseState,
                            ResetSubState
            );

        if (!retVal || !(*ContinueProcessing)) {
            return retVal;
        }
    }

    *ContinueProcessing = TRUE;
    TRAP();

    return retVal;
}

    

VOID
MouFilter_ServiceCallback(
    IN PDEVICE_OBJECT DeviceObject,
    IN PMOUSE_INPUT_DATA InputDataStart,
    IN PMOUSE_INPUT_DATA InputDataEnd,
    IN OUT PULONG InputDataConsumed
    )
/*++

Routine Description:

    Called when there are mouse packets to report to the RIT.  You can do 
    anything you like to the packets.  For instance:
    
    o Drop a packet altogether
    o Mutate the contents of a packet 
    o Insert packets into the stream 
                    
Arguments:

    DeviceObject - Context passed during the connect IOCTL
    
    InputDataStart - First packet to be reported
    
    InputDataEnd - One past the last packet to be reported.  Total number of
                   packets is equal to InputDataEnd - InputDataStart
    
    InputDataConsumed - Set to the total number of packets consumed by the RIT
                        (via the function pointer we replaced in the connect
                        IOCTL)

Return Value:

    Status is returned.

--*/
{
    PDEVICE_EXTENSION   devExt;
    WDFDEVICE   hDevice;

    hDevice = WdfWdmDeviceGetWdfDeviceHandle(DeviceObject);

    devExt = FilterGetData(hDevice);
    //
    // UpperConnectData must be called at DISPATCH
    //
    (*(PSERVICE_CALLBACK_ROUTINE) devExt->UpperConnectData.ClassService)(
        devExt->UpperConnectData.ClassDeviceObject,
        InputDataStart,
        InputDataEnd,
        InputDataConsumed
        );
    TRAP();

} 

#pragma warning(pop)
