#include "./IddController.h"

#if (NTDDI_VERSION < NTDDI_WIN8)

#include "../RustDeskIddDriver/Public.h"


#ifdef __cplusplus
extern "C" {
#endif

void SetLastMsg(const char* format, ...);
BOOL ReturnUnsupported(char* call);

BOOL g_printMsg;
char g_lastMsg[1024];
const char* g_msgHeader;

#ifdef __cplusplus
}
#endif


BOOL InstallUpdate(LPCTSTR fullInfPath, PBOOL rebootRequired)
{
    return ReturnUnsupported(__FUNCTION__);
}

BOOL Uninstall(LPCTSTR fullInfPath, PBOOL rebootRequired)
{
    return ReturnUnsupported(__FUNCTION__);
}

BOOL IsDeviceCreated(PBOOL created)
{
    return ReturnUnsupported(__FUNCTION__);
}

BOOL DeviceCreate(PHSWDEVICE hSwDevice)
{
    return ReturnUnsupported(__FUNCTION__);
}

VOID DeviceClose(HSWDEVICE hSwDevice)
{
    ReturnUnsupported(__FUNCTION__);
}

BOOL MonitorPlugIn(UINT index, UINT edid, INT retries)
{
    return ReturnUnsupported(__FUNCTION__);
}

BOOL MonitorPlugOut(UINT index)
{
    return ReturnUnsupported(__FUNCTION__);
}

BOOL MonitorModesUpdate(UINT index, UINT modeCount, PMonitorMode modes)
{
    return ReturnUnsupported(__FUNCTION__);
}

#endif // NTDDI_VERSION
