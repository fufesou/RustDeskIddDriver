#pragma once

#include <winioctl.h>
#include <guiddef.h>

#define IOCTL_CHANGER_IDD_PLUG_IN CTL_CODE(IOCTL_CHANGER_BASE, 0x1001, METHOD_BUFFERED, FILE_READ_ACCESS | FILE_WRITE_ACCESS)
#define IOCTL_CHANGER_IDD_PLUG_OUT CTL_CODE(IOCTL_CHANGER_BASE, 0x1002, METHOD_BUFFERED, FILE_READ_ACCESS | FILE_WRITE_ACCESS)


#define STATUS_ERROR_ADAPTER_NOT_INIT (3 << 30) + 11
//#define STATUS_ERROR_IO_CTL_GET_INPUT (3 << 30) + 21
//#define STATUS_ERROR_IO_CTL_GET_OUTPUT (3 << 30) + 22
#define STATUS_ERROR_MONITOR_EXISTS (3 << 30) + 51
#define STATUS_ERROR_MONITOR_NOT_EXISTS (3 << 30) + 52

typedef struct _CtlPlugIn {
    UINT ConnectorIndex;
    GUID ContainerId;
} CtlPlugIn, *PCtlPlugIn;

typedef struct _CtlPlugOut {
    UINT ConnectorIndex;
} CtlPlugOut, *PCtlPlugOut;
