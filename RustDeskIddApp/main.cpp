#include <iostream>
#include <vector>

#include <windows.h>
#include <swdevice.h>
#include <conio.h>
#include <wrl.h>
#include <ioapiset.h>

#include "../RustDeskIddDriver/IOCTL.h"

#define IDD_DRIVER_NAME             L"RustDeskIddDriver"
#define IDD_DRIVER_NAME_WITH_EXT    L"RustDeskIddDriver.sys"

// #define IDD_NT_DEVICE_NAME          L"\\Device\\RustDeskIddDriver"
// #define IDD_DOS_DEVICES_LINK_NAME   L"\\DosDevices\\RustDeskIddDriver"
#define IDD_WIN32_DEVICE_NAME       L"\\\\.\\RustDeskIddDriver"

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
    UNREFERENCED_PARAMETER(pszDeviceInstanceId);
}

int __cdecl main(int argc, wchar_t *argv[])
{
    UNREFERENCED_PARAMETER(argc);
    UNREFERENCED_PARAMETER(argv);

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
    if (hDevice == INVALID_HANDLE_VALUE) {
        hDevice = CreateFileW(
            IDD_WIN32_DEVICE_NAME,
            GENERIC_READ | GENERIC_WRITE,
            0,
            NULL,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL,
            NULL
        );

        if (hDevice == INVALID_HANDLE_VALUE)
        {
            printf("Idd device: CreateFile(%ls) failed, last error 0x%x\n", IDD_WIN32_DEVICE_NAME, GetLastError());
        }
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
            if (hDevice == INVALID_HANDLE_VALUE)
            {
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
            break;
        case 'o':
        case 'O':
            if (hDevice == INVALID_HANDLE_VALUE)
            {
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