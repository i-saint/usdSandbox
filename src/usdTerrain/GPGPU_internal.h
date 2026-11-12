#pragma once
#include "GPGPU.h"
#include <cassert>
#ifdef _WIN32
#   include <windows.h>
#   include <dxgi1_6.h>
#endif

namespace ist {

#ifdef _WIN32
    class AutoEvent
    {
    public:
        AutoEvent() : _event(::CreateEvent(nullptr, FALSE, FALSE, nullptr)) {}
        ~AutoEvent() { ::CloseHandle(_event); }
        void Wait() { ::WaitForSingleObject(_event, INFINITE); }
        void Signal() { ::SetEvent(_event); }
        HANDLE Get() const { return _event; }

    private:
        HANDLE _event;
    };

    void GetDXGIFormat(GPUTextureFormat format, DXGI_FORMAT& dxgi, DXGI_FORMAT& dxgitypeless, size_t& byteSize);
#endif // _WIN32


    template<class T, class... Args>
    inline std::shared_ptr<T> MakeRef(Args&&... args)
    {
        auto* p = new T(std::forward<Args>(args)...);
        return std::shared_ptr<T>(p, [](T* p) { if (p) p->Release(); });
    }

#ifdef _WIN32
    inline void GetDXGIFormat(GPUTextureFormat format, DXGI_FORMAT& dxgi, DXGI_FORMAT& dxgitypeless, size_t& byteSize)
    {
        switch (format) {
        case GPUTextureFormat::R8Unorm:  dxgi = DXGI_FORMAT_R8_UNORM;  dxgitypeless = DXGI_FORMAT_R8_TYPELESS;  byteSize = 1; break;
        case GPUTextureFormat::R8Snorm:  dxgi = DXGI_FORMAT_R8_SNORM;  dxgitypeless = DXGI_FORMAT_R8_TYPELESS;  byteSize = 1; break;
        case GPUTextureFormat::R8Sint:   dxgi = DXGI_FORMAT_R8_SINT;   dxgitypeless = DXGI_FORMAT_R8_TYPELESS;  byteSize = 1; break;
        case GPUTextureFormat::R8Uint:   dxgi = DXGI_FORMAT_R8_UINT;   dxgitypeless = DXGI_FORMAT_R8_TYPELESS;  byteSize = 1; break;
        case GPUTextureFormat::R16Unorm: dxgi = DXGI_FORMAT_R16_UNORM; dxgitypeless = DXGI_FORMAT_R16_TYPELESS; byteSize = 2; break;
        case GPUTextureFormat::R16Snorm: dxgi = DXGI_FORMAT_R16_SNORM; dxgitypeless = DXGI_FORMAT_R16_TYPELESS; byteSize = 2; break;
        case GPUTextureFormat::R16Uint:  dxgi = DXGI_FORMAT_R16_UINT;  dxgitypeless = DXGI_FORMAT_R16_TYPELESS; byteSize = 2; break;
        case GPUTextureFormat::R16Sint:  dxgi = DXGI_FORMAT_R16_SINT;  dxgitypeless = DXGI_FORMAT_R16_TYPELESS; byteSize = 2; break;
        case GPUTextureFormat::R16Float: dxgi = DXGI_FORMAT_R16_FLOAT; dxgitypeless = DXGI_FORMAT_R16_TYPELESS; byteSize = 2; break;
        case GPUTextureFormat::R32Uint:  dxgi = DXGI_FORMAT_R32_UINT;  dxgitypeless = DXGI_FORMAT_R32_TYPELESS; byteSize = 4; break;
        case GPUTextureFormat::R32Sint:  dxgi = DXGI_FORMAT_R32_SINT;  dxgitypeless = DXGI_FORMAT_R32_TYPELESS; byteSize = 4; break;
        case GPUTextureFormat::R32Float: dxgi = DXGI_FORMAT_R32_FLOAT; dxgitypeless = DXGI_FORMAT_R32_TYPELESS; byteSize = 4; break;

        case GPUTextureFormat::RG8Unorm:  dxgi = DXGI_FORMAT_R8G8_UNORM;   dxgitypeless = DXGI_FORMAT_R8G8_TYPELESS;   byteSize = 2; break;
        case GPUTextureFormat::RG8Snorm:  dxgi = DXGI_FORMAT_R8G8_SNORM;   dxgitypeless = DXGI_FORMAT_R8G8_TYPELESS;   byteSize = 2; break;
        case GPUTextureFormat::RG8Sint:   dxgi = DXGI_FORMAT_R8G8_SINT;    dxgitypeless = DXGI_FORMAT_R8G8_TYPELESS;   byteSize = 2; break;
        case GPUTextureFormat::RG8Uint:   dxgi = DXGI_FORMAT_R8G8_UINT;    dxgitypeless = DXGI_FORMAT_R8G8_TYPELESS;   byteSize = 2; break;
        case GPUTextureFormat::RG16Unorm: dxgi = DXGI_FORMAT_R16G16_UNORM; dxgitypeless = DXGI_FORMAT_R16G16_TYPELESS; byteSize = 4; break;
        case GPUTextureFormat::RG16Snorm: dxgi = DXGI_FORMAT_R16G16_SNORM; dxgitypeless = DXGI_FORMAT_R16G16_TYPELESS; byteSize = 4; break;
        case GPUTextureFormat::RG16Uint:  dxgi = DXGI_FORMAT_R16G16_UINT;  dxgitypeless = DXGI_FORMAT_R16G16_TYPELESS; byteSize = 4; break;
        case GPUTextureFormat::RG16Sint:  dxgi = DXGI_FORMAT_R16G16_SINT;  dxgitypeless = DXGI_FORMAT_R16G16_TYPELESS; byteSize = 4; break;
        case GPUTextureFormat::RG16Float: dxgi = DXGI_FORMAT_R16G16_FLOAT; dxgitypeless = DXGI_FORMAT_R16G16_TYPELESS; byteSize = 4; break;
        case GPUTextureFormat::RG32Uint:  dxgi = DXGI_FORMAT_R32G32_UINT;  dxgitypeless = DXGI_FORMAT_R32G32_TYPELESS; byteSize = 8; break;
        case GPUTextureFormat::RG32Sint:  dxgi = DXGI_FORMAT_R32G32_SINT;  dxgitypeless = DXGI_FORMAT_R32G32_TYPELESS; byteSize = 8; break;
        case GPUTextureFormat::RG32Float: dxgi = DXGI_FORMAT_R32G32_FLOAT; dxgitypeless = DXGI_FORMAT_R32G32_TYPELESS; byteSize = 8; break;

        case GPUTextureFormat::RGBA8Unorm:  dxgi = DXGI_FORMAT_R8G8B8A8_UNORM;     dxgitypeless = DXGI_FORMAT_R8G8B8A8_TYPELESS;     byteSize = 4; break;
        case GPUTextureFormat::RGBA8Snorm:  dxgi = DXGI_FORMAT_R8G8B8A8_SNORM;     dxgitypeless = DXGI_FORMAT_R8G8B8A8_TYPELESS;     byteSize = 4; break;
        case GPUTextureFormat::RGBA8Sint:   dxgi = DXGI_FORMAT_R8G8B8A8_SINT;      dxgitypeless = DXGI_FORMAT_R8G8B8A8_TYPELESS;     byteSize = 4; break;
        case GPUTextureFormat::RGBA8Uint:   dxgi = DXGI_FORMAT_R8G8B8A8_UINT;      dxgitypeless = DXGI_FORMAT_R8G8B8A8_TYPELESS;     byteSize = 4; break;
        case GPUTextureFormat::RGBA16Unorm: dxgi = DXGI_FORMAT_R16G16B16A16_UNORM; dxgitypeless = DXGI_FORMAT_R16G16B16A16_TYPELESS; byteSize = 8; break;
        case GPUTextureFormat::RGBA16Snorm: dxgi = DXGI_FORMAT_R16G16B16A16_SNORM; dxgitypeless = DXGI_FORMAT_R16G16B16A16_TYPELESS; byteSize = 8; break;
        case GPUTextureFormat::RGBA16Uint:  dxgi = DXGI_FORMAT_R16G16B16A16_UINT;  dxgitypeless = DXGI_FORMAT_R16G16B16A16_TYPELESS; byteSize = 8; break;
        case GPUTextureFormat::RGBA16Sint:  dxgi = DXGI_FORMAT_R16G16B16A16_SINT;  dxgitypeless = DXGI_FORMAT_R16G16B16A16_TYPELESS; byteSize = 8; break;
        case GPUTextureFormat::RGBA16Float: dxgi = DXGI_FORMAT_R16G16B16A16_FLOAT; dxgitypeless = DXGI_FORMAT_R16G16B16A16_TYPELESS; byteSize = 8; break;
        case GPUTextureFormat::RGBA32Uint:  dxgi = DXGI_FORMAT_R32G32B32A32_UINT;  dxgitypeless = DXGI_FORMAT_R32G32B32A32_TYPELESS; byteSize = 16; break;
        case GPUTextureFormat::RGBA32Sint:  dxgi = DXGI_FORMAT_R32G32B32A32_SINT;  dxgitypeless = DXGI_FORMAT_R32G32B32A32_TYPELESS; byteSize = 16; break;
        case GPUTextureFormat::RGBA32Float: dxgi = DXGI_FORMAT_R32G32B32A32_FLOAT; dxgitypeless = DXGI_FORMAT_R32G32B32A32_TYPELESS; byteSize = 16; break;

        case GPUTextureFormat::BC1:  dxgi = DXGI_FORMAT_BC1_UNORM; dxgitypeless = DXGI_FORMAT_BC1_TYPELESS;  byteSize = 0; break;
        case GPUTextureFormat::BC2:  dxgi = DXGI_FORMAT_BC2_UNORM; dxgitypeless = DXGI_FORMAT_BC2_TYPELESS;  byteSize = 0; break;
        case GPUTextureFormat::BC3:  dxgi = DXGI_FORMAT_BC3_UNORM; dxgitypeless = DXGI_FORMAT_BC3_TYPELESS;  byteSize = 0; break;
        case GPUTextureFormat::BC4:  dxgi = DXGI_FORMAT_BC4_UNORM; dxgitypeless = DXGI_FORMAT_BC4_TYPELESS;  byteSize = 0; break;
        case GPUTextureFormat::BC5:  dxgi = DXGI_FORMAT_BC5_UNORM; dxgitypeless = DXGI_FORMAT_BC5_TYPELESS;  byteSize = 0; break;
        case GPUTextureFormat::BC6H: dxgi = DXGI_FORMAT_BC6H_UF16; dxgitypeless = DXGI_FORMAT_BC6H_TYPELESS; byteSize = 0; break;
        case GPUTextureFormat::BC7:  dxgi = DXGI_FORMAT_BC7_UNORM; dxgitypeless = DXGI_FORMAT_BC7_TYPELESS;  byteSize = 0; break;

        default: assert(false); return;
        }
    }
#endif // _WIN32

} // namespace ist

