#include "driver.h"
#include <Hidport.h>

#include "SetBlack.h"

EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL EvtIoDeviceControlFilter;

NTSTATUS PnpNotifyDeviceInterfaceChange(
    _In_ PVOID pvNotificationStructure,
    _Inout_opt_ PVOID pvContext) {

    //KdPrint(("TailLight: PnpNotifyDeviceInterfaceChange enter\n"));
    NT_ASSERTMSG("WDFDEVICE not passed in!", pvContext);

    if (pvNotificationStructure == NULL) {
        return STATUS_SUCCESS;
    }

    PDEVICE_INTERFACE_CHANGE_NOTIFICATION pDevInterface =
        (PDEVICE_INTERFACE_CHANGE_NOTIFICATION)pvNotificationStructure;

    ASSERT(IsEqualGUID(*(_GUID*)&(pDevInterface->InterfaceClassGuid),
        *(_GUID*)&GUID_DEVINTERFACE_HID));

    {     
        // Assumption: Device will arrive before removal.
        if (IsEqualGUID(*(LPGUID)&(pDevInterface->Event),
            *(LPGUID)&GUID_DEVICE_INTERFACE_ARRIVAL)) {

            // Opening a device may trigger PnP operations. Ensure that either a
            // timer or a work item is used when opening up a device.
            // Refer to p356 of Oney and IoGetDeviceObjectPointer.
            //
            // NOTE: It is possible for us to get blocked waiting for a system
            // thread. One solution would be to use a timer that spawns a
            // system thread on timeout vs. a wait loop.


            // Always must return success, so ignore result.
            CreateWorkItemForIoTargetOpenDevice((WDFDEVICE)pvContext,
                *pDevInterface->SymbolicLinkName);
        }
    }

    return STATUS_SUCCESS;
}


NTSTATUS EvtSelfManagedIoInit(WDFDEVICE device) {

    WDFDRIVER driver = WdfDeviceGetDriver(device);
    PDRIVER_CONTEXT driverContext = WdfObjectGet_DRIVER_CONTEXT(driver);

    //TRACE_FN_ENTRY;

    NTSTATUS status = STATUS_SUCCESS;

    status = IoRegisterPlugPlayNotification(
        EventCategoryDeviceInterfaceChange,
        PNPNOTIFY_DEVICE_INTERFACE_INCLUDE_EXISTING_INTERFACES,
        (PVOID)&GUID_DEVINTERFACE_HID,
        WdfDriverWdmGetDriverObject(driver),
        PnpNotifyDeviceInterfaceChange,
        (PVOID)device,
        &driverContext->pnpDevInterfaceChangedHandle
    );

    //TRACE_FN_EXIT;

    return status;
}

NTSTATUS AllocatePDOName(WDFDEVICE device, UNICODE_STRING& pdoName) 
// initialize DEVICE_CONTEXT struct with PdoName
{
    // In order to send ioctls to our PDO, we have open to open it
    // by name so that we have a valid filehandle (fileobject).
    // When we send ioctls using the IoTarget, framework automatically 
    // sets the filobject in the stack location.
    WDF_OBJECT_ATTRIBUTES attributes = {};
    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    attributes.ParentObject = device; // auto-delete with device
    NTSTATUS status = STATUS_SUCCESS;

    WDFMEMORY memory = 0;
    status = WdfDeviceAllocAndQueryProperty(device,
        DevicePropertyPhysicalDeviceObjectName,
        NonPagedPoolNx,
        &attributes,
        &memory);

    if (!NT_SUCCESS(status)) {
        KdPrint(("TailLight: WdfDeviceAllocAndQueryProperty"
            "DevicePropertyPhysicalDeviceObjectName failed 0x%x\n", status));
        return status;
    }

    // initialize pDeviceContext->PdoName based on memory
    size_t bufferLength = 0;
    pdoName.Buffer = (WCHAR*)WdfMemoryGetBuffer(memory, &bufferLength);
    if (pdoName.Buffer == NULL)
        return STATUS_UNSUCCESSFUL;

    pdoName.MaximumLength = (USHORT)bufferLength;
    pdoName.Length = (USHORT)bufferLength - sizeof(UNICODE_NULL);

    KdPrint(("TailLight: PdoName: %wZ\n", pdoName)); // outputs "\Device\00000083

    return status;
}

NTSTATUS AllocateHwIdString(WDFDEVICE device, UNICODE_STRING& hwId)
{
    WDF_OBJECT_ATTRIBUTES attributes = {};
    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    attributes.ParentObject = device; // auto-delete with device
    NTSTATUS status = STATUS_SUCCESS;

    WDFMEMORY memory = 0;
    status = WdfDeviceAllocAndQueryProperty(device,
        DevicePropertyHardwareID,
        NonPagedPoolNx,
        &attributes,
        &memory);
    if (!NT_SUCCESS(status)) {
        KdPrint(("TailLight: WdfDeviceAllocAndQueryProperty"
            "DevicePropertyHardwareID failed 0x%x\n", 
            status));
        return status;
    }

    hwId.Buffer = (PWCHAR)WdfMemoryGetBuffer(memory, (SIZE_T*) & (hwId.MaximumLength));
    if (hwId.Buffer == nullptr)
        return STATUS_UNSUCCESSFUL;

    hwId.Length = (USHORT)hwId.MaximumLength - sizeof(UNICODE_NULL);

    KdPrint(("TailLight: Device hardware ID %S\n", hwId.Buffer));

    return status;
}

// TODO: Consider passing in the property and making a generic property retrieval
NTSTATUS AllocateHwIdString(WDFIOTARGET target, UNICODE_STRING& hwId)
/*++
Routine Description:
    Creates from non-paged memory a MULTI_SZ unicode hardware Id of the I/O 
    target and sets the default object lifetime to the IO target.

Arguments:
    WDFIOTARGET - A WDF IO target

    STRING - Where to place the allocated string.
--*/
{
    WDF_OBJECT_ATTRIBUTES attributes = {};
    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    attributes.ParentObject = target; // auto-delete with device
    NTSTATUS status = STATUS_SUCCESS;

    WDFMEMORY memory = 0;
    status = WdfIoTargetAllocAndQueryTargetProperty(target,
        DevicePropertyHardwareID,
        NonPagedPoolNx,
        &attributes,
        &memory);
    if (!NT_SUCCESS(status)) {
        KdPrint(("TailLight: WdfDeviceAllocAndQueryProperty"
            "DevicePropertyHardwareID failed 0x%x\n",
            status));
        return status;
    }

    hwId.Buffer = (PWCHAR)WdfMemoryGetBuffer(memory, (SIZE_T*) & (hwId.MaximumLength));
    if (hwId.Buffer == nullptr)
        return STATUS_UNSUCCESSFUL;

    hwId.Length = (USHORT)hwId.MaximumLength - sizeof(UNICODE_NULL);

    KdPrint(("TailLight: IO target Hardware ID %S\n", hwId.Buffer));

    return status;
}

NTSTATUS EvtDriverDeviceAdd(_In_ WDFDRIVER Driver, _Inout_ PWDFDEVICE_INIT DeviceInit)
/*++
Routine Description:
    EvtDriverDeviceAdd is called by the framework in response to AddDevice
    call from the PnP manager. We create and initialize a device object to
    represent to be part of the device stack as a filter.

Arguments:
    Driver - Handle to a framework driver object created in DriverEntry

    DeviceInit - Pointer to a framework-allocated WDFDEVICE_INIT structure.
--*/    
{
    UNREFERENCED_PARAMETER(Driver);

    NTSTATUS status = STATUS_UNSUCCESSFUL;

    // Configure the device as a filter driver
    WdfFdoInitSetFilter(DeviceInit);

    {
        // register PnP callbacks (must be done before WdfDeviceCreate)
        WDF_PNPPOWER_EVENT_CALLBACKS PnpPowerCallbacks;
        WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&PnpPowerCallbacks);
        PnpPowerCallbacks.EvtDeviceSelfManagedIoInit = EvtSelfManagedIoInit;
        WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &PnpPowerCallbacks);
    }

    WDFDEVICE device = 0;
    {
        // create device
        WDF_OBJECT_ATTRIBUTES attributes = {};
        WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, DEVICE_CONTEXT);

        status = WdfDeviceCreate(&DeviceInit, &attributes, &device);
        if (!NT_SUCCESS(status)) {
            KdPrint(("TailLight: WdfDeviceCreate, Error 0x%x\n", status));
            return status;
        }
    }

    // Driver Framework always zero initializes an objects context memory
    DEVICE_CONTEXT* pDeviceContext = WdfObjectGet_DEVICE_CONTEXT(device);

    status = AllocatePDOName(device, pDeviceContext->PdoName);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    status = AllocateHwIdString(device, pDeviceContext->OurHardwareId);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    {
        // create queue for filtering
        WDF_IO_QUEUE_CONFIG queueConfig = {};
        WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&queueConfig, WdfIoQueueDispatchParallel); // don't synchronize
        //queueConfig.EvtIoRead  // pass-through read requests 
        //queueConfig.EvtIoWrite // pass-through write requests
        queueConfig.EvtIoDeviceControl = EvtIoDeviceControlFilter; // filter IOCTL requests

        WDFQUEUE queue = 0; // auto-deleted when parent is deleted
        status = WdfIoQueueCreate(device, &queueConfig, WDF_NO_OBJECT_ATTRIBUTES, &queue);

        if (!NT_SUCCESS(status)) {
            KdPrint(("TailLight: WdfIoQueueCreate failed 0x%x\n", status));
            return status;
        }
    }

    // Initialize WMI provider
    status = WmiInitialize(device);
    if (!NT_SUCCESS(status)) {
        KdPrint(("TailLight: Error initializing WMI 0x%x\n", status));
    }

    return status;
}


VOID EvtIoDeviceControlFilter(
    _In_  WDFQUEUE          Queue,
    _In_  WDFREQUEST        Request,
    _In_  size_t            OutputBufferLength,
    _In_  size_t            InputBufferLength,
    _In_  ULONG             IoControlCode
)
/*++
Routine Description:
    This event callback function is called when the driver receives an

    (KMDF) IOCTL_HID_Xxx code when handling IRP_MJ_INTERNAL_DEVICE_CONTROL
    (UMDF) IOCTL_HID_Xxx, IOCTL_UMDF_HID_Xxx when handling IRP_MJ_DEVICE_CONTROL

Arguments:
    Queue - A handle to the queue object that is associated with the I/O request

    Request - A handle to a framework request object.

    OutputBufferLength - The length, in bytes, of the request's output buffer,
            if an output buffer is available.

    InputBufferLength - The length, in bytes, of the request's input buffer, if
            an input buffer is available.

    IoControlCode - The driver or system defined IOCTL associated with the request
--*/
{
    UNREFERENCED_PARAMETER(OutputBufferLength);

    //KdPrint(("TailLight: EvtIoDeviceControl (IoControlCode=0x%x, InputBufferLength=%Iu)\n", IoControlCode, InputBufferLength));

    WDFDEVICE device = WdfIoQueueGetDevice(Queue);

    NTSTATUS status = STATUS_SUCCESS; //unhandled
    switch (IoControlCode) {
      case IOCTL_HID_SET_FEATURE: // 0xb0191
        status = SetFeatureFilter(device, Request, InputBufferLength);
        break;
    }
    // No NT_SUCCESS(status) check here since we don't want to fail blocked calls

    // Forward the request down the driver stack
    WDF_REQUEST_SEND_OPTIONS options = {};
    WDF_REQUEST_SEND_OPTIONS_INIT(&options, WDF_REQUEST_SEND_OPTION_SEND_AND_FORGET);

    BOOLEAN ret = WdfRequestSend(Request, WdfDeviceGetIoTarget(device), &options);
    if (ret == FALSE) {
        status = WdfRequestGetStatus(Request);
        KdPrint(("TailLight: WdfRequestSend failed with status: 0x%x\n", status));
        WdfRequestComplete(Request, status);
    }
}
