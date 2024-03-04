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

NTSTATUS DeviceContext_StorePDOName(WDFDEVICE device, DEVICE_CONTEXT& dc) 
// initialize DEVICE_CONTEXT struct with PdoName
{
    // In order to send ioctls to our PDO, we have open to open it
    // by name so that we have a valid filehandle (fileobject).
    // When we send ioctls using the IoTarget, framework automatically 
    // sets the filobject in the stack location.
    WDF_OBJECT_ATTRIBUTES attributes = {};
    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    attributes.ParentObject = device; // auto-delete with device

    WDFMEMORY memory = 0;
    NTSTATUS status = WdfDeviceAllocAndQueryProperty(device,
        DevicePropertyPhysicalDeviceObjectName,
        NonPagedPoolNx,
        &attributes,
        &memory);

    if (!NT_SUCCESS(status)) {
        KdPrint(("TailLight: WdfDeviceAllocAndQueryProperty DevicePropertyPhysicalDeviceObjectName failed 0x%x\n", status));
        return STATUS_UNSUCCESSFUL;
    }

    // initialize pDeviceContext->PdoName based on memory
    size_t bufferLength = 0;
    dc.PdoName.Buffer = (WCHAR*)WdfMemoryGetBuffer(memory, &bufferLength);
    if (dc.PdoName.Buffer == NULL)
        return STATUS_UNSUCCESSFUL;

    dc.PdoName.MaximumLength = (USHORT)bufferLength;
    dc.PdoName.Length = (USHORT)bufferLength - sizeof(UNICODE_NULL);

    KdPrint(("TailLight: PdoName: %wZ\n", dc.PdoName)); // outputs "\Device\00000083

    return STATUS_SUCCESS;
}

NTSTATUS ExtractDigitFromHardwareId(PCHAR pHardwareId,
    USHORT offset, 
    UCHAR numDigits,
    CONST USHORT& resultantDigits) {
    /*++
    Routine Description:

    Arguments:
        us - string to fixup
        numDigits - Number of digits to be extracted but must be less than 4
        resultantDigits - Where to place the extracted digits
    --*/
    NT_ASSERT(pHardwareId > (PCHAR)65535);
 
    pHardwareId = pHardwareId + offset;

    static const UCHAR digitBuffSize = 4;
    NT_ASSERT(digitBuffSize >= numDigits);
    CHAR digitBuff[digitBuffSize] = {};

    NTSTATUS status = STATUS_SUCCESS;
    status = RtlStringCbCopyA(digitBuff, numDigits, pHardwareId);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    status = RtlCharToInteger(digitBuff, 16, (PULONG)resultantDigits);
    return status;
}

NTSTATUS ExtractHardwareIds(WDFMEMORY memory, 
    USB_HARDWARE_ID_INFO& hwIds) {
   
    NTSTATUS status = STATUS_SUCCESS;
    PCHAR  pHardwareId = nullptr;
    USHORT hardwareIdLen = 0;

    pHardwareId = (PCHAR)WdfMemoryGetBuffer(memory, 
        (SIZE_T*)&hardwareIdLen);
    if (pHardwareId == nullptr)
        return STATUS_UNSUCCESSFUL;

    KdPrint(("TailLight: Hardware ID %s\n", pHardwareId));


    static const USHORT vendor_offset = sizeof("HID\\VID_") - sizeof(CHAR);
    status = ExtractDigitFromHardwareId(pHardwareId, vendor_offset, 4, hwIds.idVendor);
    if (!NT_SUCCESS(status)) {
        goto ExitAndFree;
    }

    static const ULONG device_offset = sizeof("045E&PID_") - sizeof(CHAR);
    status = ExtractDigitFromHardwareId(pHardwareId, device_offset, 4, hwIds.idProduct);
    if (!NT_SUCCESS(status)) {
        goto ExitAndFree;
    }

    static const ULONG interface_offset = sizeof("082A&REV_0095&MI_") - sizeof(CHAR);
    status = ExtractDigitFromHardwareId(pHardwareId, device_offset, 2, hwIds.bInterface);
    if (!NT_SUCCESS(status)) {
        goto ExitAndFree;
    }

    ExitAndFree:
    return status;
}

NTSTATUS RetrieveUsbHardwareIds(WDFDEVICE device, USB_HARDWARE_ID_INFO& hwId)
{
    WDF_OBJECT_ATTRIBUTES attributes = {};
    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    attributes.ParentObject = device; // auto-delete with device
    NTSTATUS status = STATUS_UNSUCCESSFUL;

    WDFMEMORY memory = 0;
    status = WdfDeviceAllocAndQueryProperty(device,
        DevicePropertyHardwareID,
        NonPagedPoolNx,
        &attributes,
        &memory);
    if (!NT_SUCCESS(status)) {
        KdPrint(("TailLight: WdfDeviceAllocAndQueryProperty DevicePropertyHardwareID failed 0x%x\n", status));
        return STATUS_UNSUCCESSFUL;
    }

    UNICODE_STRING hardwareId = {};

    status = ExtractHardwareIds(memory, hwId);
    if (!NT_SUCCESS(status)) {
        goto ExitAndFree;
    }

    ExitAndFree:
    NukeWdfHandle(memory);

    return STATUS_SUCCESS;
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
    auto pdo = WdfFdoInitWdmGetPhysicalDevice(DeviceInit);

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
            KdPrint(("TailLight: WdfDeviceCreate, Error %x\n", status));
            return status;
        }
    }

    // Driver Framework always zero initializes an objects context memory
    DEVICE_CONTEXT* pDeviceContext = WdfObjectGet_DEVICE_CONTEXT(device);
    pDeviceContext->pPDO = pdo;
    pDeviceContext->ourFDO = WdfDeviceWdmGetDeviceObject(device);

    status = DeviceContext_StorePDOName(device, *pDeviceContext);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    /*status = RetrieveUsbHardwareIds(device, pDeviceContext->hwId);
    if (!NT_SUCCESS(status)) {
        return status;
    }*/

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
