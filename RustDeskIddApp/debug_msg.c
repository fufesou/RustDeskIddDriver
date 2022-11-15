#include <Windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <strsafe.h>


BOOL g_printMsg = TRUE;
char g_lastMsg[1024];
const char* g_msgHeader = "RustDeskIdd: ";


VOID SetPrintErrMsg(BOOL b)
{
    g_printMsg = (b == TRUE);
}

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

BOOL ReturnUnsupported(char* call)
{
    SetLastMsg("%s is unsupported. Win8 or upper is required.\n", call);
    if (g_printMsg)
    {
        printf(g_lastMsg);
    }
    return FALSE;
}
