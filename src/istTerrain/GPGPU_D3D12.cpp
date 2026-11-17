#include "pch.h"
#include "GPGPU.h"
#include "GPGPU_internal.h"

#ifdef _WIN32
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <vector>

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")

using Microsoft::WRL::ComPtr;

namespace ist::d3d12 {

    static constexpr uint32_t kMaxCbvSlots = 16;
    static constexpr uint32_t kMaxSrvSlots = 16;
    static constexpr uint32_t kMaxUavSlots = 16;
    static constexpr uint32_t kDescPerDispatch = kMaxCbvSlots + kMaxSrvSlots + kMaxUavSlots;
    static constexpr uint32_t kCbvBlockOffset = 0;
    static constexpr uint32_t kSrvBlockOffset = kMaxCbvSlots;
    static constexpr uint32_t kUavBlockOffset = kMaxCbvSlots + kMaxSrvSlots;

    static constexpr uint32_t kCPUHeapCap = 2048;
    static constexpr uint32_t kGPUHeapCap = 4096;

    // Indices in the CPU heap reserved for null descriptors
    static constexpr uint32_t kNullCbvIdx = 0;
    static constexpr uint32_t kNullSrvIdx = 1;
    static constexpr uint32_t kNullUavIdx = 2;
    static constexpr uint32_t kUserDescBase = 3;

    // Root parameter indices
    static constexpr uint32_t kRpCbv = 0;
    static constexpr uint32_t kRpSrv = 1;
    static constexpr uint32_t kRpUav = 2;

    class GPUDevice;


    class GPUResourceBase
    {
    public:
        virtual ~GPUResourceBase() = default;

        ID3D12Resource* Resource() const { return _resource.Get(); }
        D3D12_RESOURCE_STATES GetState() const { return _state; }
        void SetState(D3D12_RESOURCE_STATES s) { _state = s; }

        uint32_t CbvIdx() const { return _cbvIdx; }
        uint32_t SrvIdx() const { return _srvIdx; }
        uint32_t UavIdx() const { return _uavIdx; }

    protected:
        ComPtr<ID3D12Resource>  _resource;
        D3D12_RESOURCE_STATES   _state = D3D12_RESOURCE_STATE_COMMON;

        uint32_t _cbvIdx = kNullCbvIdx;
        uint32_t _srvIdx = kNullSrvIdx;
        uint32_t _uavIdx = kNullUavIdx;
    };


    class GPUBuffer : public IGPUBuffer, public GPUResourceBase
    {
    public:
        GPUBuffer(GPUDevice* dev, uint32_t size, const void* initData);
        GPUBuffer(GPUDevice* dev, uint32_t elementSize, uint32_t elementCount, const void* initData);
        void Release() override { delete this; }

        uint32_t GetElementSize() const override { return _elementSize; }
        uint32_t GetElementCount() const override { return _elementCount; }
        bool     IsConstant() const { return _isConstant; }
        uint32_t ByteSize() const { return _elementSize * _elementCount; }


    private:
        uint32_t _elementSize = 0;
        uint32_t _elementCount = 0;
        bool     _isConstant = false;
    };

    class GPUTexture2D : public IGPUTexture2D, public GPUResourceBase
    {
    public:
        static constexpr uint32_t    kBytesPerPixel = 16;

        GPUTexture2D(GPUDevice* dev, uint32_t width, uint32_t height, uint32_t arraySize, GPUTextureFormat format, const void* const* initData);
        void Release() override { delete this; }

        uint32_t GetWidth() const override { return _width; }
        uint32_t GetHeight() const override { return _height; }
        uint32_t GetArraySize() const override { return _arraySize; }

    private:
        uint32_t _width = 0;
        uint32_t _height = 0;
        uint32_t _arraySize = 0;
        GPUTextureFormat _format{};
    };

    class GPUComputeShader : public IGPUComputeShader
    {
    public:
        GPUComputeShader(GPUDevice* dev, ID3D12RootSignature* rootSig, const void* bytecode, size_t size);
        void Release() override { delete this; }

        ID3D12PipelineState* PSO() const { return _pso.Get(); }

    private:
        ComPtr<ID3D12PipelineState> _pso;
    };

    struct ReadbackEntry
    {
        ComPtr<ID3D12Resource> resource;
        uint32_t rowPitch = 0;
        uint32_t numSlices = 0;
        size_t   sliceSize = 0;   // bytes per slice in the readback buffer

        IGPUContext::BufferReadCallback        bufCb;
        IGPUContext::Texture2DReadCallback     texCb;
        IGPUContext::Texture2DArrayReadCallback texArrCb;
    };

    class GPUContext : public IGPUContext
    {
    public:
        explicit GPUContext(GPUDevice* dev);
        ~GPUContext() override;
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
        void     EnsureOpen();
        void     EnsureState(GPUResourceBase* res, D3D12_RESOURCE_STATES desired);
        uint32_t AllocGPUBlock();
        D3D12_GPU_DESCRIPTOR_HANDLE GPUHandle(uint32_t idx) const;
        void CopyToGPU(uint32_t cpuIdx, uint32_t gpuIdx);
        ComPtr<ID3D12Resource> CreateReadbackBuffer(size_t byteSize);

        GPUDevice* _dev = nullptr;

        ComPtr<ID3D12CommandAllocator> _alloc;
        ComPtr<ID3D12GraphicsCommandList> _cmdList;
        ComPtr<ID3D12Fence> _fence;
        uint64_t _fenceVal = 0;
        AutoEvent _fenceEvent;

        // GPU-visible CBV/SRV/UAV heap (ring buffer, reset on Reset())
        ComPtr<ID3D12DescriptorHeap> _gpuHeap;
        uint32_t                     _gpuHead = 0;

        IGPUComputeShaderPtr _shader;
        IGPUResourcePtr _cbvSlots[kMaxCbvSlots] = {};
        IGPUResourcePtr _srvSlots[kMaxSrvSlots] = {};
        IGPUResourcePtr _uavSlots[kMaxUavSlots] = {};

        std::vector<ReadbackEntry> _readbacks;
        bool _opened = false;
    };

    class GPUDevice : public IGPUDevice
    {
    public:
        explicit GPUDevice();
        ~GPUDevice() override;
        void Release() override { delete this; }

        IGPUBufferPtr CreateConstantBuffer(size_t size, const void* data) override;
        IGPUBufferPtr CreateStructuredBuffer(size_t elementSize, size_t elementCount, const void* data) override;
        IGPUTexture2DPtr CreateTexture2D(uint32_t width, uint32_t height, GPUTextureFormat format, const void* data) override;
        IGPUTexture2DPtr CreateTexture2DArray(uint32_t width, uint32_t height, uint32_t arraySize, GPUTextureFormat format, const void* data[]) override;
        IGPUComputeShaderPtr CreateComputeShader(const void* bytecode, size_t size) override;
        IGPUContextPtr CreateContext() override;

        ID3D12Device* D() const { return _device.Get(); }
        ID3D12CommandQueue* Queue() const { return _queue.Get(); }
        ID3D12RootSignature* RootSig() const { return _rootSig.Get(); }
        uint32_t DescSize() const { return _descSize; }

        uint32_t AllocCPUDesc();
        D3D12_CPU_DESCRIPTOR_HANDLE CPUHandle(uint32_t idx) const;

        // Upload helpers (synchronous)
        void UploadBuffer(ID3D12Resource* dest, D3D12_RESOURCE_STATES finalState, const void* data, size_t size);
        void UploadTexture(ID3D12Resource* dest, D3D12_RESOURCE_STATES finalState, uint32_t width, uint32_t height, uint32_t arraySize, const void* const* slices);

    private:
        void InitRootSignature();
        void InitNullDescriptors();
        void ExecUploadAndWait();

        ComPtr<ID3D12Device>       _device;
        ComPtr<ID3D12CommandQueue> _queue;
        ComPtr<ID3D12RootSignature> _rootSig;
        ComPtr<ID3D12DescriptorHeap> _cpuHeap;
        uint32_t _cpuHead = 0;
        uint32_t _descSize = 0;

        // Dedicated copy queue + command list for resource uploads
        ComPtr<ID3D12CommandQueue>            _copyQueue;
        ComPtr<ID3D12CommandAllocator>        _copyAlloc;
        ComPtr<ID3D12GraphicsCommandList>     _copyList;
        ComPtr<ID3D12Fence>                   _copyFence;
        uint64_t                              _copyFenceVal = 0;
        HANDLE                                _copyEvent = nullptr;
        std::vector<ComPtr<ID3D12Resource>>   _copyTemps;   // kept alive until ExecUploadAndWait
        bool                                  _copyListOpen = false;
    };


#pragma region Utils
    static D3D12_RESOURCE_BARRIER MakeTransition(ID3D12Resource* res, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
    {
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = res;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = before;
        b.Transition.StateAfter = after;
        return b;
    }
#pragma endregion Utils

#pragma region GPUBuffer
    GPUBuffer::GPUBuffer(GPUDevice* dev, uint32_t size, const void* initData)
        : _elementSize(size), _elementCount(1), _isConstant(true)
    {
        auto* d = dev->D();
        uint32_t rawSize = _elementSize;

        // Constant buffers live on the upload heap (permanently GENERIC_READ)
        uint32_t alignedSize = (rawSize + 255u) & ~255u;

        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_UPLOAD;

        D3D12_RESOURCE_DESC rd{};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width = alignedSize;
        rd.Height = rd.DepthOrArraySize = rd.MipLevels = 1;
        rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        d->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&_resource));
        _state = D3D12_RESOURCE_STATE_GENERIC_READ;

        if (initData) {
            void* mapped = nullptr;
            _resource->Map(0, nullptr, &mapped);
            memcpy(mapped, initData, rawSize);
            _resource->Unmap(0, nullptr);
        }

        _cbvIdx = dev->AllocCPUDesc();
        D3D12_CONSTANT_BUFFER_VIEW_DESC cvd{};
        cvd.BufferLocation = _resource->GetGPUVirtualAddress();
        cvd.SizeInBytes = alignedSize;
        d->CreateConstantBufferView(&cvd, dev->CPUHandle(_cbvIdx));
    }

    GPUBuffer::GPUBuffer(GPUDevice* dev, uint32_t elementSize, uint32_t elementCount, const void* initData)
        : _elementSize(elementSize), _elementCount(elementCount), _isConstant(false)
    {
        auto* d = dev->D();
        uint32_t rawSize = _elementSize * _elementCount;

        // Structured buffers live on the default heap
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;

        D3D12_RESOURCE_DESC rd{};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width = rawSize;
        rd.Height = rd.DepthOrArraySize = rd.MipLevels = 1;
        rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        d->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&_resource));
        _state = D3D12_RESOURCE_STATE_COMMON;

        if (initData) {
            dev->UploadBuffer(_resource.Get(), D3D12_RESOURCE_STATE_COMMON, initData, rawSize);
        }

        // SRV
        _srvIdx = dev->AllocCPUDesc();
        D3D12_SHADER_RESOURCE_VIEW_DESC svd{};
        svd.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        svd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        svd.Buffer.FirstElement = 0;
        svd.Buffer.NumElements = elementCount;
        svd.Buffer.StructureByteStride = elementSize;
        d->CreateShaderResourceView(_resource.Get(), &svd, dev->CPUHandle(_srvIdx));

        // UAV
        _uavIdx = dev->AllocCPUDesc();
        D3D12_UNORDERED_ACCESS_VIEW_DESC uvd{};
        uvd.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        uvd.Buffer.FirstElement = 0;
        uvd.Buffer.NumElements = elementCount;
        uvd.Buffer.StructureByteStride = elementSize;
        d->CreateUnorderedAccessView(_resource.Get(), nullptr, &uvd, dev->CPUHandle(_uavIdx));
    }


    GPUTexture2D::GPUTexture2D(GPUDevice* dev, uint32_t width, uint32_t height, uint32_t arraySize, GPUTextureFormat format, const void* const* initData)
        : _width(width), _height(height), _arraySize(arraySize), _format(format)
    {
        auto* d = dev->D();

        DXGI_FORMAT dxgiFormat;
        DXGI_FORMAT dxgiTypeless;
        size_t byteSize;
        GetDXGIFormat(format, dxgiFormat, dxgiTypeless, byteSize);

        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC rd{};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        rd.Width = width;
        rd.Height = height;
        rd.DepthOrArraySize = static_cast<UINT16>(arraySize);
        rd.MipLevels = 1;
        rd.Format = dxgiTypeless;
        rd.SampleDesc.Count = 1;
        rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        d->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&_resource));
        _state = D3D12_RESOURCE_STATE_COMMON;

        if (initData) {
            dev->UploadTexture(_resource.Get(), D3D12_RESOURCE_STATE_COMMON, width, height, arraySize, initData);
        }

        // SRV (all array slices)
        _srvIdx = dev->AllocCPUDesc();
        D3D12_SHADER_RESOURCE_VIEW_DESC svd{};
        svd.Format = dxgiFormat;
        svd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        if (arraySize == 1) {
            svd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            svd.Texture2D.MipLevels = 1;
        }
        else {
            svd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
            svd.Texture2DArray.MipLevels = 1;
            svd.Texture2DArray.ArraySize = arraySize;
        }
        d->CreateShaderResourceView(_resource.Get(), &svd, dev->CPUHandle(_srvIdx));

        // UAV (whole array)
        _uavIdx = dev->AllocCPUDesc();
        D3D12_UNORDERED_ACCESS_VIEW_DESC uvd{};
        uvd.Format = dxgiFormat;
        if (arraySize == 1) {
            uvd.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        }
        else {
            uvd.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
            uvd.Texture2DArray.ArraySize = arraySize;
        }
        d->CreateUnorderedAccessView(_resource.Get(), nullptr, &uvd, dev->CPUHandle(_uavIdx));
    }
#pragma endregion GPUBuffer

#pragma region GPUComputeShader
    GPUComputeShader::GPUComputeShader(GPUDevice* dev, ID3D12RootSignature* rootSig, const void* bytecode, size_t size)
    {
        auto* device = dev->D();
        D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};
        desc.pRootSignature = rootSig;
        desc.CS = { bytecode, size };
        device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&_pso));
    }
#pragma endregion GPUComputeShader

#pragma region GPUContext
    GPUContext::GPUContext(GPUDevice* dev) : _dev(dev)
    {
        auto* d = dev->D();

        d->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COMPUTE, IID_PPV_ARGS(&_alloc));
        d->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COMPUTE, _alloc.Get(), nullptr, IID_PPV_ARGS(&_cmdList));
        _opened = true;   // CreateCommandList leaves the list open

        d->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&_fence));

        // GPU-visible CBV/SRV/UAV heap (shader-visible, ring buffer)
        D3D12_DESCRIPTOR_HEAP_DESC hd{};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        hd.NumDescriptors = kGPUHeapCap;
        hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        d->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&_gpuHeap));
    }

    GPUContext::~GPUContext()
    {}

    void GPUContext::EnsureOpen()
    {
        if (!_opened) {
            _alloc->Reset();
            _cmdList->Reset(_alloc.Get(), nullptr);
            _opened = true;
        }
    }

    void GPUContext::EnsureState(GPUResourceBase* res, D3D12_RESOURCE_STATES desired)
    {
        if (res->GetState() == desired) {
            return;
        }
        auto bar = MakeTransition(res->Resource(), res->GetState(), desired);
        _cmdList->ResourceBarrier(1, &bar);
        res->SetState(desired);
    }

    uint32_t GPUContext::AllocGPUBlock()
    {
        uint32_t base = _gpuHead;
        _gpuHead += kDescPerDispatch;
        if (_gpuHead > kGPUHeapCap) {
            // Wrap around (safe only after Wait() has confirmed GPU is idle)
            _gpuHead = kDescPerDispatch;
            base = 0;
        }
        return base;
    }

    D3D12_GPU_DESCRIPTOR_HANDLE GPUContext::GPUHandle(uint32_t idx) const
    {
        D3D12_GPU_DESCRIPTOR_HANDLE h = _gpuHeap->GetGPUDescriptorHandleForHeapStart();
        h.ptr += static_cast<UINT64>(idx) * _dev->DescSize();
        return h;
    }

    void GPUContext::CopyToGPU(uint32_t cpuIdx, uint32_t gpuIdx)
    {
        D3D12_CPU_DESCRIPTOR_HANDLE src = _dev->CPUHandle(cpuIdx);
        D3D12_CPU_DESCRIPTOR_HANDLE dst = _gpuHeap->GetCPUDescriptorHandleForHeapStart();
        dst.ptr += static_cast<UINT64>(gpuIdx) * _dev->DescSize();
        _dev->D()->CopyDescriptorsSimple(1, dst, src, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    }

    void GPUContext::SetShader(IGPUComputeShaderPtr shader)
    {
        _shader = std::move(shader);
    }

    void GPUContext::SetConstants(uint32_t slot, IGPUBufferPtr buffer)
    {
        if (slot < kMaxCbvSlots) {
            _cbvSlots[slot] = buffer;
        }
    }

    void GPUContext::SetSrv(uint32_t slot, IGPUResourcePtr resource)
    {
        if (slot < kMaxSrvSlots) {
            _srvSlots[slot] = resource;
        }
    }

    void GPUContext::SetUav(uint32_t slot, IGPUResourcePtr resource)
    {
        if (slot < kMaxUavSlots) {
            _uavSlots[slot] = resource;
        }
    }

    void GPUContext::Dispatch(uint32_t x, uint32_t y, uint32_t z)
    {
        EnsureOpen();
        auto* shader = static_cast<GPUComputeShader*>(_shader.get());
        if (!shader) return;

        uint32_t block = AllocGPUBlock();

        // Copy bound CBV slots (set via SetConstants)
        for (uint32_t i = 0; i < kMaxCbvSlots; ++i) {
            uint32_t gpuCbv = block + kCbvBlockOffset + i;
            auto* res = _cbvSlots[i].get();
            if (!res) {
                CopyToGPU(kNullCbvIdx, gpuCbv);
                continue;
            }
            if (auto* r = dynamic_cast<GPUResourceBase*>(res)) {
                // Constant buffers live on upload heap (permanently GENERIC_READ); no transition needed
                CopyToGPU(r->CbvIdx(), gpuCbv);
            }
        }

        // Copy bound SRV slots (set via SetSrv)
        for (uint32_t i = 0; i < kMaxSrvSlots; ++i) {
            uint32_t gpuSrv = block + kSrvBlockOffset + i;
            auto* res = _srvSlots[i].get();
            if (!res) {
                CopyToGPU(kNullSrvIdx, gpuSrv);
                continue;
            }
            if (auto* r = dynamic_cast<GPUResourceBase*>(res)) {
                EnsureState(r, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                CopyToGPU(r->SrvIdx(), gpuSrv);
            }
        }

        // Copy bound UAV slots
        for (uint32_t i = 0; i < kMaxUavSlots; ++i) {
            uint32_t gpuUav = block + kUavBlockOffset + i;
            auto* res = _uavSlots[i].get();
            if (!res) {
                CopyToGPU(kNullUavIdx, gpuUav);
                continue;
            }
            if (auto* r = dynamic_cast<GPUResourceBase*>(res)) {
                EnsureState(r, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                CopyToGPU(r->UavIdx(), gpuUav);
            }
        }

        ID3D12DescriptorHeap* heaps[] = { _gpuHeap.Get() };
        _cmdList->SetDescriptorHeaps(1, heaps);
        _cmdList->SetComputeRootSignature(_dev->RootSig());
        _cmdList->SetPipelineState(shader->PSO());
        _cmdList->SetComputeRootDescriptorTable(kRpCbv, GPUHandle(block + kCbvBlockOffset));
        _cmdList->SetComputeRootDescriptorTable(kRpSrv, GPUHandle(block + kSrvBlockOffset));
        _cmdList->SetComputeRootDescriptorTable(kRpUav, GPUHandle(block + kUavBlockOffset));
        _cmdList->Dispatch(x, y, z);

        // Transition UAV-bound resources back to COMMON after dispatch.
        // This acts as a synchronization point (equivalent to a UAV barrier),
        // ensuring writes are visible whether the same resource is reused as
        // UAV (WAW) or SRV (WAR) in a subsequent dispatch.
        {
            D3D12_RESOURCE_BARRIER barriers[kMaxUavSlots];
            uint32_t count = 0;
            for (uint32_t i = 0; i < kMaxUavSlots; ++i) {
                auto* res = _uavSlots[i].get();
                if (!res) {
                    continue;
                }

                auto* r = dynamic_cast<GPUResourceBase*>(res);
                if (!r || r->GetState() != D3D12_RESOURCE_STATE_UNORDERED_ACCESS) {
                    continue;
                }
                barriers[count++] = MakeTransition(r->Resource(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
                r->SetState(D3D12_RESOURCE_STATE_COMMON);
            }
            if (count > 0) {
                _cmdList->ResourceBarrier(count, barriers);
            }
        }
    }

    ComPtr<ID3D12Resource> GPUContext::CreateReadbackBuffer(size_t byteSize)
    {
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_READBACK;

        D3D12_RESOURCE_DESC rd{};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width = byteSize;
        rd.Height = rd.DepthOrArraySize = rd.MipLevels = 1;
        rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        ComPtr<ID3D12Resource> res;
        _dev->D()->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&res));
        return res;
    }

    void GPUContext::ReadBuffer(IGPUBufferPtr buffer, const BufferReadCallback& cb)
    {
        EnsureOpen();

        auto* buf = static_cast<GPUBuffer*>(buffer.get());
        size_t byteSize = buf->ByteSize();

        ReadbackEntry entry;
        entry.resource = CreateReadbackBuffer(byteSize);
        entry.rowPitch = static_cast<uint32_t>(byteSize);
        entry.numSlices = 1;
        entry.sliceSize = byteSize;
        entry.bufCb = cb;

        if (!buf->IsConstant()) {
            // Default-heap structured buffers need explicit COPY_SOURCE transition
            EnsureState(buf, D3D12_RESOURCE_STATE_COPY_SOURCE);
        }
        // Upload-heap constant buffers are always GENERIC_READ (⊇ COPY_SOURCE); no transition needed

        _cmdList->CopyBufferRegion(entry.resource.Get(), 0, buf->Resource(), 0, byteSize);

        if (!buf->IsConstant()) {
            auto bar = MakeTransition(buf->Resource(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            _cmdList->ResourceBarrier(1, &bar);
            buf->SetState(D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        }

        _readbacks.push_back(std::move(entry));
    }

    void GPUContext::ReadTexture2D(IGPUTexture2DPtr texture, const Texture2DReadCallback& cb)
    {
        EnsureOpen();

        auto* tex = static_cast<GPUTexture2D*>(texture.get());

        D3D12_RESOURCE_DESC rd = tex->Resource()->GetDesc();
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout{};
        UINT   numRows = 0;
        UINT64 rowBytes = 0, totalBytes = 0;
        _dev->D()->GetCopyableFootprints(&rd, 0, 1, 0, &layout, &numRows, &rowBytes, &totalBytes);

        ReadbackEntry entry;
        entry.resource = CreateReadbackBuffer(totalBytes);
        entry.rowPitch = layout.Footprint.RowPitch;
        entry.numSlices = 1;
        entry.sliceSize = totalBytes;
        entry.texCb = cb;

        EnsureState(tex, D3D12_RESOURCE_STATE_COPY_SOURCE);

        D3D12_TEXTURE_COPY_LOCATION dst{};
        dst.pResource = entry.resource.Get();
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint = layout;

        D3D12_TEXTURE_COPY_LOCATION src{};
        src.pResource = tex->Resource();
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.SubresourceIndex = 0;
        _cmdList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

        auto bar = MakeTransition(tex->Resource(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        _cmdList->ResourceBarrier(1, &bar);
        tex->SetState(D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        _readbacks.push_back(std::move(entry));
    }

    void GPUContext::ReadTexture2DArray(IGPUTexture2DPtr texture, const Texture2DArrayReadCallback& cb)
    {
        EnsureOpen();

        auto* tex = static_cast<GPUTexture2D*>(texture.get());
        uint32_t array = tex->GetArraySize();

        D3D12_RESOURCE_DESC rd = tex->Resource()->GetDesc();
        std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> layouts(array);
        std::vector<UINT>   numRowsArr(array);
        std::vector<UINT64> rowBytesArr(array);
        UINT64 totalBytes = 0;
        _dev->D()->GetCopyableFootprints(&rd, 0, array, 0, layouts.data(), numRowsArr.data(), rowBytesArr.data(), &totalBytes);

        ReadbackEntry entry;
        entry.resource = CreateReadbackBuffer(totalBytes);
        entry.rowPitch = layouts[0].Footprint.RowPitch;
        entry.numSlices = array;
        entry.sliceSize = static_cast<size_t>(layouts[0].Footprint.RowPitch) * numRowsArr[0];
        entry.texArrCb = cb;

        EnsureState(tex, D3D12_RESOURCE_STATE_COPY_SOURCE);

        for (uint32_t i = 0; i < array; ++i) {
            D3D12_TEXTURE_COPY_LOCATION dst{};
            dst.pResource = entry.resource.Get();
            dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            dst.PlacedFootprint = layouts[i];

            D3D12_TEXTURE_COPY_LOCATION src{};
            src.pResource = tex->Resource();
            src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            src.SubresourceIndex = i;
            _cmdList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        }

        auto bar = MakeTransition(tex->Resource(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        _cmdList->ResourceBarrier(1, &bar);
        tex->SetState(D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        _readbacks.push_back(std::move(entry));
    }

    void GPUContext::Flush()
    {
        if (!_opened) {
            return;
        }

        _cmdList->Close();
        _opened = false;

        ID3D12CommandList* lists[] = { _cmdList.Get() };
        _dev->Queue()->ExecuteCommandLists(1, lists);
        _dev->Queue()->Signal(_fence.Get(), _fenceVal++);
    }

    void GPUContext::Finish()
    {
        Flush();

        if (_fence->GetCompletedValue() < _fenceVal) {
            _fence->SetEventOnCompletion(_fenceVal, _fenceEvent.Get());
            _fenceEvent.Wait();
        }

        for (auto& e : _readbacks) {
            void* mapped = nullptr;
            e.resource->Map(0, nullptr, &mapped);
            if (e.bufCb) {
                e.bufCb(mapped);
            }
            else if (e.texCb) {
                e.texCb(mapped, static_cast<int>(e.rowPitch));
            }
            else if (e.texArrCb) {
                const uint8_t* p = static_cast<const uint8_t*>(mapped);
                for (uint32_t i = 0; i < e.numSlices; ++i) {
                    e.texArrCb(p + i * e.sliceSize, static_cast<int>(e.rowPitch), static_cast<int>(i));
                }
            }
            e.resource->Unmap(0, nullptr);
        }
        _readbacks.clear();
    }

    bool GPUContext::IsComplete()
    {
        return _fence->GetCompletedValue() >= _fenceVal;
    }

    void GPUContext::Reset()
    {
        _gpuHead = 0;
        for (auto& s : _srvSlots) { s = nullptr; }
        for (auto& s : _uavSlots) { s = nullptr; }
        _shader = nullptr;

        if (!_opened) {
            _alloc->Reset();
            _cmdList->Reset(_alloc.Get(), nullptr);
            _opened = true;
        }
    }
#pragma endregion GPUContext

#pragma region GPUDevice
    GPUDevice::GPUDevice()
    {
        ComPtr<IDXGIFactory6> factory;
        CreateDXGIFactory1(IID_PPV_ARGS(&factory));

        ComPtr<IDXGIAdapter1> adapter;
        factory->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter));

        ComPtr<ID3D12Device> device;
        ::D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&_device));

        if (!_device) {
            return;
        }

        _descSize = _device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

        // CPU-only CBV/SRV/UAV heap (non-shader-visible) for resource creation
        D3D12_DESCRIPTOR_HEAP_DESC hd{};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        hd.NumDescriptors = kCPUHeapCap;
        _device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&_cpuHeap));

        // Compute command queue
        D3D12_COMMAND_QUEUE_DESC qd{};
        qd.Type = D3D12_COMMAND_LIST_TYPE_COMPUTE;
        _device->CreateCommandQueue(&qd, IID_PPV_ARGS(&_queue));

        // Copy queue + command list for uploads
        D3D12_COMMAND_QUEUE_DESC cqd{};
        cqd.Type = D3D12_COMMAND_LIST_TYPE_COPY;
        _device->CreateCommandQueue(&cqd, IID_PPV_ARGS(&_copyQueue));
        _device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COPY, IID_PPV_ARGS(&_copyAlloc));
        _device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COPY, _copyAlloc.Get(), nullptr, IID_PPV_ARGS(&_copyList));
        _copyList->Close();

        _device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&_copyFence));
        _copyEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);

        InitRootSignature();
        InitNullDescriptors();
    }

    GPUDevice::~GPUDevice()
    {
        if (_copyEvent) {
            ::CloseHandle(_copyEvent);
        }
    }

    uint32_t GPUDevice::AllocCPUDesc()
    {
        assert(_cpuHead < kCPUHeapCap);
        return _cpuHead++;
    }

    D3D12_CPU_DESCRIPTOR_HANDLE GPUDevice::CPUHandle(uint32_t idx) const
    {
        D3D12_CPU_DESCRIPTOR_HANDLE h = _cpuHeap->GetCPUDescriptorHandleForHeapStart();
        h.ptr += static_cast<UINT64>(idx) * _descSize;
        return h;
    }

    void GPUDevice::InitRootSignature()
    {
        D3D12_DESCRIPTOR_RANGE cbvRange{};
        cbvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_CBV;
        cbvRange.NumDescriptors = kMaxCbvSlots;
        cbvRange.BaseShaderRegister = 0;

        D3D12_DESCRIPTOR_RANGE srvRange{};
        srvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        srvRange.NumDescriptors = kMaxSrvSlots;
        srvRange.BaseShaderRegister = 0;

        D3D12_DESCRIPTOR_RANGE uavRange{};
        uavRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        uavRange.NumDescriptors = kMaxUavSlots;
        uavRange.BaseShaderRegister = 0;

        D3D12_ROOT_PARAMETER params[3]{};
        params[kRpCbv].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[kRpCbv].DescriptorTable.NumDescriptorRanges = 1;
        params[kRpCbv].DescriptorTable.pDescriptorRanges = &cbvRange;
        params[kRpCbv].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        params[kRpSrv].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[kRpSrv].DescriptorTable.NumDescriptorRanges = 1;
        params[kRpSrv].DescriptorTable.pDescriptorRanges = &srvRange;
        params[kRpSrv].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        params[kRpUav].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[kRpUav].DescriptorTable.NumDescriptorRanges = 1;
        params[kRpUav].DescriptorTable.pDescriptorRanges = &uavRange;
        params[kRpUav].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        D3D12_ROOT_SIGNATURE_DESC rsd{};
        rsd.NumParameters = 3;
        rsd.pParameters = params;
        rsd.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

        ComPtr<ID3DBlob> blob, err;
        ::D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err);
        _device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&_rootSig));
    }

    void GPUDevice::InitNullDescriptors()
    {
        // Reserve indices 0-2 for null descriptors (kNullCbvIdx / kNullSrvIdx / kNullUavIdx)
        _cpuHead = kUserDescBase;

        // Null CBV – BufferLocation=0 is accepted as a null descriptor
        D3D12_CONSTANT_BUFFER_VIEW_DESC cvd{};
        _device->CreateConstantBufferView(&cvd, CPUHandle(kNullCbvIdx));

        // Null SRV (typed buffer, pResource=nullptr)
        D3D12_SHADER_RESOURCE_VIEW_DESC svd{};
        svd.Format = DXGI_FORMAT_R32_FLOAT;
        svd.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        svd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        svd.Buffer.NumElements = 1;
        _device->CreateShaderResourceView(nullptr, &svd, CPUHandle(kNullSrvIdx));

        // Null UAV (typed buffer, pResource=nullptr)
        D3D12_UNORDERED_ACCESS_VIEW_DESC uvd{};
        uvd.Format = DXGI_FORMAT_R32_FLOAT;
        uvd.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        uvd.Buffer.NumElements = 1;
        _device->CreateUnorderedAccessView(nullptr, nullptr, &uvd, CPUHandle(kNullUavIdx));
    }

    void GPUDevice::ExecUploadAndWait()
    {
        _copyList->Close();
        ID3D12CommandList* lists[] = { _copyList.Get() };
        _copyQueue->ExecuteCommandLists(1, lists);
        ++_copyFenceVal;
        _copyQueue->Signal(_copyFence.Get(), _copyFenceVal);
        _copyFence->SetEventOnCompletion(_copyFenceVal, _copyEvent);
        ::WaitForSingleObject(_copyEvent, INFINITE);

        _copyTemps.clear();
        _copyAlloc->Reset();
        _copyList->Reset(_copyAlloc.Get(), nullptr);
        _copyList->Close();
        _copyListOpen = false;
    }

    void GPUDevice::UploadBuffer(ID3D12Resource* dest, D3D12_RESOURCE_STATES finalState, const void* data, size_t size)
    {
        // Open the copy list if needed
        if (!_copyListOpen) {
            _copyAlloc->Reset();
            _copyList->Reset(_copyAlloc.Get(), nullptr);
            _copyListOpen = true;
        }

        // Temporary upload buffer
        ComPtr<ID3D12Resource> upload;
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC rd{};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width = size;
        rd.Height = rd.DepthOrArraySize = rd.MipLevels = 1;
        rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        _device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&upload));

        void* mapped = nullptr;
        upload->Map(0, nullptr, &mapped);
        memcpy(mapped, data, size);
        upload->Unmap(0, nullptr);

        // COPY queue: resources implicitly promote from COMMON to COPY_DEST
        _copyList->CopyBufferRegion(dest, 0, upload.Get(), 0, size);
        _copyTemps.push_back(std::move(upload));

        ExecUploadAndWait();
    }

    void GPUDevice::UploadTexture(ID3D12Resource* dest, D3D12_RESOURCE_STATES /*finalState*/, uint32_t width, uint32_t height, uint32_t arraySize, const void* const* slices)
    {
        if (!slices) return;

        if (!_copyListOpen) {
            _copyAlloc->Reset();
            _copyList->Reset(_copyAlloc.Get(), nullptr);
            _copyListOpen = true;
        }

        D3D12_RESOURCE_DESC rd = dest->GetDesc();
        std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> layouts(arraySize);
        std::vector<UINT>   numRowsArr(arraySize);
        std::vector<UINT64> rowBytesArr(arraySize);
        UINT64 totalBytes = 0;
        _device->GetCopyableFootprints(&rd, 0, arraySize, 0, layouts.data(), numRowsArr.data(), rowBytesArr.data(), &totalBytes);

        ComPtr<ID3D12Resource> upload;
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC brd{};
        brd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        brd.Width = totalBytes;
        brd.Height = brd.DepthOrArraySize = brd.MipLevels = 1;
        brd.SampleDesc.Count = 1;
        brd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        _device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &brd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&upload));

        uint8_t* mapped = nullptr;
        upload->Map(0, nullptr, reinterpret_cast<void**>(&mapped));
        for (uint32_t i = 0; i < arraySize; ++i) {
            if (!slices[i]) continue;
            const uint8_t* src = static_cast<const uint8_t*>(slices[i]);
            uint8_t* dst = mapped + layouts[i].Offset;
            uint32_t srcRowPitch = width * GPUTexture2D::kBytesPerPixel;
            for (uint32_t row = 0; row < numRowsArr[i]; ++row) {
                memcpy(dst + row * layouts[i].Footprint.RowPitch, src + row * srcRowPitch, srcRowPitch);
            }
        }
        upload->Unmap(0, nullptr);

        for (uint32_t i = 0; i < arraySize; ++i) {
            D3D12_TEXTURE_COPY_LOCATION dstLoc{};
            dstLoc.pResource = dest;
            dstLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            dstLoc.SubresourceIndex = i;

            D3D12_TEXTURE_COPY_LOCATION srcLoc{};
            srcLoc.pResource = upload.Get();
            srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            srcLoc.PlacedFootprint = layouts[i];
            _copyList->CopyTextureRegion(&dstLoc, 0, 0, 0, &srcLoc, nullptr);
        }

        _copyTemps.push_back(std::move(upload));
        ExecUploadAndWait();
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
        return MakeRef<GPUComputeShader>(this, _rootSig.Get(), bytecode, size);
    }

    IGPUContextPtr GPUDevice::CreateContext()
    {
        return MakeRef<GPUContext>(this);
    }
#pragma endregion GPUDevice

    IGPUDevicePtr CreateGPUDevice()
    {
        auto ret = MakeRef<GPUDevice>();
        if (!ret->D()) {
            return nullptr;
        }
        return ret;
    }

} // namespace ist::d3d12
#endif // _WIN32

namespace ist {

    IGPUDevicePtr CreateGPUDevice_D3D12()
    {
#ifdef _WIN32
        return d3d12::CreateGPUDevice();
#else
        return nullptr;
#endif
    }

} // namespace ist


