#include "HdrSample.h"
#include <dxgi1_4.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <chrono>
#include <system_error>
#include <thread>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "runtimeobject.lib")

namespace hdr {
namespace {
using namespace winrt;
using namespace winrt::Windows::Graphics::Capture;
using namespace winrt::Windows::Graphics::DirectX;
using namespace winrt::Windows::Graphics::DirectX::Direct3D11;

GraphicsCaptureItem MonitorItem(HMONITOR monitor)
{
    const auto factory = get_activation_factory<GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
    GraphicsCaptureItem item{nullptr};
    check_hresult(factory->CreateForMonitor(monitor, guid_of<GraphicsCaptureItem>(), put_abi(item)));
    return item;
}

IDirect3DDevice CaptureDevice(LUID renderAdapter)
{
    com_ptr<IDXGIFactory4> factory;
    check_hresult(CreateDXGIFactory1(IID_PPV_ARGS(factory.put())));
    com_ptr<IDXGIAdapter1> adapter;
    check_hresult(factory->EnumAdapterByLuid(renderAdapter, IID_PPV_ARGS(adapter.put())));
    com_ptr<ID3D11Device> native;
    check_hresult(D3D11CreateDevice(adapter.get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                    nullptr, 0, D3D11_SDK_VERSION, native.put(), nullptr, nullptr));
    com_ptr<IInspectable> inspectable;
    check_hresult(CreateDirect3D11DeviceFromDXGIDevice(native.as<IDXGIDevice>().get(), inspectable.put()));
    return inspectable.as<IDirect3DDevice>();
}

} // namespace

Frame CaptureItem(const GraphicsCaptureItem& item, LUID renderAdapter)
{
    const auto size = item.Size();
    constexpr int FrameCount = 1;
    const auto pool = Direct3D11CaptureFramePool::CreateFreeThreaded(
        CaptureDevice(renderAdapter), DirectXPixelFormat::R16G16B16A16Float, FrameCount, size);
    const auto session = pool.CreateCaptureSession(item);
    session.StartCapture();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < deadline) {
        if (auto frame = pool.TryGetNextFrame()) {
            const auto actual = frame.ContentSize();
            if (size.Width != actual.Width || size.Height != actual.Height)
                throw std::runtime_error("Monitor size changed during HDR capture");
            auto access = frame.Surface().as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
            com_ptr<ID3D11Texture2D> texture;
            check_hresult(access->GetInterface(__uuidof(ID3D11Texture2D), texture.put_void()));
            auto result = ReadScRgbFrame(texture.get());
            frame.Close();
            session.Close();
            pool.Close();
            return result;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    throw std::runtime_error("Windows Graphics Capture did not deliver a frame within three seconds");
}

Frame CaptureFrame(HMONITOR monitor, LUID renderAdapter)
{
    try {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        struct Apartment { ~Apartment() { winrt::uninit_apartment(); } } apartment;
        if (!GraphicsCaptureSession::IsSupported()) throw std::runtime_error("Windows Graphics Capture is unavailable");
        return CaptureItem(MonitorItem(monitor), renderAdapter);
    } catch (const winrt::hresult_error& error) {
        throw std::system_error(error.code().value, std::system_category(), winrt::to_string(error.message()));
    }
}
} // namespace hdr
