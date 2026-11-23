#include "HdrSample.h"
#include <wincodec.h>
#include <algorithm>
#include <wrl/client.h>
#include <stdexcept>
#include <system_error>

#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")

namespace hdr {
using Microsoft::WRL::ComPtr;
std::vector<BYTE> TagPng(const std::vector<BYTE>& png, const CtlColorState& state);

namespace {
void Check(HRESULT hr, const char* operation)
{
    if (FAILED(hr)) throw std::system_error(hr, std::system_category(), operation);
}

struct ComApartment {
    const HRESULT init = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ~ComApartment() { if (SUCCEEDED(init)) CoUninitialize(); }
};

void WriteRows(IWICBitmapFrameEncode* image, const Frame& frame, const Calibration& calibration)
{
    Check(image->Initialize(nullptr), "Initialize PNG frame");
    Check(image->SetSize(frame.width, frame.height), "Set PNG dimensions");
    auto format = GUID_WICPixelFormat48bppRGB;
    Check(image->SetPixelFormat(&format), "Set 16-bit RGB samples");
    if (format != GUID_WICPixelFormat48bppRGB) throw std::runtime_error("16-bit RGB PNG encoding unavailable");
    std::vector<USHORT> row(size_t(frame.width) * 3);
    const UINT stride = static_cast<UINT>(row.size() * sizeof(USHORT));
    for (UINT y = 0; y < frame.height; ++y) {
        for (UINT x = 0; x < frame.width; ++x) {
            const auto codes = Hdr10(ScRgb(frame.pixels[size_t(y) * frame.width + x]), calibration);
            for (size_t c = 0; c < codes.size(); ++c)
                row[size_t(x) * 3 + c] = static_cast<USHORT>((codes[c] * 65535 + 511) / 1023);
        }
        Check(image->WritePixels(1, stride, stride, reinterpret_cast<BYTE*>(row.data())), "Write HDR pixels");
    }
    Check(image->Commit(), "Commit HDR PNG frame");
}

std::vector<BYTE> StreamBytes(IStream* stream)
{
    STATSTG info{};
    Check(stream->Stat(&info, STATFLAG_NONAME), "Measure HDR PNG stream");
    if (info.cbSize.HighPart) throw std::runtime_error("PNG stream exceeds the Windows imaging buffer limit");
    Check(stream->Seek({}, STREAM_SEEK_SET, nullptr), "Rewind HDR PNG stream");
    std::vector<BYTE> bytes(info.cbSize.LowPart);
    ULONG count = 0;
    Check(stream->Read(bytes.data(), info.cbSize.LowPart, &count), "Read HDR PNG stream");
    if (count != bytes.size()) throw std::runtime_error("Short read from HDR PNG encoder");
    return bytes;
}
} // namespace

std::vector<BYTE> EncodePng(const Frame& frame, const Calibration& calibration)
{
    if (!frame.width || !frame.height || UINT64(frame.width) * frame.height != frame.pixels.size() ||
        frame.width > UINT_MAX / (3 * sizeof(USHORT))) throw std::runtime_error("Invalid HDR frame dimensions");
    ComApartment apartment;
    if (apartment.init != RPC_E_CHANGED_MODE) Check(apartment.init, "Initialize Windows Imaging Component");
    ComPtr<IWICImagingFactory> factory;
    Check(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)), "Create WIC factory");
    ComPtr<IStream> stream;
    Check(CreateStreamOnHGlobal(nullptr, TRUE, &stream), "Create HDR PNG stream");
    ComPtr<IWICBitmapEncoder> encoder;
    Check(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder), "Create PNG encoder");
    Check(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache), "Initialize PNG encoder");
    ComPtr<IWICBitmapFrameEncode> image;
    Check(encoder->CreateNewFrame(&image, nullptr), "Create HDR PNG frame");
    WriteRows(image.Get(), frame, calibration);
    Check(encoder->Commit(), "Commit HDR PNG");
    return TagPng(StreamBytes(stream.Get()), calibration.state);
}

void SavePng(const wchar_t* path, const std::vector<BYTE>& png)
{
    const HANDLE file = CreateFileW(path, GENERIC_WRITE | DELETE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) throw std::system_error(GetLastError(), std::system_category(), "Create new HDR PNG");
    size_t offset = 0;
    DWORD error = ERROR_SUCCESS;
    while (offset < png.size()) {
        const DWORD size = static_cast<DWORD>((std::min)(png.size() - offset, size_t(MAXDWORD)));
        DWORD written = 0;
        if (!WriteFile(file, png.data() + offset, size, &written, nullptr)) { error = GetLastError(); break; }
        if (!written) { error = ERROR_WRITE_FAULT; break; }
        offset += written;
    }
    if (error) {
        FILE_DISPOSITION_INFO discard{TRUE};
        const BOOL marked = SetFileInformationByHandle(file, FileDispositionInfo, &discard, sizeof(discard));
        CloseHandle(file);
        throw std::system_error(error, std::system_category(), marked ? "Write HDR PNG" : "Write HDR PNG (partial file remains)");
    }
    if (!CloseHandle(file)) throw std::system_error(GetLastError(), std::system_category(), "Close HDR PNG");
}
} // namespace hdr
