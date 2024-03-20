#pragma once

/** Driver-specific struct for storing instance-specific data. */
// InterlockXxx writes to data that is at least 16 bits long. Since the
// default alignment is 8 bytes and there is no practical reason to save
// 4 bytes we'll make everything at least 8 bytes.
struct DEVICE_CONTEXT {
    UNICODE_STRING PdoName;
    ULONG          fulSetBlackSuccess;
    WDFWMIINSTANCE WmiInstance;
    PVOID          pnpDevInterfaceChangedHandle;
};
WDF_DECLARE_CONTEXT_TYPE(DEVICE_CONTEXT)

WDF_DECLARE_CONTEXT_TYPE(TailLightDeviceInformation)

EVT_WDF_DEVICE_CONTEXT_CLEANUP EvtDeviceContextCleanup;

NTSTATUS PnpNotifyDeviceInterfaceChange(
    _In_ PVOID NotificationStructure,
    _Inout_opt_ PVOID Context);

UNICODE_STRING GetTargetPropertyString(WDFIOTARGET target, DEVICE_REGISTRY_PROPERTY DeviceProperty);