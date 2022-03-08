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
#include <newdev.h>
#include <tchar.h>

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
    wsprintfW(deviceInstanceId, L"\\\\.\\%s", pszDeviceInstanceId);
}

BOOLEAN
GetDevicePath(
    _In_ LPCGUID InterfaceGuid,
    _Out_writes_(BufLen) PTCHAR DevicePath,
    _In_ size_t BufLen
)
{
    CONFIGRET cr = CR_SUCCESS;
    PTSTR deviceInterfaceList = NULL;
    ULONG deviceInterfaceListLength = 0;
    PTSTR nextInterface;
    HRESULT hr = E_FAIL;
    BOOLEAN bRet = TRUE;

    cr = CM_Get_Device_Interface_List_Size(
        &deviceInterfaceListLength,
        (LPGUID)InterfaceGuid,
        NULL,
        CM_GET_DEVICE_INTERFACE_LIST_ALL_DEVICES);
    if (cr != CR_SUCCESS) {
        printf("Error 0x%x retrieving device interface list size.\n", cr);
        goto clean0;
    }

    if (deviceInterfaceListLength <= 1) {
        printf("Error: No active device interfaces found."
            " Is the sample driver loaded?\n");
        bRet = FALSE;
        goto clean0;
    }
    printf("deviceInterfaceListLength %u\n", deviceInterfaceListLength);

    deviceInterfaceList = (PTSTR)malloc(deviceInterfaceListLength * sizeof(TCHAR));
    if (deviceInterfaceList == NULL) {
        printf("Error allocating memory for device interface list.\n");
        bRet = FALSE;
        goto clean0;
    }
    ZeroMemory(deviceInterfaceList, deviceInterfaceListLength * sizeof(TCHAR));

    for (int i=0; i<3 && _tcslen(deviceInterfaceList) == 0; i++)
    {
        cr = CM_Get_Device_Interface_List(
            (LPGUID)InterfaceGuid,
            NULL,
            deviceInterfaceList,
            deviceInterfaceListLength,
            CM_GET_DEVICE_INTERFACE_LIST_PRESENT);
        if (cr != CR_SUCCESS) {
            printf("Error 0x%x retrieving device interface list.\n", cr);
            goto clean0;
        }
        _tprintf(_T("get deviceInterfaceList %s\n"), deviceInterfaceList);
    }

    nextInterface = deviceInterfaceList + _tcslen(deviceInterfaceList) + 1;
#ifdef UNICODE
    if (*nextInterface != UNICODE_NULL) {
#else
    if (*nextInterface != ANSI_NULL) {
#endif
        printf("Warning: More than one device interface instance found. \n"
            "Selecting first matching device.\n\n");
    }

    printf("begin copy device path\n");
    hr = StringCchCopy(DevicePath, BufLen, deviceInterfaceList);
    if (FAILED(hr)) {
        printf("Error: StringCchCopy failed with HRESULT 0x%x", hr);
        bRet = FALSE;
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

BOOLEAN GetDevicePath2(
    _In_ LPCGUID InterfaceGuid,
    _Out_writes_(BufLen) PTCHAR DevicePath,
    _In_ size_t BufLen
)
{
    HANDLE                              hDevice = INVALID_HANDLE_VALUE;
    PSP_DEVICE_INTERFACE_DETAIL_DATA    deviceInterfaceDetailData = NULL;
    ULONG                               predictedLength = 0;
    ULONG                               requiredLength = 0;
    ULONG                               bytes;
    HDEVINFO                            hardwareDeviceInfo;
    SP_DEVICE_INTERFACE_DATA            deviceInterfaceData;
    BOOLEAN                             status = FALSE;
    HRESULT                             hr;
    //
    // Open a handle to the device interface information set of all
    // present toaster bus enumerator interfaces.
    //

    hardwareDeviceInfo = SetupDiGetClassDevs(
        InterfaceGuid,
        NULL, // Define no enumerator (global)
        NULL, // Define no
        (DIGCF_PRESENT | // Only Devices present
            DIGCF_DEVICEINTERFACE)); // Function class devices.
    if (INVALID_HANDLE_VALUE == hardwareDeviceInfo)
    {
        printf("Idd device: SetupDiGetClassDevs failed, last error 0x%x\n", GetLastError());
        return FALSE;
    }

    deviceInterfaceData.cbSize = sizeof(SP_DEVICE_INTERFACE_DATA);

    if (!SetupDiEnumDeviceInterfaces(hardwareDeviceInfo,
        0, // No care about specific PDOs
        InterfaceGuid,
        0, //
        &deviceInterfaceData)) {
        printf("Idd device: SetupDiEnumDeviceInterfaces failed, last error 0x%x\n", GetLastError());
        goto Clean0;
    }

    //
    // Allocate a function class device data structure to receive the
    // information about this particular device.
    //

    SetupDiGetDeviceInterfaceDetail(
        hardwareDeviceInfo,
        &deviceInterfaceData,
        NULL, // probing so no output buffer yet
        0, // probing so output buffer length of zero
        &requiredLength,
        NULL);//not interested in the specific dev-node

    if (ERROR_INSUFFICIENT_BUFFER != GetLastError()) {
        printf("Idd device: SetupDiGetDeviceInterfaceDetail failed, last error 0x%x\n", GetLastError());
        goto Clean0;
    }


    predictedLength = requiredLength;

    deviceInterfaceDetailData = (PSP_DEVICE_INTERFACE_DETAIL_DATA)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
        predictedLength);

    if (deviceInterfaceDetailData) {
        deviceInterfaceDetailData->cbSize =
            sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA);
    }
    else {
        goto Clean0;
    }


    if (!SetupDiGetDeviceInterfaceDetail(
        hardwareDeviceInfo,
        &deviceInterfaceData,
        deviceInterfaceDetailData,
        predictedLength,
        &requiredLength,
        NULL)) {
        printf("Idd device: SetupDiGetDeviceInterfaceDetail failed, last error 0x%x\n", GetLastError());
        goto Clean1;
    }

    hr = StringCchCopy(DevicePath, BufLen, deviceInterfaceDetailData->DevicePath);
    if (FAILED(hr)) {
        status = FALSE;
        printf("Error: StringCchCopy failed with HRESULT 0x%x", hr);
        goto Clean1;
    }

Clean1:
    HeapFree(GetProcessHeap(), 0, deviceInterfaceDetailData);
Clean0:
    SetupDiDestroyDeviceInfoList(hardwareDeviceInfo);
    return status;
}

#define MAX_DEVPATH_LENGTH                       256

// TODOs:
// 1. Check if should install driver?
// 2. Open Driver device after installed.
// 3. Add monitor device if no monitors detected.
int __cdecl main(int argc, wchar_t *argv[])
{
    UNREFERENCED_PARAMETER(argc);
    UNREFERENCED_PARAMETER(argv);

    // install driver
    if (FALSE == UpdateDriverForPlugAndPlayDevices(
        NULL,
        _T("RustDeskIddDriver"),
        _T("C:\\Users\\cxl3\\Desktop\\Debug\\RustDeskIddDriver\\RustDeskIddDriver.inf"),
        INSTALLFLAG_FORCE,
        NULL
    ))
    {
        printf("Idd device: UpdateDriverForPlugAndPlayDevicesW failed, last error 0x%x\n", GetLastError());
    }
    else
    {
        printf("Idd device: UpdateDriverForPlugAndPlayDevicesW done\n");
    }


    // create device
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

    _getch();


    // open device for control
    TCHAR devicePath[MAX_DEVPATH_LENGTH] = { 0 };

    HANDLE hDevice = INVALID_HANDLE_VALUE;
    if (GetDevicePath(&GUID_DEVINTERFACE_IDD_DRIVER_DEVICE, devicePath, sizeof(devicePath) / sizeof(devicePath[0])))
    {
        _tprintf(_T("Idd device: try open %s\n"), devicePath);
        hDevice = CreateFile(
            devicePath,
            GENERIC_READ | GENERIC_WRITE,
            // FILE_SHARE_READ | FILE_SHARE_WRITE,
            0,
            NULL, // no SECURITY_ATTRIBUTES structure
            OPEN_EXISTING, // No special create flags
            0, // No special attributes
            NULL
        );
    }
    if (hDevice == INVALID_HANDLE_VALUE)
    {
        _tprintf(_T("Idd device: CreateFile %s failed, last error 0x%x\n"), devicePath, GetLastError());
    }
    
    // Now wait for user to indicate the device should be stopped
    printf("Press 'x' to exit and destory the software device\n");
    printf("Press '1','2','3' to plug in monitor (0,1,2)\n");
    printf("Press '4','5','6' to plug out monitor (0,1,2)\n");
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
        case '1':
        case '2':
        case '3':
            printf("begin plug in monitor\n");
            if (hDevice == INVALID_HANDLE_VALUE)
            {
                printf("device handler is not created, ignore plug in\n");
                break;
            }

            // plug in monitor
            CtlPlugIn plugIn;
            plugIn.ConnectorIndex = key - '1';
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
        case '4':
        case '5':
        case '6':
            printf("begin plug out monitor\n");
            if (hDevice == INVALID_HANDLE_VALUE)
            {
                printf("device handler is not created, ignore plug out\n");
                break;
            }

            // plug out monitor
            CtlPlugOut plugOut;
            plugOut.ConnectorIndex = key - '4';
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