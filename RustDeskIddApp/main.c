#include <stdio.h>
#include <stdlib.h>
#include <conio.h>
#include <tchar.h>

#include "./IddController.h"

#pragma comment(lib, "kernel32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "winspool.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "uuid.lib")
#pragma comment(lib, "odbc32.lib")
#pragma comment(lib, "odbccp32.lib")
#pragma comment(lib, "swdevice.lib")
#pragma comment(lib, "Cfgmgr32.lib")
#pragma comment(lib, "Setupapi.lib")
#pragma comment(lib, "Newdev.lib")


int prompt_input()
{
    printf("Press  key                  execute:\n");
    printf("       1. 'x'               1. exit\n");
    printf("       2. 'i'               2. install or update driver\n");
    printf("       3. 'u'               3. uninstall driver\n");
    printf("       4. 'c'               4. create device\n");
    printf("       5. 'd'               5. destroy device\n");
    printf("       6. '1','2','3'       6. plug in monitor 0,1,2\n");
    printf("       7. '4','5','6'       7. plug out monitor 0,1,2\n");
    return _getch();
}

int __cdecl main(int argc, char* argv[])
{
    HSWDEVICE hSwDevice = NULL;
    BOOL bExit = FALSE;

    DWORD width = 1920;
    DWORD height = 1080;
    DWORD sync = 60;

    TCHAR exePath[1024] = { 0, };
    (void)GetModuleFileName(NULL, exePath, sizeof(exePath)/sizeof(exePath[0]) - 1);
    *_tcsrchr(exePath, _T('\\')) = _T('\0');
    PTCHAR infPath = _T("RustDeskIddDriver\\RustDeskIddDriver.inf");
    TCHAR infFullPath[1024] = { 0, };
    _sntprintf_s(infFullPath, sizeof(infFullPath) / sizeof(infFullPath[0]), _TRUNCATE, _T("%s\\%s"), exePath, infPath);

    do
    {
        int key = prompt_input();
        BOOL rebootRequired = FALSE;
        switch (key)
        {
        case 'i':
            printf("Install or update driver begin\n");
            if (FALSE == InstallUpdate(infFullPath, &rebootRequired))
            {
                printf(GetLastMsg());
            }
            else
            {
                printf("Install or update driver done, reboot is %s required\n", (rebootRequired == TRUE ? "" : "not"));
            }
            break;
        case 'u':
            printf("Uninstall driver begin\n");
            if (FALSE == Uninstall(infFullPath, &rebootRequired))
            //if (FALSE == InstallUpdate(_T("D:\\projects\\windows\\IndirectDisplay\\x64\\Debug\\RustDeskIddDriver\\RustDeskIddDriver.inf"), &rebootRequired))
            {
                printf(GetLastMsg());
            }
            else
            {
                printf("Uninstall driver done, reboot is %s required\n", (rebootRequired == TRUE ? "" : "not"));
            }
            break;
        case 'c':
            printf("Create device begin\n");
            if (hSwDevice != NULL)
            {
                printf("Device created before\n");
                break;
            }
            if (FALSE == DeviceCreate(&hSwDevice))
            {
                printf(GetLastMsg());
                DeviceClose(hSwDevice);
                hSwDevice = NULL;
            }
            else
            {
                printf("Create device done\n");
            }
            break;
        case 'd':
            printf("Close device begin\n");
            DeviceClose(hSwDevice);
            hSwDevice = NULL;
            printf("Close device done\n");
            break;
        case '1':
        case '2':
        case '3':
            printf("Plug in monitor begin\n");
            if (FALSE == MonitorPlugIn(key - '1', 0, 25))
            {
                printf(GetLastMsg());
            }
            else
            {
                printf("Plug in monitor done\n");

                MonitorMode modes[2] = { { 1920, 1080,  60 }, { 1024,  768,  60 }, };
                if (FALSE == MonitorModesUpdate(key - '1', sizeof(modes)/sizeof(modes[0]), modes))
                {
                    printf(GetLastMsg());
                }
            }
            break;
        case '4':
        case '5':
        case '6':
            printf("Plug out monitor begin\n");
            if (FALSE == MonitorPlugOut(key - '4'))
            {
                printf(GetLastMsg());
            }
            else
            {
                printf("Plug out monitor done\n");
            }
            break;
        case 'x':
            bExit = TRUE;
            break;
        default:
            break;
        }

        printf("\n\n");
    }while (!bExit);

    if (hSwDevice)
    {
        SwDeviceClose(hSwDevice);
    }

    return 0;
}
