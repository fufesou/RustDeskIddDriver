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
    printf("       8. 'h','H'           8. monitor 0: h(height-10) H(height+10)\n");
    printf("       9. 'w','W'           9. monitor 0: w(width-10)  W(width+10) \n");
    return _getch();
}

int main(int argc, char* argv[])
{
    HSWDEVICE hSwDevice = NULL;
    bool bExit = false;

    DWORD width = 1920;
    DWORD height = 1080;
    DWORD sync = 60;
    do
    {
        int key = prompt_input();
        BOOL rebootRequired = FALSE;
        switch (key)
        {
        case 'i':
            printf("Install or update driver begin\n");
            if (FALSE == InstallUpdate(_T("C:\\Users\\cxl3\\Desktop\\Debug\\RustDeskIddDriver\\RustDeskIddDriver.inf"), &rebootRequired))
            //if (FALSE == InstallUpdate(_T("D:\\projects\\windows\\IndirectDisplay\\x64\\Debug\\RustDeskIddDriver\\RustDeskIddDriver.inf"), &rebootRequired))
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
            if (FALSE == MonitorPlugIn(key - '1', 25))
            {
                printf(GetLastMsg());
            }
            else
            {
                printf("Plug in monitor done\n");
                if (FALSE == MonitorModeUpdate(key - '1', width, height, sync))
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
        case 'h':
            printf("Height decrese begin\n");
            // demo no need to care about height <= 0
            height -= 10;
            if (FALSE == MonitorModeUpdate(0, width, height, sync))
            {
                printf(GetLastMsg());
            }
            else
            {
                printf("Height decrese done\n");
            }
            break;
        case 'H':
            printf("Height increse begin\n");
            // demo no need to care about height too big
            height += 10;
            if (FALSE == MonitorModeUpdate(0, width, height, sync))
            {
                printf(GetLastMsg());
            }
            else
            {
                printf("Height increse done\n");
            }
            break;
        case 'w':
            printf("Width decrese begin\n");
            // demo no need to care about width <= 0
            width -= 10;
            if (FALSE == MonitorModeUpdate(0, width, height, sync))
            {
                printf(GetLastMsg());
            }
            else
            {
                printf("Width decrese done\n");
            }
            break;
        case 'W':
            printf("Width increase begin\n");
            // demo no need to care about width too big
            width += 10;
            if (FALSE == MonitorModeUpdate(0, width, height, sync))
            {
                printf(GetLastMsg());
            }
            else
            {
                printf("Width increase done\n");
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
