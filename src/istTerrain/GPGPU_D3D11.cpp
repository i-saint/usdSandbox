#include "pch.h"
#include "GPGPU.h"
#include "GPGPU_internal.h"

#ifdef _WIN32
#include <d3d11.h>
#include <d3d11_4.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <vector>
#include <algorithm>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

using Microsoft::WRL::ComPtr;

namespace ist::d3d11 {

    static constexpr uint32_t kMaxCbvSlots = 16;
    static constexpr uint32_t kMaxSrvSlots = 16;
    // D3D11.1+ CS supports 64 UAV slots; we cap at 16 to match other backends
    static constexpr uint32_t kMaxUavSlots = 16;

    class GPUDevice;


    class GPUBuffer : public IGPUBuffer
    {
    public:
        // Constant buffer ctor
        GPUBuffer(GPUDevice* dev, uint32_t size, const void* initData);
        // Structured buffer ctor
        GPUBuffer(GPUDevice* dev, uint32_t elementSize, uint32_t elementCount, const void* initData);
        void Release() override { delete this; }

        uint32_t GetElementSize()  const override { return _elementSize; }
        uint32_t GetElementCount() const override { return _elementCount; }
        bool     IsConstant()      const { return _isConstant; }
        uint32_t ByteSize()        const { return _elementSize * _elementCount; }

        ID3D11Buffer*              D3DBuffer() const { return _buffer.Get(); }
        ID3D11ShaderResourceView*  SRV()       const { return _srv.Get(); }
        ID3D11UnorderedAccessView* UAV()       const { return _uav.Get(); }

    private:
        uint32_t _elementSize  = 0;
        uint32_t _elementCount = 0;
        bool     _isConstant   = false;

        ComPtr<ID3D11Buffer>              _buffer;
        ComPtr<ID3D11ShaderResourceView>  _srv;
        ComPtr<ID3D11UnorderedAccessView> _uav;
    };

    class GPUTexture2D : public IGPUTexture2D
    {
    public:
        GPUTexture2D(GPUDevice* dev, uint32_t width, uint32_t height, uint32_t arraySize,
                     GPUTextureFormat format, const void* const* initData);
        void Release() override { delete this; }

        uint32_t GetWidth()     const override { return _width; }
        uint32_t GetHeight()    const override { return _height; }
        uint32_t GetArraySize() const override { return _arraySize; }

        ID3D11Texture2D*           D3DTexture() const { return _texture.Get(); }
        ID3D11ShaderResourceView*  SRV()        const { return _srv.Get(); }
        ID3D11UnorderedAccessView* UAV()        const { return _uav.Get(); }

    private:
        uint32_t         _width     = 0;
        uint32_t         _height    = 0;
        uint32_t         _arraySize = 0;

        ComPtr<ID3D11Texture2D>           _texture;
        ComPtr<ID3D11ShaderResourceView>  _srv;
        ComPtr<ID3D11UnorderedAccessView> _uav;
    };

    class GPUComputeShader : public IGPUComputeShader
    {
    public:
        GPUComputeShader(ID3D11Device* device, const void* bytecode, size_t size);
        void Release() override { delete this; }

        ID3D11ComputeShader* CS() const { return _cs.Get(); }

    private:
        ComPtr<ID3D11ComputeShader> _cs;
    };

    struct ReadbackEntry
    {
        ComPtr<ID3D11Buffer>    stagingBuffer;   // for buffer readback
        ComPtr<ID3D11Texture2D> stagingTexture;  // for texture readback
        uint32_t                numSlices = 0;

        IGPUContext::BufferReadCallback         bufCb;
        IGPUContext::Texture2DReadCallback      texCb;
        IGPUContext::Texture2DArrayReadCallback texArrCb;
    };

    class GPUContext : public IGPUContext
    {
    public:
        explicit GPUContext(GPUDevice* dev);
        void Release() override { delete this; }

        void SetShader(IGPUComputeShaderPtr shader) override;
        void SetConstants(uint32_t slot, IGPUBufferPtr buffer) override;
        void SetSrv(uint32_t slot, IGPUResourcePtr resource) override;
        void SetUav(uint32_t slot, IGPUResourcePtr resource) override;
        void Dispatch(uint32_t x, uint32_t y, uint32_t z) override;

        void ReadBuffer(IGPUBufferPtr buffer, const BufferReadCallback& cb) override;
        void ReadTexture2D(IGPUTexture2DPtr texture, const Texture2DReadCallback& cb) override;
        void ReadTexture2DArray(IGPUTexture2DPtr texture, const Texture2DArrayReadCallback& cb) override;

        void Flush() override;
        void Finish() override;
        bool IsComplete() override;
        void Reset() override;

    private:
        GPUDevice* _dev = nullptr;

        IGPUComputeShaderPtr _shader;
        IGPUBufferPtr   _cbvSlots[kMaxCbvSlots] = {};
        IGPUResourcePtr _srvSlots[kMaxSrvSlots] = {};
        IGPUResourcePtr _uavSlots[kMaxUavSlots] = {};

        std::vector<ReadbackEntry> _readbacks;
        uint64_t _signalValue = 0;
        bool     _flushed     = false;
    };

    class GPUDevice : public IGPUDevice
    {
    public:
        GPUDevice();
        ~GPUDevice() override = default;
        void Release() override { delete this; }

        IGPUBufferPtr        CreateConstantBuffer(size_t size, const void* data) override;
        IGPUBufferPtr        CreateStructuredBuffer(size_t elementSize, size_t elementCount, const void* data) override;
        IGPUTexture2DPtr     CreateTexture2D(uint32_t width, uint32_t height, GPUTextureFormat format, const void* data) override;
        IGPUTexture2DPtr     CreateTexture2DArray(uint32_t width, uint32_t height, uint32_t arraySize, GPUTextureFormat format, const void* data[]) override;
        IGPUComputeShaderPtr CreateComputeShader(const void* bytecode, size_t size) override;
        IGPUContextPtr       CreateContext() override;

        ID3D11Device5*        Dev()   const { return _device.Get(); }
        ID3D11DeviceContext4* Ctx()   const { return _context.Get(); }
        ID3D11Fence*          Fence() const { return _fence.Get(); }
        uint64_t              SignalNext() { return ++_fenceVal; }

    private:
        ComPtr<ID3D11Device5>        _device;
        ComPtr<ID3D11DeviceContext4> _context;
        ComPtr<ID3D11Fence>          _fence;
        uint64_t                     _fenceVal = 0;
    };


#pragma region GPUBuffer
    GPUBuffer::GPUBuffer(GPUDevice* dev, uint32_t size, const void* initData)
        : _elementSize(size), _elementCount(1), _isConstant(true)
    {
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth  = (size + 15u) & ~15u; // D3D11 constant buffers must be multiples of 16 B
        bd.Usage      = D3D11_USAGE_DEFAULT;
        bd.BindFlags  = D3D11_BIND_CONSTANT_BUFFER;

        if (initData) {
            D3D11_SUBRESOURCE_DATA srd{ initData };
            dev->Dev()->CreateBuffer(&bd, &srd, &_buffer);
        } else {
            dev->Dev()->CreateBuffer(&bd, nullptr, &_buffer);
        }
    }

    GPUBuffer::GPUBuffer(GPUDevice* dev, uint32_t elementSize, uint32_t elementCount, const void* initData)
        : _elementSize(elementSize), _elementCount(elementCount), _isConstant(false)
    {
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth           = elementSize * elementCount;
        bd.Usage               = D3D11_USAGE_DEFAULT;
        bd.BindFlags           = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
        bd.MiscFlags           = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
        bd.StructureByteStride = elementSize;

        if (initData) {
            D3D11_SUBRESOURCE_DATA srd{ initData };
            dev->Dev()->CreateBuffer(&bd, &srd, &_buffer);
        } else {
            dev->Dev()->CreateBuffer(&bd, nullptr, &_buffer);
        }

        D3D11_SHADER_RESOURCE_VIEW_DESC svd{};
        svd.Format              = DXGI_FORMAT_UNKNOWN;
        svd.ViewDimension       = D3D11_SRV_DIMENSION_BUFFER;
        svd.Buffer.FirstElement = 0;
        svd.Buffer.NumElements  = elementCount;
        dev->Dev()->CreateShaderResourceView(_buffer.Get(), &svd, &_srv);

        D3D11_UNORDERED_ACCESS_VIEW_DESC uvd{};
        uvd.Format              = DXGI_FORMAT_UNKNOWN;
        uvd.ViewDimension       = D3D11_UAV_DIMENSION_BUFFER;
        uvd.Buffer.FirstElement = 0;
        uvd.Buffer.NumElements  = elementCount;
        dev->Dev()->CreateUnorderedAccessView(_buffer.Get(), &uvd, &_uav);
    }
#pragma endregion GPUBuffer

#pragma region GPUTexture2D
    GPUTexture2D::GPUTexture2D(GPUDevice* dev, uint32_t width, uint32_t height, uint32_t arraySize,
                               GPUTextureFormat format, const void* const* initData)
        : _width(width), _height(height), _arraySize(arraySize)
    {
        DXGI_FORMAT dxgi, typeless;
        size_t pixelSize;
        GetDXGIFormat(format, dxgi, typeless, pixelSize);

        D3D11_TEXTURE2D_DESC td{};
        td.Width            = width;
        td.Height           = height;
        td.MipLevels        = 1;
        td.ArraySize        = arraySize;
        td.Format           = dxgi;
        td.SampleDesc.Count = 1;
        td.Usage            = D3D11_USAGE_DEFAULT;
        td.BindFlags        = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;

        if (initData && pixelSize > 0) {
            std::vector<D3D11_SUBRESOURCE_DATA> srds(arraySize);
            for (uint32_t i = 0; i < arraySize; ++i) {
                srds[i].pSysMem     = initData[i];
                srds[i].SysMemPitch = static_cast<UINT>(width * pixelSize);
            }
            dev->Dev()->CreateTexture2D(&td, srds.data(), &_texture);
        } else {
            dev->Dev()->CreateTexture2D(&td, nullptr, &_texture);
        }

        D3D11_SHADER_RESOURCE_VIEW_DESC svd{};
        svd.Format = dxgi;
        if (arraySize == 1) {
            svd.ViewDimension           = D3D11_SRV_DIMENSION_TEXTURE2D;
            svd.Texture2D.MipLevels     = 1;
        } else {
            svd.ViewDimension              = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
            svd.Texture2DArray.MipLevels   = 1;
            svd.Texture2DArray.ArraySize   = arraySize;
        }
        dev->Dev()->CreateShaderResourceView(_texture.Get(), &svd, &_srv);

        D3D11_UNORDERED_ACCESS_VIEW_DESC uvd{};
        uvd.Format = dxgi;
        if (arraySize == 1) {
            uvd.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
        } else {
            uvd.ViewDimension              = D3D11_UAV_DIMENSION_TEXTURE2DARRAY;
            uvd.Texture2DArray.ArraySize   = arraySize;
        }
        // Note: UAV creation may fail for formats that don't support typed UAV (e.g. BC*).
        // The UAV will remain null in that case; the resource can still be used as SRV.
        dev->Dev()->CreateUnorderedAccessView(_texture.Get(), &uvd, &_uav);
    }
#pragma endregion GPUTexture2D

#pragma region GPUComputeShader
    GPUComputeShader::GPUComputeShader(ID3D11Device* device, const void* bytecode, size_t size)
    {
        device->CreateComputeShader(bytecode, size, nullptr, &_cs);
    }
#pragma endregion GPUComputeShader

#pragma region GPUContext
    GPUContext::GPUContext(GPUDevice* dev) : _dev(dev) {}

    void GPUContext::SetShader(IGPUComputeShaderPtr shader)
    {
        _shader = std::move(shader);
    }

    void GPUContext::SetConstants(uint32_t slot, IGPUBufferPtr buffer)
    {
        if (slot < kMaxCbvSlots) _cbvSlots[slot] = buffer;
    }

    void GPUContext::SetSrv(uint32_t slot, IGPUResourcePtr resource)
    {
        if (slot < kMaxSrvSlots) _srvSlots[slot] = resource;
    }

    void GPUContext::SetUav(uint32_t slot, IGPUResourcePtr resource)
    {
        if (slot < kMaxUavSlots) _uavSlots[slot] = resource;
    }

    void GPUContext::Dispatch(uint32_t x, uint32_t y, uint32_t z)
    {
        auto* ctx    = _dev->Ctx();
        auto* shader = static_cast<GPUComputeShader*>(_shader.get());
        if (!shader) return;

        // Unbind all CS slots first to avoid SRV/UAV binding conflicts.
        // D3D11 silently clears conflicting bindings but the debug layer warns about them;
        // clearing everything up-front is the simplest way to suppress those warnings.
        {
            ID3D11ShaderResourceView*  nullSrvs[kMaxSrvSlots] = {};
            ID3D11UnorderedAccessView* nullUavs[kMaxUavSlots] = {};
            ID3D11Buffer*              nullCbs [kMaxCbvSlots]  = {};
            UINT initCounts[kMaxUavSlots];
            std::fill(std::begin(initCounts), std::end(initCounts), static_cast<UINT>(-1));
            ctx->CSSetShaderResources(0, kMaxSrvSlots, nullSrvs);
            ctx->CSSetUnorderedAccessViews(0, kMaxUavSlots, nullUavs, initCounts);
            ctx->CSSetConstantBuffers(0, kMaxCbvSlots, nullCbs);
        }

        ctx->CSSetShader(shader->CS(), nullptr, 0);

        // Bind constant buffers (b-registers)
        {
            ID3D11Buffer* cbs[kMaxCbvSlots] = {};
            for (uint32_t i = 0; i < kMaxCbvSlots; ++i)
                if (auto* buf = static_cast<GPUBuffer*>(_cbvSlots[i].get()))
                    cbs[i] = buf->D3DBuffer();
            ctx->CSSetConstantBuffers(0, kMaxCbvSlots, cbs);
        }

        // Bind SRVs (t-registers)
        {
            ID3D11ShaderResourceView* srvs[kMaxSrvSlots] = {};
            for (uint32_t i = 0; i < kMaxSrvSlots; ++i) {
                auto* res = _srvSlots[i].get();
                if (!res) continue;
                if (auto* buf = dynamic_cast<GPUBuffer*>(res))
                    srvs[i] = buf->SRV();
                else if (auto* tex = dynamic_cast<GPUTexture2D*>(res))
                    srvs[i] = tex->SRV();
            }
            ctx->CSSetShaderResources(0, kMaxSrvSlots, srvs);
        }

        // Bind UAVs (u-registers)
        {
            ID3D11UnorderedAccessView* uavs[kMaxUavSlots] = {};
            UINT initCounts[kMaxUavSlots];
            std::fill(std::begin(initCounts), std::end(initCounts), static_cast<UINT>(-1));
            for (uint32_t i = 0; i < kMaxUavSlots; ++i) {
                auto* res = _uavSlots[i].get();
                if (!res) continue;
                if (auto* buf = dynamic_cast<GPUBuffer*>(res))
                    uavs[i] = buf->UAV();
                else if (auto* tex = dynamic_cast<GPUTexture2D*>(res))
                    uavs[i] = tex->UAV();
            }
            ctx->CSSetUnorderedAccessViews(0, kMaxUavSlots, uavs, initCounts);
        }

        ctx->Dispatch(x, y, z);
    }

    void GPUContext::ReadBuffer(IGPUBufferPtr buffer, const BufferReadCallback& cb)
    {
        auto* buf = static_cast<GPUBuffer*>(buffer.get());

        // Create a staging copy from the same description, minus bind flags
        D3D11_BUFFER_DESC bd{};
        buf->D3DBuffer()->GetDesc(&bd);
        bd.Usage          = D3D11_USAGE_STAGING;
        bd.BindFlags      = 0;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        bd.MiscFlags      = 0;

        ReadbackEntry entry;
        _dev->Dev()->CreateBuffer(&bd, nullptr, &entry.stagingBuffer);
        _dev->Ctx()->CopyResource(entry.stagingBuffer.Get(), buf->D3DBuffer());
        entry.numSlices = 1;
        entry.bufCb     = cb;
        _readbacks.push_back(std::move(entry));
    }

    void GPUContext::ReadTexture2D(IGPUTexture2DPtr texture, const Texture2DReadCallback& cb)
    {
        auto* tex = static_cast<GPUTexture2D*>(texture.get());

        D3D11_TEXTURE2D_DESC td{};
        tex->D3DTexture()->GetDesc(&td);
        td.Usage          = D3D11_USAGE_STAGING;
        td.BindFlags      = 0;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        td.MiscFlags      = 0;

        ReadbackEntry entry;
        _dev->Dev()->CreateTexture2D(&td, nullptr, &entry.stagingTexture);
        _dev->Ctx()->CopyResource(entry.stagingTexture.Get(), tex->D3DTexture());
        entry.numSlices = 1;
        entry.texCb     = cb;
        _readbacks.push_back(std::move(entry));
    }

    void GPUContext::ReadTexture2DArray(IGPUTexture2DPtr texture, const Texture2DArrayReadCallback& cb)
    {
        auto* tex = static_cast<GPUTexture2D*>(texture.get());

        D3D11_TEXTURE2D_DESC td{};
        tex->D3DTexture()->GetDesc(&td);
        td.Usage          = D3D11_USAGE_STAGING;
        td.BindFlags      = 0;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        td.MiscFlags      = 0;

        ReadbackEntry entry;
        _dev->Dev()->CreateTexture2D(&td, nullptr, &entry.stagingTexture);
        _dev->Ctx()->CopyResource(entry.stagingTexture.Get(), tex->D3DTexture());
        entry.numSlices = tex->GetArraySize();
        entry.texArrCb  = cb;
        _readbacks.push_back(std::move(entry));
    }

    void GPUContext::Flush()
    {
        _dev->Ctx()->Flush();
        _signalValue = _dev->SignalNext();
        _dev->Ctx()->Signal(_dev->Fence(), _signalValue);
        _flushed = true;
    }

    void GPUContext::Finish()
    {
        // Auto-flush if the caller skipped it
        if (!_flushed) Flush();

        // CPU-wait for the fence
        if (_dev->Fence()->GetCompletedValue() < _signalValue) {
            AutoEvent ev;
            _dev->Fence()->SetEventOnCompletion(_signalValue, ev.Get());
            ev.Wait();
        }

        // Map staging resources and invoke callbacks
        auto* ctx = _dev->Ctx();
        for (auto& entry : _readbacks) {
            if (entry.bufCb) {
                D3D11_MAPPED_SUBRESOURCE ms{};
                ctx->Map(entry.stagingBuffer.Get(), 0, D3D11_MAP_READ, 0, &ms);
                entry.bufCb(ms.pData);
                ctx->Unmap(entry.stagingBuffer.Get(), 0);
            } else if (entry.texCb) {
                D3D11_MAPPED_SUBRESOURCE ms{};
                ctx->Map(entry.stagingTexture.Get(), 0, D3D11_MAP_READ, 0, &ms);
                entry.texCb(ms.pData, static_cast<int>(ms.RowPitch));
                ctx->Unmap(entry.stagingTexture.Get(), 0);
            } else if (entry.texArrCb) {
                for (uint32_t i = 0; i < entry.numSlices; ++i) {
                    D3D11_MAPPED_SUBRESOURCE ms{};
                    // Subresource index = MipSlice + MipLevels * ArraySlice; MipLevels=1 here
                    ctx->Map(entry.stagingTexture.Get(), i, D3D11_MAP_READ, 0, &ms);
                    entry.texArrCb(ms.pData, static_cast<int>(ms.RowPitch), static_cast<int>(i));
                    ctx->Unmap(entry.stagingTexture.Get(), i);
                }
            }
        }
        _readbacks.clear();
        _flushed = false;
    }

    bool GPUContext::IsComplete()
    {
        return _dev->Fence()->GetCompletedValue() >= _signalValue;
    }

    void GPUContext::Reset()
    {
        for (auto& s : _cbvSlots) s = nullptr;
        for (auto& s : _srvSlots) s = nullptr;
        for (auto& s : _uavSlots) s = nullptr;
        _shader  = nullptr;
        _flushed = false;
    }
#pragma endregion GPUContext

#pragma region GPUDevice
    GPUDevice::GPUDevice()
    {
        ComPtr<IDXGIFactory6> factory;
        ::CreateDXGIFactory1(IID_PPV_ARGS(&factory));

        ComPtr<IDXGIAdapter1> adapter;
        factory->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter));

        D3D_FEATURE_LEVEL featureLevels[] = { D3D_FEATURE_LEVEL_11_1 };
        D3D_FEATURE_LEVEL actualLevel;

        ComPtr<ID3D11Device>        rawDevice;
        ComPtr<ID3D11DeviceContext> rawContext;
        ::D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_HARDWARE, nullptr,
            0, featureLevels, 1, D3D11_SDK_VERSION,
            &rawDevice, &actualLevel, &rawContext);

        rawDevice.As(&_device);
        rawContext.As(&_context);
        _device->CreateFence(0, D3D11_FENCE_FLAG_NONE, IID_PPV_ARGS(&_fence));
    }

    IGPUBufferPtr GPUDevice::CreateConstantBuffer(size_t size, const void* data)
    {
        return MakeRef<GPUBuffer>(this, static_cast<uint32_t>(size), data);
    }

    IGPUBufferPtr GPUDevice::CreateStructuredBuffer(size_t elementSize, size_t elementCount, const void* data)
    {
        return MakeRef<GPUBuffer>(this, static_cast<uint32_t>(elementSize), static_cast<uint32_t>(elementCount), data);
    }

    IGPUTexture2DPtr GPUDevice::CreateTexture2D(uint32_t width, uint32_t height, GPUTextureFormat format, const void* data)
    {
        const void* slices[1] = { data };
        return MakeRef<GPUTexture2D>(this, width, height, 1u, format, data ? slices : nullptr);
    }

    IGPUTexture2DPtr GPUDevice::CreateTexture2DArray(uint32_t width, uint32_t height, uint32_t arraySize, GPUTextureFormat format, const void* data[])
    {
        return MakeRef<GPUTexture2D>(this, width, height, arraySize, format, data);
    }

    IGPUComputeShaderPtr GPUDevice::CreateComputeShader(const void* bytecode, size_t size)
    {
        return MakeRef<GPUComputeShader>(_device.Get(), bytecode, size);
    }

    IGPUContextPtr GPUDevice::CreateContext()
    {
        return MakeRef<GPUContext>(this);
    }
#pragma endregion GPUDevice


    IGPUDevicePtr CreateGPUDevice()
    {
        auto ret = MakeRef<GPUDevice>();
        if (!ret->Dev()) return nullptr;
        return ret;
    }

} // namespace ist::d3d11
#endif // _WIN32


namespace ist {

    IGPUDevicePtr CreateGPUDevice_D3D11()
    {
#ifdef _WIN32
        return d3d11::CreateGPUDevice();
#else
        return nullptr;
#endif
    }

} // namespace ist
