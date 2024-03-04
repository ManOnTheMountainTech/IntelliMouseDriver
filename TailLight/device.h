#pragma once
#define MSINTELLIMOUSE_USBINTERFACE5_PREFIX L"\\??\\HID#VID_045E&PID_082A&MI_01&Col05"

#define BEGIN_WITH(x) { \
        auto &_ = x;
#define END_WITH() }

typedef struct _HARDWARE_ID_INFO {
    USHORT idVendor;
    USHORT idProduct;
    UCHAR  bInterface;
} HARDWARE_ID_INFO, *PHARDWARE_ID_INFO;

typedef struct _USB_HARDWARE_ID_INFO : HARDWARE_ID_INFO {
    UCHAR  bInterface;
} USB_HARDWARE_ID_INFO, *PUSB_HARDWARE_ID_INFO;

/** Driver-specific struct for storing instance-specific data. */
typedef struct _DEVICE_CONTEXT {
    UNICODE_STRING PdoName;

    // Useful for debugging. This way less need to search all the system threads
    // for the callers stack after calling IoCallDriver.
    PKTHREAD       previousThread;
    BOOLEAN        fSetBlackSuccess;
    WDFWMIINSTANCE WmiInstance;
    USB_HARDWARE_ID_INFO hwId;
    PDEVICE_OBJECT pPDO;
    PDEVICE_OBJECT ourFDO;
} DEVICE_CONTEXT;


template<typename T> inline void NukeWdfHandle(T& handle) {
    if (handle) {
        WdfObjectDelete(handle);
        handle = 0;
    }
}

WDF_DECLARE_CONTEXT_TYPE(DEVICE_CONTEXT)

WDF_DECLARE_CONTEXT_TYPE(TailLightDeviceInformation)

EVT_WDF_DEVICE_CONTEXT_CLEANUP EvtDeviceContextCleanup;

NTSTATUS PnpNotifyDeviceInterfaceChange(
    _In_ PVOID NotificationStructure,
    _Inout_opt_ PVOID Context);

NTSTATUS ExtractHardwareIds(WDFMEMORY memory,
    USB_HARDWARE_ID_INFO& hwIds);