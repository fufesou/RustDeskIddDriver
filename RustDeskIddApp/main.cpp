#include <stdio.h>
#include <stdlib.h>
#include <conio.h>
#include <tchar.h>

#include "./IddController.h"

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

int main(int argc, char* argv[])
{
    HSWDEVICE hSwDevice = NULL;
    bool bExit = false;
    do
    {
        int key = prompt_input();
        BOOL rebootRequired = FALSE;
        switch (key)
        {
        case 'i':
            printf("Install or update driver begin\n");
            if (FALSE == InstallUpdate(_T("C:\\Users\\cxl3\\Desktop\\Debug\\RustDeskIddDriver\\RustDeskIddDriver.inf"), &rebootRequired))
            // if (FALSE == InstallUpdate(_T("D:\\projects\\windows\\IndirectDisplay\\x64\\Debug\\RustDeskIddDriver.inf")))
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
            if (FALSE == Uninstall(_T("C:\\Users\\cxl3\\Desktop\\Debug\\RustDeskIddDriver\\RustDeskIddDriver.inf"), &rebootRequired))
                // if (FALSE == InstallUpdate(_T("D:\\projects\\windows\\IndirectDisplay\\x64\\Debug\\RustDeskIddDriver.inf")))
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
            if (FALSE == MonitorPlugIn(key - '1'))
            {
                printf(GetLastMsg());
            }
            else
            {
                printf("Plug in monitor done\n");
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
            bExit = true;
            break;
        default:
            break;
        }

        printf("\n\n");
    }while (!bExit);

    if (hSwDevice)
    {
        DeviceClose(hSwDevice);
    }

    return 0;
}
