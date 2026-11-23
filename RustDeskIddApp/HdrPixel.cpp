#include "HdrSample.h"
#include <DirectXPackedVector.h>
#include <wrl/client.h>
#include <stdexcept>
#include <system_error>

namespace hdr {
using Microsoft::WRL::ComPtr;

static void Check(HRESULT hr, const char* operation)
{
    if (FAILED(hr)) throw std::system_error(hr, std::system_category(), operation);
}

Rgb ScRgb(const std::array<USHORT, 4>& half)
{
    Rgb result{};
    for (size_t i = 0; i < result.size(); ++i)
        result[i] = DirectX::PackedVector::XMConvertHalfToFloat(half[i]);
    return result;
}

Frame ReadScRgbFrame(ID3D11Texture2D* texture)
{
    D3D11_TEXTURE2D_DESC source{};
    texture->GetDesc(&source);
    if (source.Format != DXGI_FORMAT_R16G16B16A16_FLOAT || source.SampleDesc.Count != 1)
        throw std::runtime_error("Windows capture did not return a single-sample FP16 surface");
    ComPtr<ID3D11Device> device;
    texture->GetDevice(&device);
    D3D11_TEXTURE2D_DESC readback{};
    readback.Width = source.Width;
    readback.Height = source.Height;
    readback.MipLevels = readback.ArraySize = readback.SampleDesc.Count = 1;
    readback.Format = source.Format;
    readback.Usage = D3D11_USAGE_STAGING;
    readback.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> stage;
    Check(device->CreateTexture2D(&readback, nullptr, &stage), "Create HDR readback texture");
    ComPtr<ID3D11DeviceContext> context;
    device->GetImmediateContext(&context);
    context->CopySubresourceRegion(stage.Get(), 0, 0, 0, 0, texture, 0, nullptr);
    Frame result{source.Width, source.Height};
    result.pixels.resize(size_t(source.Width) * source.Height);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    Check(context->Map(stage.Get(), 0, D3D11_MAP_READ, 0, &mapped), "Read captured HDR frame");
    const auto* bytes = static_cast<const BYTE*>(mapped.pData);
    const size_t stride = source.Width * sizeof(result.pixels.front());
    for (UINT y = 0; y < source.Height; ++y)
        memcpy(result.pixels.data() + size_t(y) * source.Width, bytes + size_t(y) * mapped.RowPitch, stride);
    context->Unmap(stage.Get(), 0);
    return result;
}
} // namespace hdr
