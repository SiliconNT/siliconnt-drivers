/*
 * ans: Storport miniport for the internal disk controller of Apple silicon Macs.
 *
 * The device is ACPI\SLNT0001, the ANS0 node the SiliconNT firmware builds from the
 * Apple device tree. Its _CRS gives three memory ranges (NVMe, coprocessor and
 * mailbox, SART) and two interrupts (controller, mailbox "message received").
 *
 * This is the skeleton. It claims the device, starts, and reports no disks:
 *   - it never maps or touches the controller's registers;
 *   - it never starts the controller's coprocessor;
 *   - it does not connect the interrupts (see AnsFindAdapter);
 *   - every request for a target or unit is answered "no device".
 * On a Mac this device is the internal disk of a machine its owner uses every day.
 */

#include <miniport.h>   /* base types and INTERFACE_TYPE; storport.h does not include it */
#include <storport.h>

/* Nothing to keep yet; Storport wants a non-empty device extension. */
typedef struct _ANS_ADAPTER {
    ULONG Unused;
} ANS_ADAPTER, *PANS_ADAPTER;

/* _CRS of ANS0: NVMe, coprocessor and mailbox, SART. */
#define ANS_MEMORY_RANGES 3

sp_DRIVER_INITIALIZE DriverEntry;
HW_FIND_ADAPTER AnsFindAdapter;
HW_INITIALIZE AnsInitialize;
HW_STARTIO AnsStartIo;
HW_INTERRUPT AnsInterrupt;
HW_RESET_BUS AnsResetBus;
HW_ADAPTER_CONTROL AnsAdapterControl;

ULONG
AnsFindAdapter(
    _In_ PVOID DeviceExtension,
    _In_ PVOID HwContext,
    _In_ PVOID BusInformation,
    _In_z_ PCHAR ArgumentString,
    _Inout_ PPORT_CONFIGURATION_INFORMATION ConfigInfo,
    _In_ PBOOLEAN Reserved3
    )
{
    UNREFERENCED_PARAMETER(DeviceExtension);
    UNREFERENCED_PARAMETER(HwContext);
    UNREFERENCED_PARAMETER(BusInformation);
    UNREFERENCED_PARAMETER(ArgumentString);
    UNREFERENCED_PARAMETER(Reserved3);

    /*
     * No hardware resources are used, so Storport must not connect the interrupts.
     * The skeleton cannot clear the controller's level-triggered line without
     * touching its registers: if the line were asserted, connecting it would leave
     * an interrupt nobody can acknowledge. Storport skips connecting interrupts
     * and setting up DMA when VirtualDevice is set here (SiliconNT findings/0065).
     */
    ConfigInfo->VirtualDevice = TRUE;

    ConfigInfo->NumberOfBuses = 1;
    ConfigInfo->MaximumNumberOfTargets = 1;
    ConfigInfo->MaximumNumberOfLogicalUnits = 1;

    return SP_RETURN_FOUND;
}

BOOLEAN
AnsInitialize(
    _In_ PVOID DeviceExtension
    )
{
    UNREFERENCED_PARAMETER(DeviceExtension);
    return TRUE;
}

BOOLEAN
AnsStartIo(
    _In_ PVOID DeviceExtension,
    _In_ PSCSI_REQUEST_BLOCK Srb
    )
{
    switch (Srb->Function) {
    case SRB_FUNCTION_EXECUTE_SCSI:
        /* No disks: every target and unit is absent. */
        Srb->SrbStatus = SRB_STATUS_NO_DEVICE;
        break;
    default:
        Srb->SrbStatus = SRB_STATUS_INVALID_REQUEST;
        break;
    }

    StorPortNotification(RequestComplete, DeviceExtension, Srb);
    return TRUE;
}

/*
 * Never called: the interrupts are not connected (AnsFindAdapter). Storport's
 * documentation requires the entry point, so it is here and claims nothing.
 */
BOOLEAN
AnsInterrupt(
    _In_ PVOID DeviceExtension
    )
{
    UNREFERENCED_PARAMETER(DeviceExtension);
    return FALSE;
}

BOOLEAN
AnsResetBus(
    _In_ PVOID DeviceExtension,
    _In_ ULONG PathId
    )
{
    UNREFERENCED_PARAMETER(DeviceExtension);
    UNREFERENCED_PARAMETER(PathId);
    return TRUE;
}

SCSI_ADAPTER_CONTROL_STATUS
AnsAdapterControl(
    _In_ PVOID DeviceExtension,
    _In_ SCSI_ADAPTER_CONTROL_TYPE ControlType,
    _In_ PVOID Parameters
    )
{
    PSCSI_SUPPORTED_CONTROL_TYPE_LIST list;

    UNREFERENCED_PARAMETER(DeviceExtension);

    switch (ControlType) {
    case ScsiQuerySupportedControlTypes:
        list = (PSCSI_SUPPORTED_CONTROL_TYPE_LIST)Parameters;
        if (list->MaxControlType > ScsiQuerySupportedControlTypes) {
            list->SupportedTypeList[ScsiQuerySupportedControlTypes] = TRUE;
        }
        if (list->MaxControlType > ScsiStopAdapter) {
            list->SupportedTypeList[ScsiStopAdapter] = TRUE;
        }
        if (list->MaxControlType > ScsiRestartAdapter) {
            list->SupportedTypeList[ScsiRestartAdapter] = TRUE;
        }
        return ScsiAdapterControlSuccess;

    case ScsiStopAdapter:
    case ScsiRestartAdapter:
        /* Nothing was started on the device, so there is nothing to stop or restart. */
        return ScsiAdapterControlSuccess;

    default:
        return ScsiAdapterControlUnsuccessful;
    }
}

ULONG
DriverEntry(
    _In_ PVOID DriverObject,
    _In_ PVOID RegistryPath
    )
{
    HW_INITIALIZATION_DATA init;

    RtlZeroMemory(&init, sizeof(init));
    init.HwInitializationDataSize = sizeof(init);

    /* The device is enumerated by ACPI, as the in-box storufs.sys registers. */
    init.AdapterInterfaceType = ACPIBus;

    /* The field is a PVOID because virtual miniports put another type of routine here. */
#pragma warning(suppress: 4152)
    init.HwFindAdapter = AnsFindAdapter;
    init.HwInitialize = AnsInitialize;
    init.HwStartIo = AnsStartIo;
    init.HwInterrupt = AnsInterrupt;
    init.HwResetBus = AnsResetBus;
    init.HwAdapterControl = AnsAdapterControl;

    init.DeviceExtensionSize = sizeof(ANS_ADAPTER);
    init.NumberOfAccessRanges = ANS_MEMORY_RANGES;
    init.MapBuffers = STOR_MAP_NO_BUFFERS;

    return StorPortInitialize(DriverObject, RegistryPath, &init, NULL);
}
