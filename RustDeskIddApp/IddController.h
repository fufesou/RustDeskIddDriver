#pragma once
#include <Windows.h>
#include <tchar.h>
#include <swdevice.h>

/**
 * @brief Install or Update RustDeskIddDriver.
 *
 * @param fullInfPath [in] Full path of the driver inf file.
 *
 * @return TRUE/FALSE. If FALSE returned, error message can be retrieved by GetLastMsg()
 * 
 * @see GetLastMsg#GetLastMsg
 */
BOOL InstallUpdate(LPCTSTR fullInfPath);

/**
 * @brief Check if RustDeskIddDriver device is created before.
 *        The driver device(adapter) should be single instance.
 * 
 * @param created [out] Indicate whether device is created before.
 *
 * @return TRUE/FALSE. If FALSE returned, error message can be retrieved by GetLastMsg()
 *
 */
BOOL DeviceCreated(BOOL* created);

/**
 * @brief Create device.
 *        Only one device should be created.
 *        If device is installed ealier, this function returns FALSE.
 * 
 * @param hSwDevice [out] Handler of software device, used by SwDeviceCreate(). Should be **NULL**.
 *
 * @return TRUE/FALSE. If FALSE returned, error message can be retrieved by GetLastMsg()
 *
 */
BOOL DeviceCreate(HSWDEVICE* hSwDevice);

/**
 * @brief Close device.
 *
 * @param hSwDevice Handler of software device, used by SwDeviceClose().
 *
 */
VOID DeviceClose(HSWDEVICE hSwDevice);

/**
 * @brief Plug in monitor.
 *
 * @param index [in] Monitor index, should be 0, 1, 2.
 *
 * @return TRUE/FALSE. If FALSE returned, error message can be retrieved by GetLastMsg()
 * 
 * @todo Plug in and set resolution.
 *
 */
BOOL MonitorPlugIn(UINT index);

/**
 * @brief Plug out monitor.
 *
 * @param index [in] Monitor index, should be 0, 1, 2.
 *
 * @return TRUE/FALSE. If FALSE returned, error message can be retrieved by GetLastMsg()
 *
 */
BOOL MonitorPlugOut(UINT index);

/**
 * @brief Get last error message.
 *
 * @return Message string. The string is at most 1024 bytes.
 *
 */
const char* GetLastMsg();

/**
 * @brief Set if print error message when debug.
 *
 * @param b [in] TRUE to enable printing message.
 * 
 * @remark For now, no need to read evironment variable to check if should print.
 * 
 */
VOID SetPrintErrMsg(BOOL b);
