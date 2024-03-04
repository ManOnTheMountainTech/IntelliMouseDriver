#include "driver.h"
#include <Hidport.h>

#include "debug.h"
#include "SetBlack.h"

EVT_WDF_REQUEST_COMPLETION_ROUTINE  SetBlackCompletionRoutine;
EVT_WDF_WORKITEM                    SetBlackWorkItem;

NTSTATUS CreateWorkItemForIoTargetOpenDevice(WDFDEVICE device, CONST UNICODE_STRING& symLink)
    /*++

    Routine Description:

        Creates a WDF workitem to do the SetBlack() call after the driver
        stack has initialized.

    Arguments:

        Device - Handle to a pre-allocated WDF work item.

    Requirements:
        Must be synchronized to the device.

    --*/
{

    TRACE_FN_ENTRY
    
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

        // It's possible to get called twice. Been there, done that?
        if ((!pDeviceContext) || pDeviceContext->fSetBlackSuccess) {
            return STATUS_SUCCESS;
        }

        WDF_WORKITEM_CONFIG_INIT(&workItemConfig, SetBlackWorkItem);

        status = WdfWorkItemCreate(&workItemConfig,
            &workItemAttributes,
            &hWorkItem);

        if (!NT_SUCCESS(status)) {
            KdPrintEx((DPFLTR_IHVDRIVER_ID,
                DPFLTR_ERROR_LEVEL,
                "TailLight: Workitem creation failure 0x%x\n",
                status));
            return status; // Maybe better luck next time.
        }

        auto workItemContext = WdfObjectGet_SET_BLACK_WORKITEM_CONTEXT(hWorkItem);
        workItemContext->symLink = symLink;
    }

    WdfWorkItemEnqueue(hWorkItem);

    TRACE_FN_EXIT

    return status;
}

NTSTATUS RetrieveUsbHardwareIds(WDFIOTARGET target, USB_HARDWARE_ID_INFO& hwId) {

    WDF_OBJECT_ATTRIBUTES attributes = {};
    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    attributes.ParentObject = target; // auto-delete with target if all else fails
    NTSTATUS status = STATUS_UNSUCCESSFUL;

    WDFMEMORY memory = 0;
    status = WdfIoTargetAllocAndQueryTargetProperty(target,
        DevicePropertyHardwareID,
        NonPagedPoolNx,
        &attributes,
        &memory);

    if (!NT_SUCCESS(status)) {
        KdPrint(("TailLight: WdfIoTargetQueryTargetProperty DevicePropertyHardwareID failed 0x%x\n", status));
        return STATUS_UNSUCCESSFUL;
    }

    status = ExtractHardwareIds(memory, hwId);
    if (!NT_SUCCESS(status)) {
        goto ExitAndFree;
    }

ExitAndFree:
    NukeWdfHandle<WDFMEMORY>(memory);
    return status;
}

static NTSTATUS TryToOpenIoTarget(WDFIOTARGET target, 
    CONST UNICODE_STRING symLink) {

    PAGED_CODE();

    WDF_IO_TARGET_OPEN_PARAMS   openParams = {};
    WDF_IO_TARGET_OPEN_PARAMS_INIT_OPEN_BY_NAME(
        &openParams,
        &symLink,
        STANDARD_RIGHTS_ALL);

    openParams.ShareAccess = FILE_SHARE_WRITE | FILE_SHARE_READ;

    NTSTATUS status = STATUS_UNSUCCESSFUL;

    KdPrint(("Taillight: Opening Interface %wZ\n",
        symLink));

    // Ensure freed if fails.
    status = WdfIoTargetOpen(target, &openParams);
    if (!NT_SUCCESS(status)) {
        KdPrintEx((DPFLTR_IHVDRIVER_ID,
            DPFLTR_ERROR_LEVEL,
            "TailLight: 0x%x while opening the I/O target from worker thread\n",
            status));
    }

    return status;
}

void SetBlackCompletionRoutine(
    _In_ WDFREQUEST Request,
    _In_ WDFIOTARGET Target,
    _In_ PWDF_REQUEST_COMPLETION_PARAMS Params,
    _In_ WDFCONTEXT Context)
{
    UNREFERENCED_PARAMETER(Target);
    UNREFERENCED_PARAMETER(Params);
    UNREFERENCED_PARAMETER(Context);

    NTSTATUS status = STATUS_UNSUCCESSFUL;
    status = WdfRequestGetStatus(Request);
    KdPrint(("TailLight: %s WdfRequestSend status: 0x%x\n", __func__, status));

    // One-shot and top of stack, so delete and pray.
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
    TRACE_FN_ENTRY

    NTSTATUS status = STATUS_UNSUCCESSFUL;
    WDFDEVICE device = static_cast<WDFDEVICE>(WdfWorkItemGetParentObject(workItem));
    auto workItemContext = WdfObjectGet_SET_BLACK_WORKITEM_CONTEXT(workItem);
    //DEVICE_CONTEXT* pDeviceContext = WdfObjectGet_DEVICE_CONTEXT(device);

    status = SetBlackAsync(device, workItemContext->symLink);

    /*if (NT_SUCCESS(status)) {
    * TODO: Match hardware string based on interface.
        InterlockedIncrement((PLONG)(&pDeviceContext->fSetBlackSuccess));
    }*/

    NukeWdfHandle<WDFWORKITEM>(workItem);

    TRACE_FN_EXIT
}

NTSTATUS SetBlackAsync(WDFDEVICE device, 
    CONST UNICODE_STRING& symLink) {

    TRACE_FN_ENTRY

    PAGED_CODE();

    NTSTATUS        status = STATUS_UNSUCCESSFUL;
    WDFIOTARGET     hidTarget = NULL;

    DEVICE_CONTEXT* pDeviceContext = NULL;

    pDeviceContext =
        WdfObjectGet_DEVICE_CONTEXT(device);

    if (pDeviceContext == NULL) {
        return STATUS_DEVICE_NOT_READY;
    }
    {
        WDF_OBJECT_ATTRIBUTES attributes; 
        WDF_OBJECT_ATTRIBUTES_INIT(&attributes);

        // Ensure freed if fails.
        status = WdfIoTargetCreate(
            device,
            &attributes,
            &hidTarget);

        if (!NT_SUCCESS(status)) {
            KdPrintEx((DPFLTR_IHVDRIVER_ID,
                DPFLTR_ERROR_LEVEL,
                "TailLight: 0x%x while creating I/O target from worker thread\n",
                status));
            return status;
        }

        status = TryToOpenIoTarget(hidTarget, symLink);
    }

    if (NT_SUCCESS(status)) {

        WDFDEVICE targetDevice = WdfIoTargetGetDevice(hidTarget);
        PDEVICE_OBJECT targetDevObj = WdfDeviceWdmGetDeviceObject(targetDevice);
        if (targetDevObj != pDeviceContext->ourFDO) {
            status = STATUS_NOT_FOUND;
            goto ExitAndFree;
        }
        else {
            // TODO: Remove when working
            KdPrint(("Taillight: Target device match found\n"));
        }

        /*
        USB_HARDWARE_ID_INFO hwIds = {};

        status = RetrieveUsbHardwareIds(hidTarget, hwIds);
        if (!NT_SUCCESS(status)) {
            goto ExitAndFree;
        }

        auto& deviceHwId = pDeviceContext->hwId;
        if (!(deviceHwId.idVendor == hwIds.idVendor &&
            deviceHwId.idProduct == hwIds.idProduct)) {
            status = STATUS_NOT_FOUND;
            goto ExitAndFree;
        }*/

        WDFREQUEST  request = NULL;

        status = WdfRequestCreate(WDF_NO_OBJECT_ATTRIBUTES,
            hidTarget,
            &request);

        if (!NT_SUCCESS(status)) {
            KdPrint(("TailLight: WdfRequestCreate failed 0x%x\n", status));
            goto ExitAndFree;
        }

        WdfRequestSetCompletionRoutine(
            request,
            SetBlackCompletionRoutine,
            WDF_NO_CONTEXT);

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
            goto ExitAndFree;
        }

        // TODO: Wondering if we just cant cast pInBuffr as a TailLightReport
        RtlCopyMemory(pInBuffer, &report, sizeof(TailLightReport));

        // Format the request as write operation
        status = WdfIoTargetFormatRequestForIoctl(hidTarget,
            request,
            IOCTL_HID_SET_FEATURE,
            InBuffer,
            NULL,
            0,
            0);

        if (!NT_SUCCESS(status)) {
            KdPrint(("TailLight: WdfIoTargetFormatRequestForIoctl failed: 0x%x\n", status));
            goto ExitAndFree;
        }

        // Useful for finding the caller to IoCallDriver versus searching
        // through all of the threads.
        pDeviceContext->previousThread = KeGetCurrentThread();

        if (!WdfRequestSend(request, hidTarget, WDF_NO_SEND_OPTIONS)) {
            WdfObjectDelete(request);
            request = NULL;
        }
    }

ExitAndFree:
    NukeWdfHandle<WDFIOTARGET>(hidTarget);

    TRACE_FN_EXIT

    return status;
}