#pragma once
#include <windows.h>

namespace commands {
struct IoBuffer {
    void* data;
    DWORD bytes;
};

struct DriverRequest {
    DWORD code;
    IoBuffer input;
    const wchar_t* adapterId = nullptr;
};

DWORD Control(const DriverRequest& request, IoBuffer output = {});
inline DWORD Control(DWORD code, IoBuffer input, IoBuffer output = {})
{
    return Control({code, input}, output);
}
UINT Index(const wchar_t* text);
void GpuCommand(int argc, wchar_t** argv);
void OutputCommand(int argc, wchar_t** argv);
} // namespace commands
