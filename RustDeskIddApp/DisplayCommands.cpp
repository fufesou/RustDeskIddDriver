#include "ProfileFile.h"
#include "ProfileReady.h"
#include "DisplayCommands.h"
#include "CaptureDevice.h"
#include "HdrSample.h"
#include "IddController.h"
#include <shlobj.h>
#include <iostream>
#include <stdexcept>
#include <system_error>
#include <set>
#include <tuple>

namespace commands {
DWORD Control(const DriverRequest& request, IoBuffer output)
{
    const auto code = request.code;
    DWORD used = 0;
    const auto send = [&](HANDLE device, const DriverRequest& operation) -> DWORD {
        return DeviceIoControl(device, operation.code, operation.input.data, operation.input.bytes, output.data, output.bytes, &used, nullptr) ?
            ERROR_SUCCESS : GetLastError();
    };
    const auto attempt = [&]() -> profiles::RequestResult {
        try {
            return profiles::SendControl(request, send);
        } catch (const std::system_error& error) {
            const profiles::RequestResult failed{static_cast<DWORD>(error.code().value()), true};
            if (profiles::WaitingForDevice(failed)) return failed;
            throw;
        }
    };
    const bool apply = code == IOCTL_IDD_APPLY_PROFILE || code == IOCTL_IDD_SESSION_PROFILE;
    const auto result = apply ?
        profiles::RequestWhenReady(attempt, GetTickCount64, Sleep) : attempt();
    if (result.error) {
        std::string operation = result.opening ? "Open display interface" : "Display driver request";
        if (!result.opening && result.error == static_cast<DWORD>(ERROR_GRAPHICS_VIDPN_MODALITY_NOT_SUPPORTED))
            operation = "No configured target mode fits the render GPU texture dimensions";
        if (!result.opening && result.error == ERROR_ALREADY_EXISTS &&
            (code == IOCTL_IDD_SESSION_PROFILE || code == IOCTL_IDD_SESSION_MODES))
            operation = "Connector or monitor ID belongs to another attachment";
        throw std::system_error(result.error, std::system_category(), operation + " (Win32 " + std::to_string(result.error) + ")");
    }
    return used;
}

UINT Index(const wchar_t* text)
{
    std::string digits;
    for (const wchar_t* p = text; *p; ++p) {
        if (*p < L'0' || *p > L'9') throw std::runtime_error("Expected a decimal index");
        digits.push_back(static_cast<char>(*p));
    }
    return profiles::Number(digits);
}
} // namespace commands

namespace {
using commands::Control;
using commands::Index;

void ProfileCommand(int argc, wchar_t** argv)
{
    const std::wstring action = argv[2];
    if (argc == 5 && action == L"init") {
        profiles::Create(argv[3], Index(argv[4]));
        return;
    }
    const bool targeted = argc == 6 && action == L"apply" && std::wstring(argv[4]) == L"--adapter";
    if ((!targeted && argc != 4) || (action != L"validate" && action != L"apply"))
        throw std::runtime_error("Use profile init FILE CONNECTOR, profile validate FILE or profile apply FILE [--adapter INSTANCE_ID]");
    const auto file = profiles::Load(argv[3]);
    if (action == L"apply") {
        auto packet = profiles::Packet(file);
        Control({IOCTL_IDD_APPLY_PROFILE, {packet.data(), static_cast<DWORD>(packet.size())}, targeted ? argv[5] : nullptr});
    }
    std::cout << "Connector " << file.header.ConnectorIndex << "  "
              << ((file.header.Flags & IDD_PROFILE_ENABLED) ? "enabled" : "disabled") << "\n";
    if (action == L"apply" && (file.header.Flags & IDD_PROFILE_ENABLED))
        std::cout << "Configured modes (available Windows modes depend on the render GPU)\n";
    for (const auto& m : file.modes)
        std::cout << m.Width << "x" << m.Height << "@" << m.Numerator << "/" << m.Denominator << "\n";
}

DISPLAYCONFIG_SOURCE_DEVICE_NAME ProfileSource(CtlMonitorIdentity identity)
{
    CtlCaptureColor state{};
    if (Control(IOCTL_IDD_QUERY_COLOR, {&identity, sizeof(identity)}, {&state, sizeof(state)}) != sizeof(state))
        throw std::runtime_error("Incomplete display target response");
    return hdr::DisplaySourceName(state.Target);
}

void ModeCommand(const wchar_t* path)
{
    const auto identity = profiles::LoadIdentity(path);
    const auto source = ProfileSource(identity);
    std::set<std::tuple<DWORD, DWORD, DWORD>> modes;
    DEVMODEW mode{};
    mode.dmSize = sizeof(mode);
    for (DWORD i = 0; EnumDisplaySettingsExW(source.viewGdiDeviceName, i, &mode, 0); ++i)
        modes.emplace(mode.dmPelsWidth, mode.dmPelsHeight, mode.dmDisplayFrequency);
    if (modes.empty()) throw std::runtime_error("Windows returned no display modes for the named monitor");
    const auto current = ProfileSource(identity);
    if (source.header.id != current.header.id || !hdr::SameAdapter(source.header.adapterId, current.header.adapterId) ||
        _wcsicmp(source.viewGdiDeviceName, current.viewGdiDeviceName))
        throw std::runtime_error("The Windows display source changed during mode enumeration");
    for (const auto& [width, height, hz] : modes)
        std::cout << width << "x" << height << "@" << hz << "\n";
}

std::unique_ptr<hdr::Calibration> ReadCalibration(HANDLE device, CtlColorQuery query)
{
    auto data = std::make_unique<hdr::Calibration>();
    CtlCaptureColor header{};
    std::vector<BYTE> buffer(sizeof(header) + sizeof(CtlColorTransform));
    profiles::CaptureBuffer output{buffer.data(), static_cast<DWORD>(buffer.size())};
    const DWORD error = profiles::QueryCaptureColor(device, query, output);
    if (error) throw std::system_error(error, std::system_category(), "Read HDR calibration from the selected adapter");
    const auto received = output.used;
    if (received < sizeof(header)) throw std::runtime_error("Incomplete HDR state");
    memcpy(&header, buffer.data(), sizeof(header));
    data->state = header.Color;
    data->generation = header.Generation;
    data->target = header.Target;
    data->renderAdapter = header.RenderAdapterLuid;
    const auto& state = data->state;
    if (!state.FrameAvailable || state.WireColorSpace != IDD_WIRE_HDR10 || !(state.WireRgbBits & IDD_WIRE_10BPC))
        throw std::runtime_error("The named monitor needs an active 10-bit Windows HDR path");
    if (state.GammaBytes != received - sizeof(header)) throw std::runtime_error("Incomplete HDR calibration");
    const auto* bytes = buffer.data() + sizeof(header);
    if (state.GammaType == IDD_GAMMA_TRANSFORM && state.GammaBytes == sizeof(data->transform))
        memcpy(&data->transform, bytes, sizeof(data->transform));
    else if (state.GammaType == IDD_GAMMA_RGB256 && state.GammaBytes == sizeof(data->legacy))
        memcpy(data->legacy, bytes, sizeof(data->legacy));
    else if (state.GammaType > 1 || state.GammaBytes) throw std::runtime_error("Unsupported HDR calibration layout");
    return data;
}

void CaptureCommand(wchar_t** argv)
{
    const auto header = profiles::LoadCaptureHeader(argv[2]);
    if (!(header.Flags & IDD_PROFILE_ENABLED) || !(header.Flags & IDD_PROFILE_HDR))
        throw std::runtime_error("HDR capture requires an enabled HDR profile");
    const CtlColorQuery query{header.ConnectorIndex, header.ContainerId};
    const auto device = profiles::OpenProfileDevice(query);
    const auto before = ReadCalibration(device.Get(), query);
    const auto monitor = hdr::ActiveMonitor(before->target);
    const auto frame = hdr::CaptureFrame(monitor, before->renderAdapter);
    const auto after = ReadCalibration(device.Get(), query);
    if (!hdr::SameCalibration(*before, *after) || hdr::ActiveMonitor(after->target) != monitor)
        throw std::runtime_error("Display target or color state changed during Windows capture. Capture a stable HDR scene.");
    hdr::SavePng(argv[3], hdr::EncodePng(frame, *after));
    std::wcout << L"Saved " << frame.width << L"x" << frame.height << L" BT.2020/PQ PNG to " << argv[3] << L"\n";
}

std::filesystem::path InteractiveFile(UINT connector)
{
    PWSTR raw{};
    const HRESULT hr = SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &raw);
    std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)> owned(raw, CoTaskMemFree);
    if (FAILED(hr)) throw std::system_error(hr, std::system_category(), "Locate profile directory");
    const auto folder = std::filesystem::path(owned.get()) / L"RustDeskIddApp";
    std::filesystem::create_directories(folder);
    return folder / (L"connector-" + std::to_wstring(connector) + L".profile");
}

void CheckPreviousProfile(const CtlMonitorIdentity* previous, const GUID& requestedId)
{
    if (!previous || IsEqualGUID(previous->ContainerId, requestedId)) return;
    // A changed profile ID could otherwise attach on a second adapter while the old display is still connected.
    try {
        profiles::OpenProfileDevice(*previous);
    } catch (const std::system_error& error) {
        if (error.code().value() == ERROR_NOT_FOUND) return;
        throw;
    }
    throw std::runtime_error("The previous profile on this connector is still connected");
}
} // namespace

extern "C" BOOL PlugInProfile(UINT connector, const CtlMonitorIdentity* previous, SessionMonitor* added)
{
    try {
        const auto path = InteractiveFile(connector);
        if (!std::filesystem::exists(path)) profiles::Create(path, connector);
        std::cout << "Display profile: " << path.u8string() << "\n";
        const auto file = profiles::Load(path);
        if (file.header.ConnectorIndex != connector) throw std::runtime_error("Profile connector does not match its slot");
        if (!(file.header.Flags & IDD_PROFILE_ENABLED)) throw std::runtime_error("Profile is disabled");
        CheckPreviousProfile(previous, file.header.ContainerId);
        GUID attachment{};
        const HRESULT hr = CoCreateGuid(&attachment);
        if (FAILED(hr)) throw std::system_error(hr, std::system_category(), "Create attachment ID");
        auto packet = profiles::Packet(file, &attachment);
        Control(IOCTL_IDD_SESSION_PROFILE, {packet.data(), static_cast<DWORD>(packet.size())});
        *added = {{connector, file.header.ContainerId}, attachment};
        return TRUE;
    } catch (const std::exception& error) {
        std::cerr << error.what() << "\n";
        return FALSE;
    }
}

extern "C" DWORD DisconnectProfile(const SessionMonitor* monitor)
{
    try {
        CtlSessionProfile request{monitor->AttachmentId,
            {IDD_PROFILE_VERSION, monitor->Display.ConnectorIndex, monitor->Display.ContainerId}};
        Control(IOCTL_IDD_SESSION_PROFILE, {&request, sizeof(request)});
        return ERROR_SUCCESS;
    } catch (const std::system_error& error) {
        return static_cast<DWORD>(error.code().value());
    } catch (const std::bad_alloc&) {
        return ERROR_OUTOFMEMORY;
    }
}

extern "C" BOOL UpdateProfileModes(const SessionMonitor* monitor, UINT count, const MonitorMode* modes)
{
    try {
        profiles::File file;
        file.header.ConnectorIndex = monitor->Display.ConnectorIndex;
        file.header.ContainerId = monitor->Display.ContainerId;
        for (UINT i = 0; i < count; ++i)
            file.modes.push_back({modes[i].width, modes[i].height, modes[i].sync, 1});
        auto packet = profiles::Packet(file, &monitor->AttachmentId);
        Control(IOCTL_IDD_SESSION_MODES, {packet.data(), static_cast<DWORD>(packet.size())});
        return TRUE;
    } catch (const std::exception& error) {
        std::cerr << error.what() << "\n";
        return FALSE;
    }
}

extern "C" int RunDisplayCommand(int argc, wchar_t** argv)
{
    try {
        const std::wstring group = argv[1];
        if (group == L"profile" && argc >= 4) ProfileCommand(argc, argv);
        else if (group == L"modes" && argc == 3) ModeCommand(argv[2]);
        else if (group == L"gpu" && argc >= 3) commands::GpuCommand(argc, argv);
        else if (group == L"adapter" && argc == 3 && std::wstring(argv[2]) == L"list")
            profiles::VisitAdapterInstances(profiles::DisplayInterfaces().get(), [](DWORD, const wchar_t* id) {
                std::wcout << id << L"\n";
                return true;
            });
        else if (group == L"cursor" || group == L"color") commands::OutputCommand(argc, argv);
        else if (group == L"hdr-capture" && argc == 4) CaptureCommand(argv);
        else throw std::runtime_error("Use profile init|validate|apply, modes PROFILE, adapter list, gpu list|set, cursor or color CONNECTOR|--profile FILE, or hdr-capture PROFILE NEW.png");
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << error.what() << "\n";
        return EXIT_FAILURE;
    }
}
