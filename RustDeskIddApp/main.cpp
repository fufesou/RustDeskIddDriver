#include <stdio.h>
#include <stdlib.h>
#include <conio.h>
#include <tchar.h>
#include "./IddController.h"

int main(int argc, char* argv[])
{
    HSWDEVICE hSwDevice = NULL;
    // Now wait for user to indicate the device should be stopped
    printf("Press 'x' to exit and destory the software device\n");
    printf("Press 'u' to install or update driver\n");
    printf("Press 'c' to create device\n");
    printf("Press 'd' to close device\n");
    printf("Press '1','2','3' to plug in monitor (0,1,2)\n");
    printf("Press '4','5','6' to plug out monitor (0,1,2)\n");
    bool bExit = false;
    do
    {
        int key = _getch();
        switch (key)
        {
        case 'u':
        case 'U':
            printf("Install or update driver begin\n");
            if (FALSE == InstallUpdate(_T("C:\\Users\\cxl3\\Desktop\\Debug\\RustDeskIddDriver\\RustDeskIddDriver.inf")))
            {
                printf(GetLastMsg());
            }
            else
            {
                printf("Install or update driver done\n");
            }
            break;
        case 'c':
        case 'C':
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
        case 'D':
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
        case 'X':
            bExit = true;
            break;
        default:
            break;
        }

    }while (!bExit);

    if (hSwDevice)
    {
        DeviceClose(hSwDevice);
    }

    return 0;
}
