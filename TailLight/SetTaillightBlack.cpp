#include "driver.h"
#include <Hidport.h>

#include "SetBlack.h"

EVT_WDF_REQUEST_COMPLETION_ROUTINE  SetBlackCompletionRoutine;
EVT_WDF_WORKITEM                    SetBlackWorkItem;


NTSTATUS CreateWorkItemForIoTargetOpenDevice(WDFDEVICE device, 
    CONST UNICODE_STRING& symLink)
/*++
    Routine Description:

        Creates a WDF workitem to do the SetBlack() call after the driver
        stack has initialized.

    Arguments:

        Device - Handle to a pre-allocated WDF work item.

--*/
{    
    WDFWORKITEM hWorkItem = 0;
    NTSTATUS status = STATUS_PNP_DRIVER_CONFIGURATION_INCOMPLETE;
    {
        WDF_WORKITEM_CONFIG        workItemConfig;
        WDF_OBJECT_ATTRIBUTES      workItemAttributes;
        WDF_OBJECT_ATTRIBUTES_INIT(&workItemAttributes);
        WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&workItemAttributes,
            SET_BLACK_WORKITEM_CONTEXT);
        workItemAttributes.ParentObject = device;

        DEVICE_CONTEXT* pDeviceContext = WdfObjectGet_DEVICE_CONTEXT(device);

        // It's possible to get called twice. Please refer to 
        // https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdm/nf-wdm-ioregisterplugplaynotification,
        // under "Remarks"
        if (pDeviceContext->fulSetBlackSuccess) {
            return STATUS_SUCCESS;
        }

        WDF_WORKITEM_CONFIG_INIT(&workItemConfig, SetBlackWorkItem);

        status = WdfWorkItemCreate(&workItemConfig,
            &workItemAttributes,
            &hWorkItem);

        if (!NT_SUCCESS(status)) {
            KdPrint(("TailLight: Workitem creation failure 0x%x\n",
                status));
            return status; // Maybe better luck next time.
        }

        auto workItemContext = WdfObjectGet_SET_BLACK_WORKITEM_CONTEXT(hWorkItem);
        workItemContext->symLink = symLink;
    }

    WdfWorkItemEnqueue(hWorkItem);

    return status;
}


static NTSTATUS TryToOpenIoTarget(WDFIOTARGET target, 
    CONST UNICODE_STRING symLink)
/*++
    Routine Description:
        
        Attempts to open up the IO target.
     
    Arguments:
        
        target - an IO target to host the device and IO stack formatting.
       
        symLink - an opague representation of a symlink.
    
    Return value:
        
        A success code if everything works. If a step fails, failure.
--*/
{

    PAGED_CODE();

    WDF_IO_TARGET_OPEN_PARAMS   openParams = {};
    WDF_IO_TARGET_OPEN_PARAMS_INIT_OPEN_BY_NAME(
        &openParams,
        &symLink,
        STANDARD_RIGHTS_ALL);

    openParams.ShareAccess = FILE_SHARE_WRITE | FILE_SHARE_READ;

    NTSTATUS status = STATUS_UNSUCCESSFUL;

    // Ensure freed if fails.
    status = WdfIoTargetOpen(target, &openParams);
    if (!NT_SUCCESS(status)) {
        KdPrint(("TailLight: 0x%x while opening the I/O target from worker thread\n",
            status));
    }

    return status;
}


void SetBlackCompletionRoutine(
    _In_ WDFREQUEST Request,
    _In_ WDFIOTARGET Target,
    _In_ PWDF_REQUEST_COMPLETION_PARAMS Params,
    _In_ WDFCONTEXT Context)
 /*++
    Routine Description:

        Determines if the set black request was understood by the driver 
        represented by the IO target.

        If the set black request was understood, we set a flag so that we
        don't cause unneeded bus traffic.

    Arguments:

        Context - The address of a ULONG flag. The flag is set to:
            TRUE = The call succeed.
            FALSE = The call failed.

        TODO: Check completion params.
--*/
{
    UNREFERENCED_PARAMETER(Target);
    UNREFERENCED_PARAMETER(Params);

    NTSTATUS status = STATUS_UNSUCCESSFUL;
    status = WdfRequestGetStatus(Request);
    KdPrint(("TailLight: %s WdfRequestGetStatus: 0x%x\n", __func__, status));

    if (NT_SUCCESS(status)) {
        InterlockedExchange((PLONG)Context, (LONG)TRUE);
    }

    // One-shot and top of stack, so delete.
    WdfObjectDelete(Request);
}


VOID SetBlackWorkItem(
    WDFWORKITEM workItem)
 /*++
    Routine Description:

        Creates a WDF workitem to do the SetBlack() call after the driver
        stack has initialized.

    Arguments:

        workItem - Handle to a pre-allocated WDF work item.
--*/
{
    NTSTATUS status = STATUS_UNSUCCESSFUL;
    WDFDEVICE device = static_cast<WDFDEVICE>(WdfWorkItemGetParentObject(workItem));
    auto workItemContext = WdfObjectGet_SET_BLACK_WORKITEM_CONTEXT(workItem);

    status = SetBlackAsync(device, workItemContext->symLink);

    WdfObjectDelete(workItem);
}


NTSTATUS SetBlackAsync(WDFDEVICE device, 
    CONST UNICODE_STRING& symLink) 
/*++
    Routine Description:

        Sets the taillight to black using an asynchronous request send. This
        code is called from multiple system worker threads and is thus
        multithreaded.
   
   Operation:
        
        A device that corresponds to the symLink is opened. The device is then
        asked for its name. Since we know that our device stack is
        capable of setting the taillight to black, we see if the created PDO's
        name matches our PDO's name. If they match, then a request is sent down
        to the PDO to set the taillight black.

    Arguments:

        symLink& - an opaque name representing a symbolic link.

    Returns:

        STATUS_SUCCESS: A set black light request was successfully sent down.
        
        STATUS_NOT_FOUND: The symbolic link's representation does not represent
            a device that is known to be able to set the Pro IntelliMouse's
            taillight to black.
        
        Any other NTSTATUS error code: A step didn't work or the provided
        symbolic link representation does not represent a device.   
--*/
{
    PAGED_CODE();

    NTSTATUS            status = STATUS_UNSUCCESSFUL;
    WDFIOTARGET_Wrap    target; // easier to cut and paste if generic

    DEVICE_CONTEXT* pDeviceContext = NULL;

    pDeviceContext =
        WdfObjectGet_DEVICE_CONTEXT(device);

    {
        WDF_OBJECT_ATTRIBUTES attributes; 
        WDF_OBJECT_ATTRIBUTES_INIT(&attributes);

        // Ensure freed if fails.
        status = WdfIoTargetCreate(
            device,
            &attributes,
            &target);

        if (!NT_SUCCESS(status)) {
            KdPrint(("TailLight: 0x%x while creating I/O target from worker thread\n",
                status));
            return status;
        }

        // We're warned not to process a symlink path name returned from 
        // IoRegisterDeviceInterface so we need to use a unique device property
        //  - the device object name - to see if we can send the IRP down with
        //  minimal bus traffic.
        status = TryToOpenIoTarget(target, symLink);
    }

    if (NT_SUCCESS(status)) {     
        UNICODE_STRING theirPDOName = {};

        // The PDO name will be attached to the IO target
        // and thus deleted at the end of the function.
        theirPDOName = GetTargetPropertyString(target,
            DevicePropertyPhysicalDeviceObjectName);

        // A remote request might work but we don't know which drivers
        // have this capability so we just focus on our own stack.
        if (theirPDOName.MaximumLength > 0) {
            if (!RtlEqualUnicodeString(&pDeviceContext->PdoName,
                &theirPDOName,
                TRUE)) {
                KdPrint(("TailLight: %s: Device %wZ not known to control the taillight so failing\n",
                    __func__,
                    theirPDOName));
                return STATUS_NOT_FOUND;
            }
        }

        WDFREQUEST  request = NULL;
        status = WdfRequestCreate(WDF_NO_OBJECT_ATTRIBUTES,
            target,
            &request);

        if (!NT_SUCCESS(status)) {
            KdPrint(("TailLight: WdfRequestCreate failed 0x%x\n", status));
            return status;
        }

        WdfRequestSetCompletionRoutine(
            request,
            SetBlackCompletionRoutine,
            &pDeviceContext->fulSetBlackSuccess);

        TailLightReport  report = {};
        report.Blue = 0x0;
        report.Green = 0x0;
        report.Red = 0x0;

        // Set up a WDF memory object for the IOCTL request
        WDF_OBJECT_ATTRIBUTES mem_attrib = {};
        WDF_OBJECT_ATTRIBUTES_INIT(&mem_attrib);
        mem_attrib.ParentObject = request; // auto-delete with request*/

        WDFMEMORY InBuffer = 0;
        BYTE* pInBuffer = nullptr;

        status = WdfMemoryCreate(&mem_attrib,
            NonPagedPoolNx,
            POOL_TAG,
            sizeof(TailLightReport),
            &InBuffer,
            (void**)&pInBuffer);

        if (!NT_SUCCESS(status)) {
            KdPrint(("TailLight: WdfMemoryCreate failed: 0x%x\n", status));
            WdfObjectDelete(request);
            request = 0;
            return status;
        }

        *(TailLightReport*)pInBuffer = report;

        // Format the request as a write operation
        status = WdfIoTargetFormatRequestForIoctl(target,
            request,
            IOCTL_HID_SET_FEATURE,
            InBuffer,
            NULL,
            0,
            0);

        if (!NT_SUCCESS(status)) {
            KdPrint(("TailLight: WdfIoTargetFormatRequestForIoctl failed: 0x%x\n", status));
            WdfObjectDelete(request);
            request = 0;
            return status;
        }

        // Rundown any threads that may still be executing after we succeed in
        // setting the taillight to black. This way we hit the bus one time.
        if (pDeviceContext->fulSetBlackSuccess) {
            KdPrint(("Taillight: Taillight already set to black. failing\n"));
            WdfObjectDelete(request);
            request = 0;
            return STATUS_NOT_FOUND;
        }

        if (!WdfRequestSend(request, target, WDF_NO_SEND_OPTIONS)) {
            WdfObjectDelete(request);
            request = 0;
            status = STATUS_UNSUCCESSFUL;
        }
    }

    return status;
}