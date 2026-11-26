#include "./IddController.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <newdev.h>
#include <swdevice.h>
#include <strsafe.h>
#include <cfgmgr32.h>
#include <combaseapi.h>

#include "../RustDeskIddDriver/Public.h"

typedef struct DeviceCreateCallbackContext
{
    HANDLE hEvent;
    SW_DEVICE_LIFETIME* lifetime;
    HRESULT hrCreateResult;
} DeviceCreateCallbackContext;

const GUID GUID_DEVINTERFACE_IDD_DRIVER_DEVICE = \
{ 0x781EF630, 0x72B2, 0x11d2, { 0xB8, 0x52,  0x00,  0xC0,  0x4E,  0xAF,  0x52,  0x72 } };
//{781EF630-72B2-11d2-B852-00C04EAF5272}

BOOL g_printMsg = FALSE;
char g_lastMsg[1024];
const char* g_msgHeader = "RustDeskIdd: ";

VOID WINAPI
CreationCallback(
    _In_ HSWDEVICE hSwDevice,
    _In_ HRESULT hrCreateResult,
    _In_opt_ PVOID pContext,
    _In_opt_ PCWSTR pszDeviceInstanceId
);
// https://github.com/microsoft/Windows-driver-samples/blob/9f03207ae1e8df83325f067de84494ae55ab5e97/general/DCHU/osrfx2_DCHU_base/osrfx2_DCHU_testapp/testapp.c#L88
// Not a good way for this device, I don't not why. I'm not familiar with dirver.
BOOLEAN GetDevicePath(
    _In_ LPCGUID InterfaceGuid,
    _Out_writes_(BufLen) PTCHAR DevicePath,
    _In_ size_t BufLen
);

HANDLE DeviceOpenHandle();
VOID DeviceCloseHandle(HANDLE handle);

LPSTR formatErrorString(DWORD error);

void SetLastMsg(const char* format, ...)
{
    memset(g_lastMsg, 0, sizeof(g_lastMsg));
    memcpy_s(g_lastMsg, sizeof(g_lastMsg), g_msgHeader, strlen(g_msgHeader));

    va_list args;
    va_start(args, format);
    vsnprintf_s(
        g_lastMsg + strlen(g_msgHeader),
        sizeof(g_lastMsg) - strlen(g_msgHeader),
        _TRUNCATE,
        format,
        args);
    va_end(args);
}

const char* GetLastMsg()
{
    return g_lastMsg;
}

BOOL InstallUpdate(LPCTSTR fullInfPath, PBOOL rebootRequired)
{
    SetLastMsg("Success");

    // UpdateDriverForPlugAndPlayDevices may return FALSE while driver was successfully installed...
    if (FALSE == UpdateDriverForPlugAndPlayDevices(
        NULL,
        _T("RustDeskIddDriver"),    // match hardware id in the inf file
        fullInfPath,
        INSTALLFLAG_FORCE
            // | INSTALLFLAG_NONINTERACTIVE  // INSTALLFLAG_NONINTERACTIVE may cause error 0xe0000247
        ,
        rebootRequired
    ))
    {
        DWORD error = GetLastError();
        if (error != 0)
        {
            LPSTR errorString = formatErrorString(error);
            switch (error)
            {
            case 0x109:
                SetLastMsg("Failed InstallUpdate UpdateDriverForPlugAndPlayDevicesW, error: 0x%x, %s  Please try: Install the cert.\n", error, errorString == NULL ? "(NULL)\n" : errorString);
                break;
            case 0xe0000247:
                SetLastMsg("Failed InstallUpdate UpdateDriverForPlugAndPlayDevicesW, error: 0x%x, %s  Please try: \n1. Check the device manager and event viewer.\n2. Uninstall the driver, install the cert, then try again.\n", error, errorString == NULL ? "(NULL)\n" : errorString);
                break;
            default:
                SetLastMsg("Failed InstallUpdate UpdateDriverForPlugAndPlayDevicesW, error: 0x%x, %s  Please try: Check the device manager and event viewer.\n", error, errorString == NULL ? "(NULL)\n" : errorString);
                break;
            }
            if (errorString != NULL)
            {
                LocalFree(errorString);
            }
            if (g_printMsg)
            {
                printf(g_lastMsg);
            }
            return FALSE;
        }
    }

    return TRUE;
}

BOOL Uninstall(LPCTSTR fullInfPath, PBOOL rebootRequired)
{
    SetLastMsg("Success");

    if (FALSE == DiUninstallDriver(
        NULL,
        fullInfPath,
        0,
        rebootRequired
    ))
    {
        DWORD error = GetLastError();
        if (error != 0)
        {
            LPSTR errorString = formatErrorString(error);
            SetLastMsg("Failed Uninstall DiUninstallDriverW, error: 0x%x, %s", error, errorString == NULL ? "(NULL)\n" : errorString);
            if (errorString != NULL)
            {
                LocalFree(errorString);
            }
            if (g_printMsg)
            {
                printf(g_lastMsg);
            }
            return FALSE;
        }
    }

    return TRUE;
}

BOOL IsDeviceCreated(PBOOL created)
{
    SetLastMsg("Success");

    HDEVINFO hardwareDeviceInfo = SetupDiGetClassDevs(
        &GUID_DEVINTERFACE_IDD_DRIVER_DEVICE,
        NULL, // Define no enumerator (global)
        NULL, // Define no
        (DIGCF_PRESENT | // Only Devices present
            DIGCF_DEVICEINTERFACE)); // Function class devices.
    if (INVALID_HANDLE_VALUE == hardwareDeviceInfo)
    {
        DWORD error = GetLastError();
        LPSTR errorString = formatErrorString(error);
        SetLastMsg("Idd device: Failed IsDeviceCreated SetupDiGetClassDevs, error 0x%x (%s)\n", error, errorString == NULL ? "(NULL)\n" : errorString);
        if (errorString != NULL)
        {
            LocalFree(errorString);
        }
        if (g_printMsg)
        {
            printf(g_lastMsg);
        }
        return FALSE;
    }

    SP_DEVICE_INTERFACE_DATA            deviceInterfaceData;
    deviceInterfaceData.cbSize = sizeof(SP_DEVICE_INTERFACE_DATA);

    BOOL ret = FALSE;
    do
    {
        if (TRUE == SetupDiEnumDeviceInterfaces(hardwareDeviceInfo,
            0, // No care about specific PDOs
            &GUID_DEVINTERFACE_IDD_DRIVER_DEVICE,
            0, //
            &deviceInterfaceData))
        {
            *created = TRUE;
            ret = TRUE;
            break;
        }

        DWORD error = GetLastError();
        if (error == ERROR_NO_MORE_ITEMS)
        {
            *created = FALSE;
            ret = TRUE;
            break;
        }

        LPSTR errorString = formatErrorString(error);
        SetLastMsg("Idd device: Failed IsDeviceCreated SetupDiEnumDeviceInterfaces, error: 0x%x, %s", error, errorString == NULL ? "(NULL)\n" : errorString);
        if (errorString != NULL)
        {
            LocalFree(errorString);
        }
        if (g_printMsg)
        {
            printf(g_lastMsg);
        }
        ret = FALSE;
        break;

    } while (0);

    (VOID)SetupDiDestroyDeviceInfoList(hardwareDeviceInfo);
    return ret;
}

BOOL DeviceCreate(PHSWDEVICE hSwDevice)
{
    SW_DEVICE_LIFETIME lifetime = SWDeviceLifetimeHandle;
    return DeviceCreateWithLifetime(&lifetime, hSwDevice);
}

BOOL DeviceCreateWithLifetime(SW_DEVICE_LIFETIME *lifetime, PHSWDEVICE hSwDevice)
{
    SetLastMsg("Success");

    if (*hSwDevice != NULL)
    {
        SetLastMsg("Device handle is not NULL\n");
        return FALSE;
    }

    // No need to check if the device is previous created.
    // https://learn.microsoft.com/en-us/windows/win32/api/swdevice/nf-swdevice-swdevicesetlifetime
    // When a client app calls SwDeviceCreate for a software device that was previously marked for 
    // SwDeviceLifetimeParentPresent, SwDeviceCreate succeeds if there are no open software device handles for the device 
    // (only one handle can be open for a device). A client app can then regain control over a persistent software device 
    // for the purposes of updating properties and interfaces or changing the lifetime.
    //

    // create device
    HANDLE hEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
    if (hEvent == INVALID_HANDLE_VALUE || hEvent == NULL)
    {
        DWORD error = GetLastError();
        LPSTR errorString = formatErrorString(error);
        SetLastMsg("Failed DeviceCreate CreateEvent, error: 0x%x, %s", error, errorString == NULL ? "(NULL)\n" : errorString);
        if (errorString != NULL)
        {
            LocalFree(errorString);
        }
        if (g_printMsg)
        {
            printf(g_lastMsg);
        }

        return FALSE;
    }

    DeviceCreateCallbackContext callbackContext = { hEvent, lifetime, E_FAIL, };

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
        NULL,
        CreationCallback,
        &callbackContext,
        hSwDevice);
    if (FAILED(hr))
    {
        LPSTR errorString = formatErrorString((DWORD)hr);
        SetLastMsg("Failed DeviceCreate SwDeviceCreate, hresult 0x%lx, %s", hr, errorString == NULL ? "(NULL)\n" : errorString);
        if (errorString != NULL)
        {
            LocalFree(errorString);
        }
        if (g_printMsg)
        {
            printf(g_lastMsg);
        }

        return FALSE;
    }

    // Wait for callback to signal that the device has been created
    printf("Waiting for device to be created....\n");
    DWORD waitResult = WaitForSingleObject(hEvent, 10 * 1000);
    CloseHandle(hEvent);
    if (waitResult != WAIT_OBJECT_0)
    {
        DWORD error = 0;
        LPSTR errorString = NULL;
        switch (waitResult)
        {
            case WAIT_ABANDONED:
                SetLastMsg("Failed DeviceCreate wait for device creation 0x%d, WAIT_ABANDONED\n", waitResult);
                break;
            case WAIT_TIMEOUT:
                SetLastMsg("Failed DeviceCreate wait for device creation 0x%d, WAIT_TIMEOUT\n", waitResult);
                break;
            default:
                error = GetLastError();
                if (error != 0)
                {
                    errorString = formatErrorString(error);
                    SetLastMsg("Failed DeviceCreate wait for device creation, error: 0x%x, %s", error, errorString == NULL ? "(NULL)\n" : errorString);
                    if (errorString != NULL)
                    {
                        LocalFree(errorString);
                    }
                }
                break;
        }
        if (g_printMsg)
        {
            printf(g_lastMsg);
        }
        return FALSE;
    }

    if (SUCCEEDED(callbackContext.hrCreateResult))
    {
        // printf("Device created\n\n");
        return TRUE;
    }
    else
    {
        LPSTR errorString = formatErrorString((DWORD)callbackContext.hrCreateResult);
        SetLastMsg("Failed DeviceCreate SwDeviceCreate, hrCreateResult 0x%lx, %s", callbackContext.hrCreateResult, errorString == NULL ? "(NULL)\n" : errorString);
        if (errorString != NULL)
        {
            LocalFree(errorString);
        }
        return FALSE;
    }
}

VOID DeviceClose(HSWDEVICE hSwDevice)
{
    SetLastMsg("Success");

    if (hSwDevice != INVALID_HANDLE_VALUE && hSwDevice != NULL)
    {
        HRESULT result = SwDeviceSetLifetime(hSwDevice, SWDeviceLifetimeHandle);
        SwDeviceClose(hSwDevice);
    }
    else
    {
        BOOL created = TRUE;
        if (TRUE == IsDeviceCreated(&created))
        {
            if (created == FALSE)
            {
                return;
            }
        }
        else
        {
            // Try crete sw device, and close
        }

        HSWDEVICE hSwDevice2 = NULL;
        if (DeviceCreateWithLifetime(NULL, &hSwDevice2))
        {
            if (hSwDevice2 != NULL)
            {
                HRESULT result = SwDeviceSetLifetime(hSwDevice2, SWDeviceLifetimeHandle);
                SwDeviceClose(hSwDevice2);
            }
        }
        else
        {
            //
        }
    }
}

BOOL MonitorPlugIn(UINT index, UINT edid, INT retries)
{
    SetLastMsg("Success");

    if (retries < 0)
    {
        SetLastMsg("Failed MonitorPlugIn invalid tries %d\n", retries);
        if (g_printMsg)
        {
            printf(g_lastMsg);
        }
        return FALSE;
    }

    HANDLE hDevice = INVALID_HANDLE_VALUE;
    for (; retries >= 0; --retries)
    {
        hDevice = DeviceOpenHandle();
        if (hDevice != INVALID_HANDLE_VALUE && hDevice != NULL)
        {
            break;
        }
        Sleep(1000);
    }
    if (hDevice == INVALID_HANDLE_VALUE || hDevice == NULL)
    {
        return FALSE;
    }

    BOOL ret = FALSE;
    DWORD junk = 0;
    CtlPlugIn plugIn;
    plugIn.ConnectorIndex = index;
    plugIn.MonitorEDID = edid;
    HRESULT hr = CoCreateGuid(&plugIn.ContainerId);
    if (!SUCCEEDED(hr))
    {
        LPSTR errorString = formatErrorString((DWORD)hr);
        SetLastMsg("Failed MonitorPlugIn CoCreateGuid, hresult 0x%lx, %s", hr, errorString == NULL ? "(NULL)\n" : errorString);
        if (errorString != NULL)
        {
            LocalFree(errorString);
        }
        if (g_printMsg)
        {
            printf(g_lastMsg);
        }
        ret = FALSE;
    }
    else
    {
        ret = FALSE;
        for (; retries >= 0; --retries)
        {
            if (TRUE == DeviceIoControl(
                hDevice,
                IOCTL_CHANGER_IDD_PLUG_IN,
                &plugIn,                    // Ptr to InBuffer
                sizeof(CtlPlugIn),          // Length of InBuffer
                NULL,                       // Ptr to OutBuffer
                0,                          // Length of OutBuffer
                &junk,                      // BytesReturned
                0))                         // Ptr to Overlapped structure
            {
                ret = TRUE;
                break;
            }
        }
        if (ret == FALSE)
        {
            DWORD error = GetLastError();
            LPSTR errorString = formatErrorString(error);
            SetLastMsg("Failed MonitorPlugIn DeviceIoControl, error: 0x%x, %s", error, errorString == NULL ? "(NULL)\n" : errorString);
            if (errorString != NULL)
            {
                LocalFree(errorString);
            }
            if (g_printMsg)
            {
                printf(g_lastMsg);
            }
        }
    }

    DeviceCloseHandle(hDevice);
    return ret;
}

BOOL MonitorPlugOut(UINT index)
{
    SetLastMsg("Success");

    HANDLE hDevice = DeviceOpenHandle();
    if (hDevice == INVALID_HANDLE_VALUE || hDevice == NULL)
    {
        return FALSE;
    }

    BOOL ret = FALSE;
    DWORD junk = 0;
    CtlPlugOut plugOut;
    plugOut.ConnectorIndex = index;
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
        DWORD error = GetLastError();
        LPSTR errorString = formatErrorString(error);
        SetLastMsg("Failed MonitorPlugOut DeviceIoControl, error: 0x%x, %s", error, errorString == NULL ? "(NULL)\n" : errorString);
        if (errorString != NULL)
        {
            LocalFree(errorString);
        }
        if (g_printMsg)
        {
            printf(g_lastMsg);
        }
        ret = FALSE;
    }
    else
    {
        ret = TRUE;
    }

    DeviceCloseHandle(hDevice);
    return ret;
}

BOOL MonitorModesUpdate(UINT index, UINT modeCount, PMonitorMode modes)
{
    const DWORD bytes = CtlModeRequestBytes(modeCount);
    if (!bytes || !modes) {
        SetLastMsg("Invalid monitor mode list\n");
        return FALSE;
    }
    const HANDLE device = DeviceOpenHandle();
    if (device == INVALID_HANDLE_VALUE) return FALSE;
    PCtlMonitorModes packet = malloc(bytes);
    if (!packet) {
        DeviceCloseHandle(device);
        SetLastMsg("Cannot allocate monitor mode list\n");
        return FALSE;
    }
    packet->ConnectorIndex = index;
    packet->ModeCount = modeCount;
    for (UINT i = 0; i < modeCount; ++i) {
        packet->Modes[i].Width = modes[i].width;
        packet->Modes[i].Height = modes[i].height;
        packet->Modes[i].Sync = modes[i].sync;
    }
    DWORD used = 0;
    const BOOL ok = DeviceIoControl(device, IOCTL_CHANGER_IDD_UPDATE_MONITOR_MODE, packet, bytes,
                                    NULL, 0, &used, NULL);
    const DWORD error = ok ? ERROR_SUCCESS : GetLastError();
    free(packet);
    DeviceCloseHandle(device);
    if (!ok) {
        SetLastMsg("Updating monitor modes failed (Win32 %lu)\n", error);
        if (g_printMsg) fputs(g_lastMsg, stdout);
        SetLastError(error);
    }
    return ok;
}

VOID WINAPI
CreationCallback(
    _In_ HSWDEVICE hSwDevice,
    _In_ HRESULT hrCreateResult,
    _In_opt_ PVOID pContext,
    _In_opt_ PCWSTR pszDeviceInstanceId
)
{
    DeviceCreateCallbackContext* callbackContext = NULL;

    assert(pContext != NULL);
    if (pContext != NULL)
    {
        callbackContext = (DeviceCreateCallbackContext*)pContext;
        callbackContext->hrCreateResult = hrCreateResult;
        if (SUCCEEDED(hrCreateResult))
        {
            if (callbackContext->lifetime)
            {
                HRESULT result = SwDeviceSetLifetime(hSwDevice, *callbackContext->lifetime);
                if (FAILED(result))
                {
                    // TODO: debug log error here
                }
            }
        }

        assert(callbackContext->hEvent != NULL);
        if (callbackContext->hEvent != NULL)
        {
            SetEvent(callbackContext->hEvent);
        }
    }

    // printf("Idd device %ls created\n", pszDeviceInstanceId);
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
    if (cr != CR_SUCCESS)
    {
        SetLastMsg("Failed GetDevicePath 0x%x, retrieving device interface list size.\n", cr);
        if (g_printMsg)
        {
            printf(g_lastMsg);
        }

        goto clean0;
    }

    // CAUTION: BUG here. deviceInterfaceListLength is greater than 1, even device was not created...
    if (deviceInterfaceListLength <= 1)
    {
        SetLastMsg("Error: GetDevicePath No active device interfaces found. Is the sample driver loaded?\n");
        if (g_printMsg)
        {
            printf(g_lastMsg);
        }
        bRet = FALSE;
        goto clean0;
    }

    deviceInterfaceList = (PTSTR)malloc(deviceInterfaceListLength * sizeof(TCHAR));
    if (deviceInterfaceList == NULL)
    {
        SetLastMsg("Error GetDevicePath allocating memory for device interface list.\n");
        if (g_printMsg)
        {
            printf(g_lastMsg);
        }
        bRet = FALSE;
        goto clean0;
    }
    ZeroMemory(deviceInterfaceList, deviceInterfaceListLength * sizeof(TCHAR));

    for (int i = 0; i < 3 && _tcslen(deviceInterfaceList) == 0; i++)
    {
        // CAUTION: BUG here. deviceInterfaceList is NULL, even device was not created...
        cr = CM_Get_Device_Interface_List(
            (LPGUID)InterfaceGuid,
            NULL,
            deviceInterfaceList,
            deviceInterfaceListLength,
            CM_GET_DEVICE_INTERFACE_LIST_PRESENT);
        if (cr != CR_SUCCESS)
        {
            SetLastMsg("Error GetDevicePath 0x%x retrieving device interface list.\n", cr);
            if (g_printMsg)
            {
                printf(g_lastMsg);
            }
            goto clean0;
        }
        _tprintf(_T("get deviceInterfaceList %s\n"), deviceInterfaceList);
        Sleep(1000);
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
    if (FAILED(hr))
    {
        LPSTR errorString = formatErrorString((DWORD)hr);
        SetLastMsg("Failed GetDevicePath StringCchCopy, hresult 0x%lx, %s", hr, errorString == NULL ? "(NULL)\n" : errorString);
        if (errorString != NULL)
        {
            LocalFree(errorString);
        }
        if (g_printMsg)
        {
            printf(g_lastMsg);
        }
        bRet = FALSE;
        goto clean0;
    }

clean0:
    if (deviceInterfaceList != NULL)
    {
        free(deviceInterfaceList);
    }
    if (CR_SUCCESS != cr)
    {
        bRet = FALSE;
    }

    return bRet;
}

HDEVINFO OpenDisplayInterfaceSet(void)
{
    return SetupDiGetClassDevs(&GUID_DEVINTERFACE_IDD_DRIVER_DEVICE, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
}

DWORD OpenDisplayInterface(HDEVINFO devices, SP_DEVICE_INTERFACE_DATA* entry, HANDLE* handle)
{
    DWORD size = 0;
    if (!SetupDiGetDeviceInterfaceDetail(devices, entry, NULL, 0, &size, NULL)) {
        const DWORD error = GetLastError();
        if (error != ERROR_INSUFFICIENT_BUFFER) return error;
    }
    PSP_DEVICE_INTERFACE_DETAIL_DATA detail = malloc(size);
    if (!detail) return ERROR_OUTOFMEMORY;
    detail->cbSize = sizeof(*detail);
    DWORD error = ERROR_SUCCESS;
    if (SetupDiGetDeviceInterfaceDetail(devices, entry, detail, size, NULL, NULL)) {
        *handle = CreateFile(detail->DevicePath, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                             NULL, OPEN_EXISTING, 0, NULL);
        if (*handle == INVALID_HANDLE_VALUE) error = GetLastError();
    } else {
        error = GetLastError();
    }
    free(detail);
    return error;
}

HANDLE DeviceOpenHandle(void)
{
    HANDLE handle = INVALID_HANDLE_VALUE;
    HDEVINFO devices = OpenDisplayInterfaceSet();
    DWORD error;
    if (devices == INVALID_HANDLE_VALUE) {
        error = GetLastError();
    } else {
        SP_DEVICE_INTERFACE_DATA entry = {sizeof(entry)};
        error = SetupDiEnumDeviceInterfaces(devices, NULL, &GUID_DEVINTERFACE_IDD_DRIVER_DEVICE, 0, &entry) ?
            OpenDisplayInterface(devices, &entry, &handle) : GetLastError();
        SetupDiDestroyDeviceInfoList(devices);
    }
    if (error) {
        SetLastMsg("Opening display interface failed (Win32 %lu)\n", error);
        if (g_printMsg) fputs(g_lastMsg, stdout);
        SetLastError(error);
    } else {
        SetLastMsg("Success");
    }
    return handle;
}

VOID DeviceCloseHandle(HANDLE handle)
{
    if (handle != INVALID_HANDLE_VALUE && handle != NULL)
    {
        CloseHandle(handle);
    }
}

VOID SetPrintErrMsg(BOOL b)
{
    g_printMsg = (b == TRUE);
}

// Use en-us for simple, or we may need to handle wide string.
LPSTR formatErrorString(DWORD error)
{
    LPSTR errorString = NULL;
    FormatMessageA(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        NULL,
        error,
        MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US),
        (LPSTR)&errorString,
        0,
        NULL
    );
    return errorString;
}
