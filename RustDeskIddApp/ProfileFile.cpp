#include "ProfileFile.h"
#include "../RustDeskIddDriver/DescriptorTimings.h"
#include <charconv>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <system_error>

namespace profiles {
namespace fs = std::filesystem;
using std::runtime_error;

UINT Number(const std::string& text)
{
    UINT value = 0;
    const char* end = text.data() + text.size();
    const auto parsed = std::from_chars(text.data(), end, value);
    if (parsed.ec != std::errc{} || parsed.ptr != end)
        throw runtime_error("Expected an unsigned integer: " + text);
    return value;
}

static CtlDisplayMode ParseMode(const std::string& value)
{
    const auto x = value.find('x');
    const auto at = value.find('@');
    if (x == value.npos || at == value.npos || x >= at)
        throw runtime_error("Expected WIDTHxHEIGHT@NUMERATOR[/DENOMINATOR]");
    const auto rate = value.substr(at + 1);
    const auto slash = rate.find('/');
    return {Number(value.substr(0, x)), Number(value.substr(x + 1, at - x - 1)),
            Number(rate.substr(0, slash)), slash == rate.npos ? 1 : Number(rate.substr(slash + 1))};
}

static std::string Trim(const std::string& value)
{
    const auto first = value.find_first_not_of(" \t\r");
    return first == value.npos ? "" : value.substr(first, value.find_last_not_of(" \t\r") - first + 1);
}

static GUID ParseGuid(const std::string& value)
{
    GUID id{};
    // CLSIDFromString also accepts ProgIDs. A profile specifies a GUID literal.
    if (value.size() != 38 || value.front() != '{' || value.back() != '}' ||
        FAILED(CLSIDFromString(std::wstring(value.begin(), value.end()).c_str(), &id)) || IsEqualGUID(id, GUID_NULL))
        throw runtime_error("Expected a nonzero braced GUID");
    return id;
}

static void SetFlag(UINT& flags, UINT bit, const std::string& value)
{
    if (value == "1") flags |= bit;
    else if (value == "0") flags &= ~bit;
    else throw runtime_error("Boolean values must be 0 or 1");
}

static std::vector<BYTE> ReadEdid(const fs::path& path)
{
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) throw runtime_error("Cannot open EDID file");
    const auto bytes = in.tellg();
    constexpr auto MaxEdidBytes = display::EdidBlock * (1u + UCHAR_MAX);
    if (bytes <= 0 || bytes > static_cast<std::streamoff>(MaxEdidBytes)) throw runtime_error("Invalid EDID size");
    std::vector<BYTE> edid(static_cast<size_t>(bytes));
    in.seekg(0);
    if (!in.read(reinterpret_cast<char*>(edid.data()), bytes)) throw runtime_error("Cannot read EDID file");
    return edid;
}

static void ValidateTimings(const File& file)
{
    const bool hdr = (file.header.Flags & IDD_PROFILE_HDR) != 0;
    display::DescriptorTimings timings;
    if (file.edid.empty()) {
        const auto edid = timings.Generate(file.modes, hdr);
        if (edid.empty()) throw runtime_error("Preferred mode cannot be encoded in a generated EDID. Supply a custom EDID.");
    } else {
        if (!timings.Read(file.edid)) {
            if (display::CheckEdidHeader(file.edid) == display::EdidHeaderStatus::UnsupportedVersion)
                throw runtime_error("Unsupported EDID major version " + std::to_string(file.edid[display::EdidMajorOffset]) +
                                    " (expected " + std::to_string(display::SupportedEdidMajor) + ")");
            throw runtime_error("Invalid EDID length, header, checksum or timing block");
        }
        timings.ForEachTimingIssue([](const CtlDisplayMode& mode, const char* reason) {
            std::cerr << "EDID timing " << mode.Width << 'x' << mode.Height << '@' << mode.Numerator
                      << " is unusable: " << reason << '\n';
        });
        if (hdr && !timings.HasHdr10()) throw runtime_error("Custom HDR EDID must advertise CTA PQ and static metadata type 1");
    }
    const auto result = timings.Validate(file.modes);
    const bool invalidSync = result.status == display::SignalListStatus::InvalidDetailedSync;
    if (invalidSync || result.status == display::SignalListStatus::UnusableEdidTiming) {
        const auto& mode = file.modes[result.modeIndex];
        std::ostringstream error;
        error << "No usable EDID timing for " << mode.Width << 'x' << mode.Height << '@' << mode.Numerator << '/' << mode.Denominator;
        if (invalidSync) error << ". The detailed timing has an invalid sync pulse or porch.";
        throw runtime_error(error.str());
    }
    if (result.status == display::SignalListStatus::RequiresYCbCr420) {
        const auto& mode = file.modes[result.modeIndex];
        std::ostringstream error;
        error << "The EDID restricts " << mode.Width << 'x' << mode.Height << '@' << mode.Numerator << '/' << mode.Denominator
              << " to YCbCr 4:2:0. The driver requires an RGB timing.";
        throw runtime_error(error.str());
    }
    if (result.status == display::SignalListStatus::Duplicate) throw runtime_error("Duplicate display modes resolve to the same Windows signal");
    if (result.status == display::SignalListStatus::InvalidFrequency) throw runtime_error("Display frequency cannot be represented by Windows");
}

using Fields = std::map<std::string, std::string>;

static GUID MonitorId(const Fields& fields)
{
    const auto id = fields.find("id");
    if (id == fields.end()) throw runtime_error("Missing id");
    return ParseGuid(id->second);
}

static CtlDisplayProfile ParseHeader(const Fields& fields)
{
    if (!fields.count("connector")) throw runtime_error("Missing connector");
    auto header = DefaultHeader;
    header.ContainerId = MonitorId(fields);
    header.ConnectorIndex = Number(fields.at("connector"));
    if (header.ConnectorIndex >= IDD_MAX_CONNECTORS) throw runtime_error("Connector out of range");
    for (const auto& [key, value] : fields) {
        if (key == "enabled") SetFlag(header.Flags, IDD_PROFILE_ENABLED, value);
        else if (key == "hdr") SetFlag(header.Flags, IDD_PROFILE_HDR, value);
        else if (key == "cursor") SetFlag(header.Flags, IDD_PROFILE_CURSOR, value);
        else if (key != "id" && key != "connector" && key != "edid") throw runtime_error("Unknown setting: " + key);
    }
    return header;
}

enum class FieldSet { Profile, Identity, Capture };

static bool NeedsField(FieldSet use, const std::string& key)
{
    if (use == FieldSet::Profile || key == "id" || key == "connector") return true;
    return use == FieldSet::Capture && (key == "enabled" || key == "hdr");
}

static Fields ReadFields(const fs::path& path, FieldSet use, std::vector<CtlDisplayMode>* modes = nullptr)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) throw runtime_error("Cannot open profile");
    Fields fields;
    std::string line;
    for (size_t number = 1; std::getline(in, line); ++number) {
        if (number == 1 && line.compare(0, 3, "\xef\xbb\xbf") == 0) line.erase(0, 3);
        line = Trim(line);
        if (line.empty() || line.front() == '#') continue;
        const auto eq = line.find('=');
        if (eq == line.npos) throw runtime_error("Expected key=value on line " + std::to_string(number));
        const auto key = Trim(line.substr(0, eq));
        if (!NeedsField(use, key)) continue;
        const auto value = Trim(line.substr(eq + 1));
        if (key == "mode") {
            if (modes) modes->push_back(ParseMode(value));
        } else if (!fields.emplace(key, value).second) throw runtime_error("Duplicate setting: " + key);
    }
    if (!in.eof()) throw runtime_error("Error reading profile");
    return fields;
}

CtlDisplayProfile LoadCaptureHeader(const fs::path& path)
{
    return ParseHeader(ReadFields(path, FieldSet::Capture));
}

CtlMonitorIdentity LoadIdentity(const fs::path& path)
{
    const auto header = ParseHeader(ReadFields(path, FieldSet::Identity));
    return {header.ConnectorIndex, header.ContainerId};
}

File Load(const fs::path& path)
{
    File file;
    const auto fields = ReadFields(path, FieldSet::Profile, &file.modes);
    file.header = ParseHeader(fields);
    const bool enabled = (file.header.Flags & IDD_PROFILE_ENABLED) != 0;
    if (!enabled && file.modes.empty()) return file;
    const auto status = display::ValidateModes(file.modes, enabled);
    if (status == display::ModeListStatus::TextureTooLarge)
        throw runtime_error("Display width and height must each fit the D3D11 texture limit of " +
                            std::to_string(D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION) + " pixels");
    if (status != display::ModeListStatus::Valid) throw runtime_error("Invalid, duplicate or missing display mode");
    if (enabled) {
        const auto edid = fields.find("edid");
        if (edid != fields.end() && !edid->second.empty()) file.edid = ReadEdid(path.parent_path() / fs::u8path(edid->second));
        ValidateTimings(file);
    }
    return file;
}

void Create(const fs::path& path, UINT connector)
{
    if (connector >= IDD_MAX_CONNECTORS) throw runtime_error("Connector out of range");
    GUID id;
    if (FAILED(CoCreateGuid(&id))) throw runtime_error("Cannot generate a monitor GUID");
    wchar_t wideId[39];
    if (!StringFromGUID2(id, wideId, ARRAYSIZE(wideId))) throw runtime_error("Cannot format monitor GUID");
    std::string guid;
    for (const wchar_t* p = wideId; *p; ++p) guid.push_back(static_cast<char>(*p));
    std::ostringstream text;
    text << "connector=" << connector << "\nid=" << guid
         << "\nenabled=1\nhdr=0\ncursor=0\nmode=1920x1080@60\n"
         << "mode=1920x1080@60000/1001\nmode=1920x1080@240\n"
         << "mode=2560x1440@120\nmode=2560x1440@144\nmode=3840x2160@120\n";
    const auto data = text.str();
    HANDLE handle = CreateFileW(path.c_str(), GENERIC_WRITE | DELETE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) throw std::system_error(GetLastError(), std::system_category(), "Create profile");
    DWORD written = 0;
    const BOOL ok = WriteFile(handle, data.data(), static_cast<DWORD>(data.size()), &written, nullptr);
    const DWORD error = ok ? ERROR_WRITE_FAULT : GetLastError();
    if (!ok || written != data.size()) {
        FILE_DISPOSITION_INFO remove{TRUE};
        SetFileInformationByHandle(handle, FileDispositionInfo, &remove, sizeof(remove));
        CloseHandle(handle);
        throw std::system_error(error, std::system_category(), "Write profile");
    }
    if (!CloseHandle(handle)) throw std::system_error(GetLastError(), std::system_category(), "Close profile");
}

std::vector<BYTE> Packet(const File& file, const GUID* attachmentId)
{
    auto header = file.header;
    const size_t prefix = attachmentId ? offsetof(CtlSessionProfile, Profile) : 0;
    size_t modeBytes = 0;
    if (header.Flags & IDD_PROFILE_ENABLED) {
        const size_t available = MAXDWORD - prefix - sizeof(header);
        if (file.edid.size() > available || file.modes.size() > (available - file.edid.size()) / sizeof(CtlDisplayMode))
            throw runtime_error("Profile exceeds the IOCTL buffer size");
        header.ModeCount = static_cast<UINT>(file.modes.size());
        header.EdidBytes = static_cast<UINT>(file.edid.size());
        modeBytes = file.modes.size() * sizeof(CtlDisplayMode);
    }
    std::vector<BYTE> data(prefix + sizeof(header) + modeBytes + header.EdidBytes);
    if (attachmentId) memcpy(data.data(), attachmentId, sizeof(*attachmentId));
    BYTE* payload = data.data() + prefix;
    memcpy(payload, &header, sizeof(header));
    if (modeBytes) memcpy(payload + sizeof(header), file.modes.data(), modeBytes);
    if (header.EdidBytes) memcpy(payload + sizeof(header) + modeBytes, file.edid.data(), header.EdidBytes);
    return data;
}
} // namespace profiles
