#include <iostream>
#include <vector>

#include <windows.h>
#include <swdevice.h>
#include <conio.h>
#include <wrl.h>
#include <ioapiset.h>
#include <SetupAPI.h>
#include <cfgmgr32.h>
#include <strsafe.h>

#include "../RustDeskIddDriver/Public.h"

const GUID GUID_DEVINTERFACE_IDD_DRIVER_DEVICE = \
{ 0x781EF630, 0x72B2, 0x11d2, { 0xB8, 0x52,  0x00,  0xC0,  0x4E,  0xAF,  0x52,  0x72 } };
//{781EF630-72B2-11d2-B852-00C04EAF5272}

// https://stackoverflow.com/questions/67164846/createfile-fails-unless-i-disable-enable-my-device

//#define IDD_DRIVER_NAME             L"RustDeskIddDriver"
//#define IDD_DRIVER_NAME_WITH_EXT    L"RustDeskIddDriver.sys"

// #define IDD_NT_DEVICE_NAME          L"\\Device\\RustDeskIddDriver"
// #define IDD_DOS_DEVICES_LINK_NAME   L"\\DosDevices\\RustDeskIddDriver"
//#define IDD_WIN32_DEVICE_NAME       L"\\\\.\\RustDeskIddDriver"

WCHAR deviceInstanceId[256] = {0};

VOID WINAPI
CreationCallback(
    _In_ HSWDEVICE hSwDevice,
    _In_ HRESULT hrCreateResult,
    _In_opt_ PVOID pContext,
    _In_opt_ PCWSTR pszDeviceInstanceId
    )
{
    HANDLE hEvent = *(HANDLE*) pContext;

    SetEvent(hEvent);
    UNREFERENCED_PARAMETER(hSwDevice);
    UNREFERENCED_PARAMETER(hrCreateResult);
    printf("Idd device %ls created\n", pszDeviceInstanceId);
    wsprintfW(deviceInstanceId, L"\\\\.\\GLOBAL??\\%s", pszDeviceInstanceId);
}

BOOLEAN
GetDevicePath(
    _In_ LPCGUID InterfaceGuid,
    _Out_writes_(BufLen) PWCHAR DevicePath,
    _In_ size_t BufLen
)
{
    CONFIGRET cr = CR_SUCCESS;
    PWSTR deviceInterfaceList = NULL;
    ULONG deviceInterfaceListLength = 0;
    PWSTR nextInterface;
    HRESULT hr = E_FAIL;
    BOOLEAN bRet = TRUE;

    cr = CM_Get_Device_Interface_List_SizeW(
        &deviceInterfaceListLength,
        (LPGUID)InterfaceGuid,
        NULL,
        CM_GET_DEVICE_INTERFACE_LIST_PRESENT);
    if (cr != CR_SUCCESS) {
        printf("Error 0x%x retrieving device interface list size.\n", cr);
        goto clean0;
    }

    if (deviceInterfaceListLength <= 1) {
        bRet = FALSE;
        printf("Error: No active device interfaces found.\n"
            " Is the sample driver loaded?\n");
        goto clean0;
    }

    deviceInterfaceList = (PWSTR)malloc(deviceInterfaceListLength * sizeof(WCHAR));
    if (deviceInterfaceList == NULL) {
        printf("Error allocating memory for device interface list.\n");
        goto clean0;
    }
    ZeroMemory(deviceInterfaceList, deviceInterfaceListLength * sizeof(WCHAR));

    cr = CM_Get_Device_Interface_ListW(
        (LPGUID)InterfaceGuid,
        NULL,
        deviceInterfaceList,
        deviceInterfaceListLength,
        CM_GET_DEVICE_INTERFACE_LIST_PRESENT);
    if (cr != CR_SUCCESS) {
        printf("Error 0x%x retrieving device interface list.\n", cr);
        goto clean0;
    }

    nextInterface = deviceInterfaceList + wcslen(deviceInterfaceList) + 1;
    if (*nextInterface != UNICODE_NULL) {
        printf("Warning: More than one device interface instance found. \n"
            "Selecting first matching device.\n\n");
    }

    hr = StringCchCopyW(DevicePath, BufLen, deviceInterfaceList);
    if (FAILED(hr)) {
        bRet = FALSE;
        printf("Error: StringCchCopy failed with HRESULT 0x%x", hr);
        goto clean0;
    }

clean0:
    if (deviceInterfaceList != NULL) {
        free(deviceInterfaceList);
    }
    if (CR_SUCCESS != cr) {
        bRet = FALSE;
    }

    return bRet;
}

#define MAX_DEVPATH_LENGTH                       256

int __cdecl main(int argc, wchar_t *argv[])
{
    UNREFERENCED_PARAMETER(argc);
    UNREFERENCED_PARAMETER(argv);

    WCHAR devicePath[MAX_DEVPATH_LENGTH] = { 0 };

    HANDLE hEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    HSWDEVICE hSwDevice;
    SW_DEVICE_CREATE_INFO createInfo = { 0 };
    PCWSTR description = L"RustDesk Idd Driver";

    // These match the Pnp id's in the inf file so OS will load the driver when the device is created    
    PCWSTR instanceId = L"RustDeskIddDriver";
    PCWSTR hardwareIds = L"RustDeskIddDriver\0\0";
    PCWSTR compatibleIds = L"RustDeskIddDriver\0\0";

    createInfo.cbSize = sizeof(createInfo);
    createInfo.pszzCompatibleIds = compatibleIds;
    createInfo.pszInstanceId = instanceId;
    createInfo.pszzHardwareIds = hardwareIds;
    createInfo.pszDeviceDescription = description;

    createInfo.CapabilityFlags = SWDeviceCapabilitiesRemovable |
                                 SWDeviceCapabilitiesSilentInstall |
                                 SWDeviceCapabilitiesDriverRequired;

    // Create the device
    HRESULT hr = SwDeviceCreate(L"RustDeskIddDriver",
                                L"HTREE\\ROOT\\0",
                                &createInfo,
                                0,
                                nullptr,
                                CreationCallback,
                                &hEvent,
                                &hSwDevice);
    if (FAILED(hr))
    {
        printf("SwDeviceCreate failed with 0x%lx\n", hr);
        return 1;
    }

    // Wait for callback to signal that the device has been created
    printf("Waiting for device to be created....\n");
    DWORD waitResult = WaitForSingleObject(hEvent, 10*1000);
    if (waitResult != WAIT_OBJECT_0)
    {
        printf("Wait for device creation failed\n");
        return 1;
    }
    printf("Device created\n\n");

    HANDLE hDevice = INVALID_HANDLE_VALUE;
    if (GetDevicePath(&GUID_DEVINTERFACE_IDD_DRIVER_DEVICE, devicePath, sizeof(devicePath) / sizeof(devicePath[0])))
    {
        hDevice = CreateFileW(
            devicePath,
            GENERIC_READ | GENERIC_WRITE,
            0,
            NULL,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL,
            NULL
        );
    }
    if (hDevice == INVALID_HANDLE_VALUE)
    {
        wprintf(L"Idd device: CreateFile %s failed, last error 0x%x\n", devicePath, GetLastError());
    }
    
    // Now wait for user to indicate the device should be stopped
    printf("Press 'x' to exit and destory the software device\n");
    printf("Press 'i' to plug in monitor\n");
    printf("Press 'o' to plug out monitor\n");
    bool bExit = false;
    do
    {
        // Wait for key press
        int key = _getch();
        DWORD junk = 0;

        switch (key)
        {
        case 'x':
        case 'X':
            bExit = true;
            break;
        case 'i':
        case 'I':
            printf("begin plug in monitor\n");
            if (hDevice == INVALID_HANDLE_VALUE)
            {
                printf("device handler is not created, ignore plug in\n");
                break;
            }

            // plug in monitor
            CtlPlugIn plugIn;
            plugIn.ConnectorIndex = 0;
            CoCreateGuid(&plugIn.ContainerId);
            if (!DeviceIoControl(
                hDevice,
                IOCTL_CHANGER_IDD_PLUG_IN,
                &plugIn,                    // Ptr to InBuffer
                sizeof(CtlPlugIn),          // Length of InBuffer
                NULL,                       // Ptr to OutBuffer
                0,                          // Length of OutBuffer
                &junk,                      // BytesReturned
                0))                         // Ptr to Overlapped structure
            {
                DWORD code = GetLastError();
                printf("DeviceIoControl failed with error 0x%x\n", code);
            }
            else
            {
                printf("plug in monitor done\n");
            }
            break;
        case 'o':
        case 'O':
            printf("begin plug out monitor\n");
            if (hDevice == INVALID_HANDLE_VALUE)
            {
                printf("device handler is not created, ignore plug out\n");
                break;
            }

            // plug out monitor
            CtlPlugOut plugOut;
            plugOut.ConnectorIndex = 0;
            if (!DeviceIoControl(
                hDevice,
                IOCTL_CHANGER_IDD_PLUG_OUT,
                &plugOut,               // Ptr to InBuffer
                sizeof(CtlPlugOut),     // Length of InBuffer
                NULL,                   // Ptr to OutBuffer
                0,                      // Length of OutBuffer
                &junk,                  // BytesReturned
                0))                     // Ptr to Overlapped structure
            {
                DWORD code = GetLastError();
                printf("DeviceIoControl failed with error 0x%x\n", code);
            }
            {
                printf("plug out monitor done\n");
            }
            break;
        default:
            break;
        }

    }while (!bExit);

    if (hDevice != INVALID_HANDLE_VALUE)
    {
        CloseHandle(hDevice);
    }
    
    // Stop the device, this will cause the sample to be unloaded
    SwDeviceClose(hSwDevice);

    return 0;
}