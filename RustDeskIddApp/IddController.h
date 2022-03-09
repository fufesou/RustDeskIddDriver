#pragma once
#include <Windows.h>
#include <tchar.h>
#include <swdevice.h>


BOOL InstallUpdate(LPCTSTR fullInfPath);

BOOL DeviceCreate(HSWDEVICE* hSwDevice);
VOID DeviceClose(HSWDEVICE hSwDevice);

BOOL MonitorPlugIn(UINT index);
BOOL MonitorPlugOut(UINT index);

const char* GetLastMsg();
