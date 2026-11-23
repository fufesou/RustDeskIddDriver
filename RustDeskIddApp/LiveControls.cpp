#include "ProfileFile.h"
#include "DisplayCommands.h"
#include "CursorDisplay.h"
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <iostream>
#include <string>
#include <system_error>

#pragma comment(lib, "dxgi.lib")

namespace commands {
namespace {
void CheckDxgi(HRESULT hr)
{
    if (FAILED(hr)) throw std::system_error(hr, std::system_category(), "Enumerating display adapters");
}

LUID EnumerateGpus(const UINT* wanted)
{
    Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
    CheckDxgi(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
    for (UINT i = 0;; ++i) {
        Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
        const HRESULT hr = factory->EnumAdapters1(i, &adapter);
        if (hr == DXGI_ERROR_NOT_FOUND) break;
        CheckDxgi(hr);
        DXGI_ADAPTER_DESC1 desc{};
        CheckDxgi(adapter->GetDesc1(&desc));
        if (wanted && i == *wanted) return desc.AdapterLuid;
        if (!wanted) std::wcout << i << L"  " << desc.Description << L"  LUID " << std::hex
                                << desc.AdapterLuid.HighPart << L":" << desc.AdapterLuid.LowPart << std::dec << L"\n";
    }
    if (wanted) throw std::runtime_error("GPU index not found");
    return {};
}

void CursorCommand(CtlMonitorIdentity monitor)
{
    CtlCursorQuery connector{monitor.ConnectorIndex};
    CtlNamedCursorQuery named{monitor};
    const IoBuffer input = IsEqualGUID(monitor.ContainerId, GUID_NULL) ?
        IoBuffer{&connector, sizeof(connector)} : IoBuffer{&named, sizeof(named)};
    std::vector<BYTE> bytes(sizeof(CtlCursorState) + IDD_CURSOR_BYTES);
    const DWORD size = Control(IOCTL_IDD_QUERY_CURSOR, input, {bytes.data(), static_cast<DWORD>(bytes.size())});
    if (size < sizeof(CtlCursorState)) throw std::runtime_error("Incomplete cursor response");
    CtlCursorState state{};
    memcpy(&state, bytes.data(), sizeof(state));
    std::cout << (state.Visible ? "visible" : "hidden") << "  ";
    WriteCursorPosition(std::cout, state);
    std::cout << "  shape=" << state.ShapeId << "  " << state.Width << "x" << state.Height
              << "  SDR white=" << state.SdrWhiteNits << " nits\n";
}

std::string WireColorName(UINT space)
{
    switch (space) {
    case 0: return "uninitialized";
    case IDD_WIRE_SDR: return "SDR";
    case IDD_WIRE_HDR10: return "HDR10";
    case IDD_WIRE_SDR_WCG: return "SDR WCG";
    default: return "unknown (" + std::to_string(space) + ")";
    }
}

std::string RgbDepths(UINT mask)
{
    // IDDCX_BITS_PER_COMPONENT assigns successive bits to the even depths from 6 to 16.
    static constexpr UINT depths[] = {6, 8, 10, 12, 14, 16};
    if (!mask) return "none";
    if (mask >> ARRAYSIZE(depths)) return "unknown (mask " + std::to_string(mask) + ")";
    std::string result;
    for (UINT bit = 0; bit < ARRAYSIZE(depths); ++bit) {
        if (!(mask & (1u << bit))) continue;
        if (!result.empty()) result += ",";
        result += std::to_string(depths[bit]);
    }
    return result;
}

void ColorCommand(CtlMonitorIdentity monitor)
{
    CtlCaptureColor result{};
    const bool named = !IsEqualGUID(monitor.ContainerId, GUID_NULL);
    const IoBuffer input = named ? IoBuffer{&monitor, sizeof(monitor)} : IoBuffer{&monitor.ConnectorIndex, sizeof(UINT)};
    const IoBuffer output = named ? IoBuffer{&result, sizeof(result)} : IoBuffer{&result.Color, sizeof(result.Color)};
    if (Control(IOCTL_IDD_QUERY_COLOR, input, output) != output.bytes) throw std::runtime_error("Incomplete color response");
    const auto& state = result.Color;
    std::cout << "Frame available=" << state.FrameAvailable << "  surface color space=" << state.ColorSpace
              << "  output color space=" << WireColorName(state.WireColorSpace) << "  RGB bpc=" << RgbDepths(state.WireRgbBits)
              << "  SDR white=" << state.SdrWhiteNits << " nits  HDR10 metadata=" << state.HasHdr10Metadata
              << "  gamma bytes=" << state.GammaBytes << "\n";
}
} // namespace

void GpuCommand(int argc, wchar_t** argv)
{
    if (argc == 3 && std::wstring(argv[2]) == L"list") {
        EnumerateGpus(nullptr);
        return;
    }
    const bool named = argc == 6 && std::wstring(argv[4]) == L"--profile";
    const bool targeted = argc == 6 && std::wstring(argv[4]) == L"--adapter";
    if ((argc != 4 && !named && !targeted) || std::wstring(argv[2]) != L"set")
        throw std::runtime_error("Use gpu list or gpu set INDEX|auto [--profile FILE | --adapter INSTANCE_ID]");
    CtlNamedRenderAdapter request{};
    if (named) request.Monitor = profiles::LoadIdentity(argv[5]);
    if (std::wstring(argv[3]) != L"auto") {
        const UINT wanted = Index(argv[3]);
        request.RenderAdapter = EnumerateGpus(&wanted);
    }
    const IoBuffer input = named ? IoBuffer{&request, sizeof(request)} : IoBuffer{&request.RenderAdapter, sizeof(LUID)};
    Control({IOCTL_IDD_SET_RENDER_ADAPTER, input, targeted ? argv[5] : nullptr});
}

void OutputCommand(int argc, wchar_t** argv)
{
    CtlMonitorIdentity monitor{};
    if (argc == 3) {
        monitor.ConnectorIndex = Index(argv[2]);
        if (monitor.ConnectorIndex >= IDD_MAX_CONNECTORS) throw std::runtime_error("Connector out of range");
    } else if (argc == 4 && std::wstring(argv[2]) == L"--profile") {
        monitor = profiles::LoadIdentity(argv[3]);
    } else {
        throw std::runtime_error("Use cursor or color with CONNECTOR or --profile FILE");
    }
    if (std::wstring(argv[1]) == L"cursor") CursorCommand(monitor);
    else ColorCommand(monitor);
}
} // namespace commands
