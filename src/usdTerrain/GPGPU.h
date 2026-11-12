#pragma once
#include <cstdint>
#include <memory>
#include <functional>


// This is a simple abstraction layer to allow executing compute shaders

namespace ist {

    enum class GPUTextureFormat
    {
        R8Unorm,
        R8Snorm,
        R8Uint,
        R8Sint,
        R16Unorm,
        R16Snorm,
        R16Uint,
        R16Sint,
        R16Float,
        R32Uint,
        R32Sint,
        R32Float,

        RG8Unorm,
        RG8Snorm,
        RG8Uint,
        RG8Sint,
        RG16Unorm,
        RG16Snorm,
        RG16Uint,
        RG16Sint,
        RG16Float,
        RG32Uint,
        RG32Sint,
        RG32Float,

        RGBA8Unorm,
        RGBA8Snorm,
        RGBA8Uint,
        RGBA8Sint,
        RGBA16Unorm,
        RGBA16Snorm,
        RGBA16Uint,
        RGBA16Sint,
        RGBA16Float,
        RGBA32Uint,
        RGBA32Sint,
        RGBA32Float,

        BC1,
        BC2,
        BC3,
        BC4,
        BC5,
        BC6H,
        BC7,
    };



    class IGPUResource
    {
    public:
        virtual ~IGPUResource() = default;
        virtual void Release() = 0;
    };
    using IGPUResourcePtr = std::shared_ptr<IGPUResource>;


    // constant buffer or structured buffer
    class IGPUBuffer : public IGPUResource
    {
    public:
        virtual uint32_t GetElementSize() const = 0;
        virtual uint32_t GetElementCount() const = 0; // constant buffer returns 1
    };
    using IGPUBufferPtr = std::shared_ptr<IGPUBuffer>;


    // texture2d or texture2d array with no mipmaps
    class IGPUTexture2D : public IGPUResource
    {
    public:
        virtual uint32_t GetWidth() const = 0;
        virtual uint32_t GetHeight() const = 0;
        virtual uint32_t GetArraySize() const = 0; // non array texture returns 1
    };
    using IGPUTexture2DPtr = std::shared_ptr<IGPUTexture2D>;


    class IGPUComputeShader
    {
    public:
        virtual ~IGPUComputeShader() = default;
        virtual void Release() = 0;
    };
    using IGPUComputeShaderPtr = std::shared_ptr<IGPUComputeShader>;


    class IGPUContext
    {
    public:
        virtual ~IGPUContext() = default;
        virtual void Release() = 0;
        virtual void SetShader(IGPUComputeShaderPtr shader) = 0;
        virtual void SetConstants(uint32_t slot, IGPUBufferPtr buffer) = 0;
        virtual void SetSrv(uint32_t slot, IGPUResourcePtr resource) = 0;
        virtual void SetUav(uint32_t slot, IGPUResourcePtr resource) = 0;
        virtual void Dispatch(uint32_t x, uint32_t y, uint32_t z) = 0;

        // read back functions
        // these will be called on Wait() after GPU finished.
        using BufferReadCallback = std::function<void(const void*)>;
        using Texture2DReadCallback = std::function<void(const void*, int pitch)>;
        using Texture2DArrayReadCallback = std::function<void(const void*, int pitch, int index)>;
        virtual void ReadBuffer(IGPUBufferPtr buffer, const BufferReadCallback& callback) = 0;
        virtual void ReadTexture2D(IGPUTexture2DPtr texture, const Texture2DReadCallback& callback) = 0;
        virtual void ReadTexture2DArray(IGPUTexture2DPtr texture, const Texture2DArrayReadCallback& callback) = 0;

        virtual void Flush() = 0; // flush command list
        virtual void Finish() = 0; // wait for GPU to finish and ** call read back callbacks **

        virtual bool IsComplete() = 0; // check if GPU has finished
        virtual void Reset() = 0; // reset command list
    };
    using IGPUContextPtr = std::shared_ptr<IGPUContext>;


    class IGPUDevice
    {
    public:
        virtual ~IGPUDevice() = default;
        virtual void Release() = 0;
        virtual IGPUBufferPtr CreateConstantBuffer(size_t size, const void* data = nullptr) = 0;
        virtual IGPUBufferPtr CreateStructuredBuffer(size_t elementSize, size_t elementCount, const void* data = nullptr) = 0;
        virtual IGPUTexture2DPtr CreateTexture2D(uint32_t width, uint32_t height, GPUTextureFormat format, const void* data = nullptr) = 0;
        virtual IGPUTexture2DPtr CreateTexture2DArray(uint32_t width, uint32_t height, uint32_t arraySize, GPUTextureFormat format, const void* data[] = nullptr) = 0;
        virtual IGPUComputeShaderPtr CreateComputeShader(const void* bytecode, size_t bytecodeSize) = 0;
        virtual IGPUContextPtr CreateContext() = 0;
    };
    using IGPUDevicePtr = std::shared_ptr<IGPUDevice>;

    IGPUDevicePtr CreateGPUDevice_D3D11();
    IGPUDevicePtr CreateGPUDevice_D3D12();
    IGPUDevicePtr CreateGPUDevice_Vulkan();

} // namespace ist
