#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
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

#define MAX_MONITOR_MODES 10

typedef struct {
    SessionMonitor monitors[IDD_MAX_CONNECTORS];
    UINT count;
} MonitorSession;

static const SessionMonitor* FindSessionMonitor(const MonitorSession* session, UINT connector)
{
    for (UINT i = 0; i < session->count; ++i) {
        if (session->monitors[i].Display.ConnectorIndex == connector) return &session->monitors[i];
    }
    return NULL;
}

static void RecordSessionMonitor(MonitorSession* session, const SessionMonitor* previous, SessionMonitor added)
{
    if (previous) {
        const UINT index = (UINT)(previous - session->monitors);
        --session->count;
        for (UINT i = index; i < session->count; ++i)
            session->monitors[i] = session->monitors[i + 1];
    }
    session->monitors[session->count++] = added;
}

static UINT ReadConnector(void)
{
    int ch;
    do { ch = getchar(); } while (isblank(ch));
    UINT connector = isdigit(ch) ? 0 : IDD_MAX_CONNECTORS;
    while (isdigit(ch)) {
        if (connector < IDD_MAX_CONNECTORS) connector = connector * 10 + (UINT)(ch - '0');
        ch = getchar();
    }
    while (ch != '\n' && ch != EOF) {
        if (!isspace(ch)) connector = IDD_MAX_CONNECTORS;
        ch = getchar();
    }
    if (connector < IDD_MAX_CONNECTORS) return connector;
    fprintf(stderr, "Enter a connector number from 0 to %u.\n", IDD_MAX_CONNECTORS - 1);
    return IDD_MAX_CONNECTORS;
}

static void AddSessionMonitor(MonitorSession* session, BOOL selectConnector)
{
    UINT connector = 0;
    const SessionMonitor* previous = NULL;
    if (selectConnector) {
        printf("Enter the connector to add (0-%u)\n", IDD_MAX_CONNECTORS - 1);
        connector = ReadConnector();
        if (connector == IDD_MAX_CONNECTORS) return;
        previous = FindSessionMonitor(session, connector);
    }
    if (!previous && session->count == IDD_MAX_CONNECTORS) {
        fprintf(stderr, "All connectors are recorded in this session. Use 'A' to retry a specific connector.\n");
        return;
    }
    if (!selectConnector && session->count) {
        connector = session->monitors[session->count - 1].Display.ConnectorIndex;
        do {
            connector = (connector + 1) % IDD_MAX_CONNECTORS;
        } while (FindSessionMonitor(session, connector));
    }
    printf("Plug in monitor on connector %u\n", connector);
    SessionMonitor added;
    if (PlugInProfile(connector, previous ? &previous->Display : NULL, &added)) {
        RecordSessionMonitor(session, previous, added);
        printf("Plug in monitor done\n");
    }
}

int prompt_input()
{
    printf("Press  key                  execute:\n");
    printf("       1. 'q'               1. quit\n");
    printf("       2. 'c'               2. create software device with lifetime \"SWDeviceLifetimeHandle\"\n");
    printf("       3. 'C'               3. create software device with lifetime \"SWDeviceLifetimeParentPresent\"\n");
    printf("       4. 'd'               4. destroy software device\n");
    printf("       5. 'i'               5. install or update driver\n");
    printf("       6. 'u'               6. uninstall driver\n");
    printf("       7. 'a' / 'A'         7. plug in next monitor / choose connector\n");
    printf("       8. 'b'               8. plug out monitor\n");
    printf("       9. 'm'               9. update live monitor modes\n");

    return _getch();
}

int __cdecl wmain(int argc, wchar_t* argv[])
{
    if (argc > 1) return RunDisplayCommand(argc, argv);
    HSWDEVICE hSwDevice = NULL;
    BOOL bExit = FALSE;
    SW_DEVICE_LIFETIME lifetime = SWDeviceLifetimeHandle;

    DWORD width = 1920;
    DWORD height = 1080;
    DWORD sync = 60;

    MonitorSession session = {0};

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
        case 'C':
            lifetime = key == 'c' ? SWDeviceLifetimeHandle : SWDeviceLifetimeParentPresent;
            printf("Create device begin\n");
            if (hSwDevice != NULL)
            {
                printf("Device created before\n");
                break;
            }
            if (FALSE == DeviceCreateWithLifetime(&lifetime, &hSwDevice))
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
        case 'a':
        case 'A':
            AddSessionMonitor(&session, key == 'A');
            break;
        case 'b':
            if (!session.count) {
                printf("No virtual monitors\n");
                break;
            }

            const SessionMonitor* last = &session.monitors[session.count - 1];
            printf("Plug out monitor on connector %u\n", last->Display.ConnectorIndex);
            DWORD disconnect = DisconnectProfile(last);
            if (disconnect == ERROR_SUCCESS) {
                --session.count;
                printf("Plug out monitor done\n");
            } else if (disconnect == ERROR_ALREADY_EXISTS) {
                --session.count;
                fprintf(stderr, "The session monitor was replaced. The current monitor stays connected.\n");
            } else {
                fprintf(stderr, "Disconnect failed (Win32 %lu)\n", disconnect);
            }
            break;
        case 'm':
            if (!session.count) {
                printf("No virtual monitors\n");
                break;
            }
            printf("Choose one of these session connectors");
            for (UINT i = 0; i < session.count; ++i) printf(" %u", session.monitors[i].Display.ConnectorIndex);
            putchar('\n');
            UINT connector = ReadConnector();
            if (connector == IDD_MAX_CONNECTORS) break;
            const SessionMonitor* monitor = FindSessionMonitor(&session, connector);
            if (monitor) {
                printf("\nUpdating monitor modes on connector %u.\n", connector);
                printf("Please add at lease one monitor mode, format: width, height, refresh frequency.\nThe max number of modes is %d.\nPress e to end input.\n", MAX_MONITOR_MODES);

                int input_ok = 0;
                int k = 0;
                MonitorMode modes[MAX_MONITOR_MODES] = { {0, 0, 0}, };
                while (1) {
                    char input[100];
                    if (fgets(input, sizeof(input), stdin) != NULL) {
                        if (input[0] == 'e') {
                            input_ok = 1;
                            break;
                        }
                        else {
                            sscanf_s(input, "%d,%d,%d", &modes[k].width, &modes[k].height, &modes[k].sync);
                        }
                    }
                    else {
                        fprintf(stderr, "Something is wrong while tring to read the input. Please try again\n");
                        break;
                    }
                    ++k;
                    if (k == sizeof(modes) / sizeof(modes[0])) {
                        input_ok = 1;
                        break;
                    }
                }

                if (input_ok == 1) {
                    if (k == 0) {
                        fprintf(stderr, "No monitor modes are added, skip.\n");
                    }
                    else {
                        UpdateProfileModes(monitor, k, modes);
                    }
                }
            } else {
                fprintf(stderr, "Connector %u is not in this session.\n", connector);
            }
            break;
        case 'q':
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
