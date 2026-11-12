#include "pch.h"
#include "GPGPU_internal.h"

#include <vulkan/vulkan.h>
#include <cassert>
#include <vector>
#include <array>
#include <algorithm>
#include <cstring>
#include <mutex>
#include <condition_variable>

// ============================================================
// Descriptor set layout indices (must match SPIR-V shader layout)
//   set=0  binding=slot  -> uniform buffer  (SetConstants)
//   set=1  binding=slot  -> storage buffer  (SetSrv,  buffer)
//   set=2  binding=slot  -> storage image   (SetSrv,  texture)
//   set=3  binding=slot  -> storage buffer  (SetUav,  buffer) [readwrite]
//   set=4  binding=slot  -> storage image   (SetUav,  texture) [readwrite]
// ============================================================

namespace ist::vulkan {

static constexpr uint32_t kMaxSlots      = 16;
static constexpr uint32_t kSetCB         = 0;
static constexpr uint32_t kSetSrvBuf     = 1;
static constexpr uint32_t kSetSrvTex     = 2;
static constexpr uint32_t kSetUavBuf     = 3;
static constexpr uint32_t kSetUavTex     = 4;
static constexpr uint32_t kNumSets       = 5;

class GPUDevice;
class GPUBuffer;
class GPUTexture2D;
class GPUComputeShader;
class GPUContext;

class GPUBuffer : public IGPUBuffer
{
public:
    GPUBuffer(GPUDevice* dev, uint32_t elementSize, uint32_t elementCount, const void* data);
    ~GPUBuffer() override { Release(); }
    void Release() override;

    uint32_t GetElementSize()  const override { return _elementSize; }
    uint32_t GetElementCount() const override { return _elementCount; }

    VkBuffer     Buffer()       const { return _buffer; }
    VkDeviceSize SizeBytes()    const { return (VkDeviceSize)_elementSize * _elementCount; }

private:
    GPUDevice*     _dev          = nullptr;
    VkBuffer       _buffer       = VK_NULL_HANDLE;
    VkDeviceMemory _memory       = VK_NULL_HANDLE;
    uint32_t       _elementSize  = 0;
    uint32_t       _elementCount = 1;
};

class GPUTexture2D : public IGPUTexture2D
{
public:
    GPUTexture2D(GPUDevice* dev, uint32_t width, uint32_t height, uint32_t arraySize,
                 GPUTextureFormat format, const void* const* slices);
    ~GPUTexture2D() override { Release(); }
    void Release() override;

    uint32_t GetWidth()     const override { return _width; }
    uint32_t GetHeight()    const override { return _height; }
    uint32_t GetArraySize() const override { return _arraySize; }

    VkImage     Image()     const { return _image; }
    VkImageView ImageView() const { return _imageView; }
    VkFormat    Format()    const { return _vkFormat; }
    size_t      BPP()       const { return _bpp; }
    VkImageLayout& Layout()       { return _layout; }

private:
    GPUDevice*     _dev       = nullptr;
    VkImage        _image     = VK_NULL_HANDLE;
    VkDeviceMemory _memory    = VK_NULL_HANDLE;
    VkImageView    _imageView = VK_NULL_HANDLE;
    uint32_t       _width     = 0;
    uint32_t       _height    = 0;
    uint32_t       _arraySize = 1;
    VkFormat       _vkFormat  = VK_FORMAT_UNDEFINED;
    size_t         _bpp       = 0;
    VkImageLayout  _layout    = VK_IMAGE_LAYOUT_UNDEFINED;
};

class GPUComputeShader : public IGPUComputeShader
{
public:
    GPUComputeShader(GPUDevice* dev, const void* spirv, size_t size);
    ~GPUComputeShader() override { Release(); }
    void Release() override;

    VkPipeline       Pipeline()       const { return _pipeline; }
    VkPipelineLayout PipelineLayout() const { return _pipelineLayout; }

private:
    GPUDevice*       _dev            = nullptr;
    VkShaderModule   _shaderModule   = VK_NULL_HANDLE;
    VkPipelineLayout _pipelineLayout = VK_NULL_HANDLE;
    VkPipeline       _pipeline       = VK_NULL_HANDLE;
};

class GPUDevice : public IGPUDevice
{
public:
    GPUDevice();
    ~GPUDevice() override { Release(); }
    void Release() override;

    bool Valid() const { return _device != VK_NULL_HANDLE; }

    VkDevice         Device()     const { return _device; }
    VkPhysicalDevice PhysDev()    const { return _physDev; }
    VkQueue          Queue()      const { return _queue; }
    uint32_t         QueueFamily()const { return _queueFamily; }
    VkCommandPool    CmdPool()    const { return _cmdPool; }

    // Descriptor set layouts shared by all pipelines
    VkDescriptorSetLayout SetLayout(uint32_t setIndex) const { return _setLayouts[setIndex]; }

    // One-shot staging helpers (used during resource creation)
    void UploadBuffer(VkBuffer dst, const void* src, VkDeviceSize size);
    void UploadImageLayer(VkImage dst, uint32_t layer, uint32_t width, uint32_t height, const void* src, VkDeviceSize rowBytes);
    void TransitionImageLayout(VkImage img, uint32_t layers, VkImageLayout from, VkImageLayout to);

    // IGPUDevice
    IGPUBufferPtr         CreateConstantBuffer(size_t size, const void* data)               override;
    IGPUBufferPtr         CreateStructuredBuffer(size_t elementSize, size_t elementCount, const void* data) override;
    IGPUTexture2DPtr      CreateTexture2D(uint32_t w, uint32_t h, GPUTextureFormat fmt, const void* data)  override;
    IGPUTexture2DPtr      CreateTexture2DArray(uint32_t w, uint32_t h, uint32_t n, GPUTextureFormat fmt, const void* data[]) override;
    IGPUComputeShaderPtr  CreateComputeShader(const void* bytecode, size_t size)            override;
    IGPUContextPtr        CreateContext()                                                    override;

    VkBuffer       AllocBuffer(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags memProps, VkDeviceMemory& outMem);
    void           ExecOneShot(const std::function<void(VkCommandBuffer)>& fn);

private:
    VkInstance             _instance    = VK_NULL_HANDLE;
    VkPhysicalDevice       _physDev     = VK_NULL_HANDLE;
    VkDevice               _device      = VK_NULL_HANDLE;
    VkQueue                _queue       = VK_NULL_HANDLE;
    uint32_t               _queueFamily = ~0u;
    VkCommandPool          _cmdPool     = VK_NULL_HANDLE;
    VkDescriptorSetLayout  _setLayouts[kNumSets] = {};

    void CreateSetLayouts();
};

struct ReadbackRequest
{
    enum class Kind { Buffer, Texture2D, Texture2DArray };

    Kind kind;
    std::shared_ptr<IGPUResource> resource;
    VkBuffer        stagingBuf  = VK_NULL_HANDLE;
    VkDeviceMemory  stagingMem  = VK_NULL_HANDLE;
    VkDeviceSize    stagingSize = 0;
    uint32_t        width       = 0;
    uint32_t        height      = 0;
    uint32_t        arraySize   = 1;
    size_t          bpp         = 0;
    IGPUContext::BufferReadCallback          bufCb;
    IGPUContext::Texture2DReadCallback       tex2dCb;
    IGPUContext::Texture2DArrayReadCallback  tex2dArrayCb;
};

class GPUContext : public IGPUContext
{
public:
    explicit GPUContext(GPUDevice* dev);
    ~GPUContext() override { Release(); }
    void Release() override;

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
    GPUDevice*  _dev = nullptr;

    VkCommandBuffer _cmdBuf   = VK_NULL_HANDLE;
    VkFence         _fence    = VK_NULL_HANDLE;
    VkDescriptorPool _descPool = VK_NULL_HANDLE;

    IGPUComputeShaderPtr                         _shader;
    std::array<IGPUBufferPtr,   kMaxSlots>       _cbSlots{};
    std::array<IGPUResourcePtr, kMaxSlots>       _srvSlots{};
    std::array<IGPUResourcePtr, kMaxSlots>       _uavSlots{};

    std::vector<ReadbackRequest>  _readbacks;
    std::vector<VkDescriptorSet>  _pendingDescSets; // returned to pool after Finish

    bool _submitted = false;
    bool _finished  = false;

    void BindDescriptors();
    VkDescriptorSet AllocDescSet(VkDescriptorSetLayout layout);
    void RecordBufferBarrier(VkBuffer buf, VkAccessFlags src, VkAccessFlags dst,
                             VkPipelineStageFlags srcStage, VkPipelineStageFlags dstStage);
    void RecordImageBarrier(VkImage img, uint32_t layers,
                            VkImageLayout oldLayout, VkImageLayout newLayout,
                            VkAccessFlags src, VkAccessFlags dst,
                            VkPipelineStageFlags srcStage, VkPipelineStageFlags dstStage);
};


#pragma region Helpers
static uint32_t FindMemoryType(VkPhysicalDevice physDev, uint32_t typeBits, VkMemoryPropertyFlags props)
{
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(physDev, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i) {
        if ((typeBits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & props) == props)
            return i;
    }
    assert(false && "FindMemoryType failed");
    return ~0u;
}

static VkFormat GetVkFormat(GPUTextureFormat fmt, size_t& bytesPerPixel)
{
    switch (fmt) {
    case GPUTextureFormat::R8Unorm:    bytesPerPixel = 1;  return VK_FORMAT_R8_UNORM;
    case GPUTextureFormat::R8Snorm:    bytesPerPixel = 1;  return VK_FORMAT_R8_SNORM;
    case GPUTextureFormat::R8Uint:     bytesPerPixel = 1;  return VK_FORMAT_R8_UINT;
    case GPUTextureFormat::R8Sint:     bytesPerPixel = 1;  return VK_FORMAT_R8_SINT;
    case GPUTextureFormat::R16Unorm:   bytesPerPixel = 2;  return VK_FORMAT_R16_UNORM;
    case GPUTextureFormat::R16Snorm:   bytesPerPixel = 2;  return VK_FORMAT_R16_SNORM;
    case GPUTextureFormat::R16Uint:    bytesPerPixel = 2;  return VK_FORMAT_R16_UINT;
    case GPUTextureFormat::R16Sint:    bytesPerPixel = 2;  return VK_FORMAT_R16_SINT;
    case GPUTextureFormat::R16Float:   bytesPerPixel = 2;  return VK_FORMAT_R16_SFLOAT;
    case GPUTextureFormat::R32Uint:    bytesPerPixel = 4;  return VK_FORMAT_R32_UINT;
    case GPUTextureFormat::R32Sint:    bytesPerPixel = 4;  return VK_FORMAT_R32_SINT;
    case GPUTextureFormat::R32Float:   bytesPerPixel = 4;  return VK_FORMAT_R32_SFLOAT;
    case GPUTextureFormat::RG8Unorm:   bytesPerPixel = 2;  return VK_FORMAT_R8G8_UNORM;
    case GPUTextureFormat::RG8Snorm:   bytesPerPixel = 2;  return VK_FORMAT_R8G8_SNORM;
    case GPUTextureFormat::RG8Uint:    bytesPerPixel = 2;  return VK_FORMAT_R8G8_UINT;
    case GPUTextureFormat::RG8Sint:    bytesPerPixel = 2;  return VK_FORMAT_R8G8_SINT;
    case GPUTextureFormat::RG16Unorm:  bytesPerPixel = 4;  return VK_FORMAT_R16G16_UNORM;
    case GPUTextureFormat::RG16Snorm:  bytesPerPixel = 4;  return VK_FORMAT_R16G16_SNORM;
    case GPUTextureFormat::RG16Uint:   bytesPerPixel = 4;  return VK_FORMAT_R16G16_UINT;
    case GPUTextureFormat::RG16Sint:   bytesPerPixel = 4;  return VK_FORMAT_R16G16_SINT;
    case GPUTextureFormat::RG16Float:  bytesPerPixel = 4;  return VK_FORMAT_R16G16_SFLOAT;
    case GPUTextureFormat::RG32Uint:   bytesPerPixel = 8;  return VK_FORMAT_R32G32_UINT;
    case GPUTextureFormat::RG32Sint:   bytesPerPixel = 8;  return VK_FORMAT_R32G32_SINT;
    case GPUTextureFormat::RG32Float:  bytesPerPixel = 8;  return VK_FORMAT_R32G32_SFLOAT;
    case GPUTextureFormat::RGBA8Unorm:  bytesPerPixel = 4;  return VK_FORMAT_R8G8B8A8_UNORM;
    case GPUTextureFormat::RGBA8Snorm:  bytesPerPixel = 4;  return VK_FORMAT_R8G8B8A8_SNORM;
    case GPUTextureFormat::RGBA8Uint:   bytesPerPixel = 4;  return VK_FORMAT_R8G8B8A8_UINT;
    case GPUTextureFormat::RGBA8Sint:   bytesPerPixel = 4;  return VK_FORMAT_R8G8B8A8_SINT;
    case GPUTextureFormat::RGBA16Unorm: bytesPerPixel = 8;  return VK_FORMAT_R16G16B16A16_UNORM;
    case GPUTextureFormat::RGBA16Snorm: bytesPerPixel = 8;  return VK_FORMAT_R16G16B16A16_SNORM;
    case GPUTextureFormat::RGBA16Uint:  bytesPerPixel = 8;  return VK_FORMAT_R16G16B16A16_UINT;
    case GPUTextureFormat::RGBA16Sint:  bytesPerPixel = 8;  return VK_FORMAT_R16G16B16A16_SINT;
    case GPUTextureFormat::RGBA16Float: bytesPerPixel = 8;  return VK_FORMAT_R16G16B16A16_SFLOAT;
    case GPUTextureFormat::RGBA32Uint:  bytesPerPixel = 16; return VK_FORMAT_R32G32B32A32_UINT;
    case GPUTextureFormat::RGBA32Sint:  bytesPerPixel = 16; return VK_FORMAT_R32G32B32A32_SINT;
    case GPUTextureFormat::RGBA32Float: bytesPerPixel = 16; return VK_FORMAT_R32G32B32A32_SFLOAT;
    default: assert(false && "GetVkFormat: unsupported format"); bytesPerPixel = 0; return VK_FORMAT_UNDEFINED;
    }
}
#pragma endregion Helpers

#pragma region GPUBuffer
GPUBuffer::GPUBuffer(GPUDevice* dev, uint32_t elementSize, uint32_t elementCount, const void* data)
    : _dev(dev), _elementSize(elementSize), _elementCount(elementCount)
{
    VkDeviceSize size = (VkDeviceSize)elementSize * elementCount;

    _buffer = dev->AllocBuffer(size,
        VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT |
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT   |
        VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
        _memory);

    if (data)
        dev->UploadBuffer(_buffer, data, size);
}

void GPUBuffer::Release()
{
    if (_dev && _buffer) {
        vkDestroyBuffer(_dev->Device(), _buffer, nullptr);
        vkFreeMemory(_dev->Device(), _memory, nullptr);
        _buffer = VK_NULL_HANDLE;
        _memory = VK_NULL_HANDLE;
    }
}
#pragma endregion GPUBuffer

#pragma region GPUTexture2D
GPUTexture2D::GPUTexture2D(GPUDevice* dev, uint32_t width, uint32_t height, uint32_t arraySize,
                           GPUTextureFormat format, const void* const* slices)
    : _dev(dev), _width(width), _height(height), _arraySize(arraySize)
{
    _vkFormat = GetVkFormat(format, _bpp);

    VkImageCreateInfo imageCI{};
    imageCI.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageCI.imageType     = VK_IMAGE_TYPE_2D;
    imageCI.format        = _vkFormat;
    imageCI.extent        = { width, height, 1 };
    imageCI.mipLevels     = 1;
    imageCI.arrayLayers   = arraySize;
    imageCI.samples       = VK_SAMPLE_COUNT_1_BIT;
    imageCI.tiling        = VK_IMAGE_TILING_OPTIMAL;
    imageCI.usage         = VK_IMAGE_USAGE_STORAGE_BIT |
                            VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                            VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                            VK_IMAGE_USAGE_SAMPLED_BIT;
    imageCI.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
    imageCI.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    vkCreateImage(dev->Device(), &imageCI, nullptr, &_image);

    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(dev->Device(), _image, &req);
    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize  = req.size;
    allocInfo.memoryTypeIndex = FindMemoryType(dev->PhysDev(), req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    vkAllocateMemory(dev->Device(), &allocInfo, nullptr, &_memory);
    vkBindImageMemory(dev->Device(), _image, _memory, 0);

    // Upload slices
    if (slices) {
        for (uint32_t i = 0; i < arraySize; ++i) {
            if (slices[i]) {
                dev->UploadImageLayer(_image, i, width, height, slices[i], (VkDeviceSize)(_bpp * width));
            }
        }
        dev->TransitionImageLayout(_image, arraySize, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
        _layout = VK_IMAGE_LAYOUT_GENERAL;
    } else {
        dev->TransitionImageLayout(_image, arraySize, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
        _layout = VK_IMAGE_LAYOUT_GENERAL;
    }

    // Image view
    VkImageViewCreateInfo viewCI{};
    viewCI.sType      = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewCI.image      = _image;
    viewCI.viewType   = (arraySize > 1) ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
    viewCI.format     = _vkFormat;
    viewCI.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, arraySize };
    vkCreateImageView(dev->Device(), &viewCI, nullptr, &_imageView);
}

void GPUTexture2D::Release()
{
    if (_dev && _image) {
        vkDestroyImageView(_dev->Device(), _imageView, nullptr);
        vkDestroyImage(_dev->Device(), _image, nullptr);
        vkFreeMemory(_dev->Device(), _memory, nullptr);
        _imageView = VK_NULL_HANDLE;
        _image     = VK_NULL_HANDLE;
        _memory    = VK_NULL_HANDLE;
    }
}
#pragma endregion GPUTexture2D

#pragma region GPUComputeShader
GPUComputeShader::GPUComputeShader(GPUDevice* dev, const void* spirv, size_t size)
    : _dev(dev)
{
    // Shader module
    VkShaderModuleCreateInfo smCI{};
    smCI.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    smCI.codeSize = size;
    smCI.pCode    = reinterpret_cast<const uint32_t*>(spirv);
    vkCreateShaderModule(dev->Device(), &smCI, nullptr, &_shaderModule);

    // Pipeline layout using the shared descriptor set layouts
    VkDescriptorSetLayout layouts[kNumSets];
    for (uint32_t i = 0; i < kNumSets; ++i)
        layouts[i] = dev->SetLayout(i);

    VkPipelineLayoutCreateInfo plCI{};
    plCI.sType          = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plCI.setLayoutCount = kNumSets;
    plCI.pSetLayouts    = layouts;
    vkCreatePipelineLayout(dev->Device(), &plCI, nullptr, &_pipelineLayout);

    // Compute pipeline
    VkComputePipelineCreateInfo cpCI{};
    cpCI.sType  = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    cpCI.layout = _pipelineLayout;
    cpCI.stage.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    cpCI.stage.stage  = VK_SHADER_STAGE_COMPUTE_BIT;
    cpCI.stage.module = _shaderModule;
    cpCI.stage.pName  = "main";
    vkCreateComputePipelines(dev->Device(), VK_NULL_HANDLE, 1, &cpCI, nullptr, &_pipeline);
}

void GPUComputeShader::Release()
{
    if (_dev && _pipeline) {
        vkDestroyPipeline(_dev->Device(), _pipeline, nullptr);
        vkDestroyPipelineLayout(_dev->Device(), _pipelineLayout, nullptr);
        vkDestroyShaderModule(_dev->Device(), _shaderModule, nullptr);
        _pipeline       = VK_NULL_HANDLE;
        _pipelineLayout = VK_NULL_HANDLE;
        _shaderModule   = VK_NULL_HANDLE;
    }
}
#pragma endregion GPUComputeShader

#pragma region GPUContext
GPUContext::GPUContext(GPUDevice* dev) : _dev(dev)
{
    VkCommandBufferAllocateInfo allocInfo{};
    allocInfo.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.commandPool        = dev->CmdPool();
    allocInfo.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = 1;
    vkAllocateCommandBuffers(dev->Device(), &allocInfo, &_cmdBuf);

    VkFenceCreateInfo fCI{};
    fCI.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    vkCreateFence(dev->Device(), &fCI, nullptr, &_fence);

    // Descriptor pool: enough descriptors for one full context
    static const VkDescriptorPoolSize poolSizes[] = {
        { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, kMaxSlots },
        { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, kMaxSlots * 2 },
        { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,  kMaxSlots * 2 },
    };
    VkDescriptorPoolCreateInfo dpCI{};
    dpCI.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dpCI.maxSets       = kNumSets * 16; // generous room for multiple dispatches
    dpCI.poolSizeCount = 3;
    dpCI.pPoolSizes    = poolSizes;
    vkCreateDescriptorPool(dev->Device(), &dpCI, nullptr, &_descPool);

    Reset();
}

void GPUContext::Release()
{
    if (_dev && _cmdBuf) {
        Finish();
        vkFreeCommandBuffers(_dev->Device(), _dev->CmdPool(), 1, &_cmdBuf);
        vkDestroyFence(_dev->Device(), _fence, nullptr);
        vkDestroyDescriptorPool(_dev->Device(), _descPool, nullptr);
        _cmdBuf   = VK_NULL_HANDLE;
        _fence    = VK_NULL_HANDLE;
        _descPool = VK_NULL_HANDLE;
    }
}

void GPUContext::Reset()
{
    if (_submitted) Finish();
    vkResetCommandBuffer(_cmdBuf, 0);
    vkResetDescriptorPool(_dev->Device(), _descPool, 0);
    _pendingDescSets.clear();

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(_cmdBuf, &beginInfo);

    _shader = nullptr;
    _cbSlots  = {};
    _srvSlots = {};
    _uavSlots = {};
    _readbacks.clear();
    _submitted = false;
    _finished  = false;
}

void GPUContext::SetShader(IGPUComputeShaderPtr shader)
{
    _shader = std::move(shader);
}

void GPUContext::SetConstants(uint32_t slot, IGPUBufferPtr buffer)
{
    assert(slot < kMaxSlots);
    _cbSlots[slot] = std::move(buffer);
}

void GPUContext::SetSrv(uint32_t slot, IGPUResourcePtr resource)
{
    assert(slot < kMaxSlots);
    _srvSlots[slot] = std::move(resource);
}

void GPUContext::SetUav(uint32_t slot, IGPUResourcePtr resource)
{
    assert(slot < kMaxSlots);
    _uavSlots[slot] = std::move(resource);
}

VkDescriptorSet GPUContext::AllocDescSet(VkDescriptorSetLayout layout)
{
    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool     = _descPool;
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts        = &layout;
    VkDescriptorSet ds = VK_NULL_HANDLE;
    vkAllocateDescriptorSets(_dev->Device(), &allocInfo, &ds);
    return ds;
}

void GPUContext::RecordBufferBarrier(VkBuffer buf, VkAccessFlags src, VkAccessFlags dst,
                                     VkPipelineStageFlags srcStage, VkPipelineStageFlags dstStage)
{
    VkBufferMemoryBarrier b{};
    b.sType               = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    b.srcAccessMask       = src;
    b.dstAccessMask       = dst;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.buffer              = buf;
    b.offset              = 0;
    b.size                = VK_WHOLE_SIZE;
    vkCmdPipelineBarrier(_cmdBuf, srcStage, dstStage, 0, 0, nullptr, 1, &b, 0, nullptr);
}

void GPUContext::RecordImageBarrier(VkImage img, uint32_t layers,
                                    VkImageLayout oldLayout, VkImageLayout newLayout,
                                    VkAccessFlags src, VkAccessFlags dst,
                                    VkPipelineStageFlags srcStage, VkPipelineStageFlags dstStage)
{
    VkImageMemoryBarrier b{};
    b.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.oldLayout           = oldLayout;
    b.newLayout           = newLayout;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image               = img;
    b.subresourceRange    = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layers };
    b.srcAccessMask       = src;
    b.dstAccessMask       = dst;
    vkCmdPipelineBarrier(_cmdBuf, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &b);
}

void GPUContext::BindDescriptors()
{
    auto* shader = static_cast<GPUComputeShader*>(_shader.get());
    vkCmdBindPipeline(_cmdBuf, VK_PIPELINE_BIND_POINT_COMPUTE, shader->Pipeline());

    // set 0: constant buffers (uniform buffers)
    {
        VkDescriptorSet ds = AllocDescSet(_dev->SetLayout(kSetCB));
        for (uint32_t slot = 0; slot < kMaxSlots; ++slot) {
            if (!_cbSlots[slot]) continue;
            auto* buf = static_cast<GPUBuffer*>(_cbSlots[slot].get());
            VkDescriptorBufferInfo bi{ buf->Buffer(), 0, VK_WHOLE_SIZE };
            VkWriteDescriptorSet wr{};
            wr.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            wr.dstSet          = ds;
            wr.dstBinding      = slot;
            wr.descriptorCount = 1;
            wr.descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            wr.pBufferInfo     = &bi;
            vkUpdateDescriptorSets(_dev->Device(), 1, &wr, 0, nullptr);
        }
        vkCmdBindDescriptorSets(_cmdBuf, VK_PIPELINE_BIND_POINT_COMPUTE,
            shader->PipelineLayout(), kSetCB, 1, &ds, 0, nullptr);
    }

    // set 1: SRV buffers, set 2: SRV textures
    {
        VkDescriptorSet dsBuf = AllocDescSet(_dev->SetLayout(kSetSrvBuf));
        VkDescriptorSet dsTex = AllocDescSet(_dev->SetLayout(kSetSrvTex));
        for (uint32_t slot = 0; slot < kMaxSlots; ++slot) {
            if (!_srvSlots[slot]) continue;
            if (auto* buf = dynamic_cast<GPUBuffer*>(_srvSlots[slot].get())) {
                // SRV buffer: transition to shader read (already in GENERAL for storage)
                RecordBufferBarrier(buf->Buffer(),
                    VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);

                VkDescriptorBufferInfo bi{ buf->Buffer(), 0, VK_WHOLE_SIZE };
                VkWriteDescriptorSet wr{};
                wr.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                wr.dstSet          = dsBuf;
                wr.dstBinding      = slot;
                wr.descriptorCount = 1;
                wr.descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                wr.pBufferInfo     = &bi;
                vkUpdateDescriptorSets(_dev->Device(), 1, &wr, 0, nullptr);
            } else if (auto* tex = dynamic_cast<GPUTexture2D*>(_srvSlots[slot].get())) {
                VkDescriptorImageInfo ii{ VK_NULL_HANDLE, tex->ImageView(), VK_IMAGE_LAYOUT_GENERAL };
                VkWriteDescriptorSet wr{};
                wr.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                wr.dstSet          = dsTex;
                wr.dstBinding      = slot;
                wr.descriptorCount = 1;
                wr.descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
                wr.pImageInfo      = &ii;
                vkUpdateDescriptorSets(_dev->Device(), 1, &wr, 0, nullptr);
            }
        }
        VkDescriptorSet srvSets[2] = { dsBuf, dsTex };
        vkCmdBindDescriptorSets(_cmdBuf, VK_PIPELINE_BIND_POINT_COMPUTE,
            shader->PipelineLayout(), kSetSrvBuf, 2, srvSets, 0, nullptr);
    }

    // set 3: UAV buffers, set 4: UAV textures
    {
        VkDescriptorSet dsBuf = AllocDescSet(_dev->SetLayout(kSetUavBuf));
        VkDescriptorSet dsTex = AllocDescSet(_dev->SetLayout(kSetUavTex));
        for (uint32_t slot = 0; slot < kMaxSlots; ++slot) {
            if (!_uavSlots[slot]) continue;
            if (auto* buf = dynamic_cast<GPUBuffer*>(_uavSlots[slot].get())) {
                VkDescriptorBufferInfo bi{ buf->Buffer(), 0, VK_WHOLE_SIZE };
                VkWriteDescriptorSet wr{};
                wr.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                wr.dstSet          = dsBuf;
                wr.dstBinding      = slot;
                wr.descriptorCount = 1;
                wr.descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                wr.pBufferInfo     = &bi;
                vkUpdateDescriptorSets(_dev->Device(), 1, &wr, 0, nullptr);
            } else if (auto* tex = dynamic_cast<GPUTexture2D*>(_uavSlots[slot].get())) {
                VkDescriptorImageInfo ii{ VK_NULL_HANDLE, tex->ImageView(), VK_IMAGE_LAYOUT_GENERAL };
                VkWriteDescriptorSet wr{};
                wr.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                wr.dstSet          = dsTex;
                wr.dstBinding      = slot;
                wr.descriptorCount = 1;
                wr.descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
                wr.pImageInfo      = &ii;
                vkUpdateDescriptorSets(_dev->Device(), 1, &wr, 0, nullptr);
            }
        }
        VkDescriptorSet uavSets[2] = { dsBuf, dsTex };
        vkCmdBindDescriptorSets(_cmdBuf, VK_PIPELINE_BIND_POINT_COMPUTE,
            shader->PipelineLayout(), kSetUavBuf, 2, uavSets, 0, nullptr);
    }
}

void GPUContext::Dispatch(uint32_t x, uint32_t y, uint32_t z)
{
    if (!_shader) return;

    BindDescriptors();
    vkCmdDispatch(_cmdBuf, x, y, z);

    // Post-dispatch: memory barrier so UAV writes are visible before next dispatch / readback
    VkMemoryBarrier memBarrier{};
    memBarrier.sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    memBarrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    memBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT |
                               VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(_cmdBuf,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
        0, 1, &memBarrier, 0, nullptr, 0, nullptr);
}

void GPUContext::ReadBuffer(IGPUBufferPtr buffer, const BufferReadCallback& cb)
{
    auto* buf = static_cast<GPUBuffer*>(buffer.get());
    VkDeviceSize size = buf->SizeBytes();

    ReadbackRequest req;
    req.kind        = ReadbackRequest::Kind::Buffer;
    req.resource    = buffer;
    req.stagingSize = size;
    req.bufCb       = cb;
    req.stagingBuf  = _dev->AllocBuffer(size,
        VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        req.stagingMem);

    RecordBufferBarrier(buf->Buffer(),
        VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);

    VkBufferCopy region{ 0, 0, size };
    vkCmdCopyBuffer(_cmdBuf, buf->Buffer(), req.stagingBuf, 1, &region);

    _readbacks.push_back(std::move(req));
}

void GPUContext::ReadTexture2D(IGPUTexture2DPtr texture, const Texture2DReadCallback& cb)
{
    auto* tex = static_cast<GPUTexture2D*>(texture.get());
    VkDeviceSize rowBytes = (VkDeviceSize)(tex->BPP() * tex->GetWidth());
    VkDeviceSize size     = rowBytes * tex->GetHeight();

    ReadbackRequest req;
    req.kind        = ReadbackRequest::Kind::Texture2D;
    req.resource    = texture;
    req.stagingSize = size;
    req.width       = tex->GetWidth();
    req.height      = tex->GetHeight();
    req.bpp         = tex->BPP();
    req.tex2dCb     = cb;
    req.stagingBuf  = _dev->AllocBuffer(size,
        VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        req.stagingMem);

    VkBufferImageCopy region{};
    region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    region.imageExtent      = { tex->GetWidth(), tex->GetHeight(), 1 };
    vkCmdCopyImageToBuffer(_cmdBuf, tex->Image(), VK_IMAGE_LAYOUT_GENERAL, req.stagingBuf, 1, &region);

    _readbacks.push_back(std::move(req));
}

void GPUContext::ReadTexture2DArray(IGPUTexture2DPtr texture, const Texture2DArrayReadCallback& cb)
{
    auto* tex = static_cast<GPUTexture2D*>(texture.get());
    VkDeviceSize rowBytes  = (VkDeviceSize)(tex->BPP() * tex->GetWidth());
    VkDeviceSize layerSize = rowBytes * tex->GetHeight();
    uint32_t     arrSize   = tex->GetArraySize();
    VkDeviceSize totalSize = layerSize * arrSize;

    ReadbackRequest req;
    req.kind           = ReadbackRequest::Kind::Texture2DArray;
    req.resource       = texture;
    req.stagingSize    = totalSize;
    req.width          = tex->GetWidth();
    req.height         = tex->GetHeight();
    req.arraySize      = arrSize;
    req.bpp            = tex->BPP();
    req.tex2dArrayCb   = cb;
    req.stagingBuf     = _dev->AllocBuffer(totalSize,
        VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        req.stagingMem);

    for (uint32_t i = 0; i < arrSize; ++i) {
        VkBufferImageCopy region{};
        region.bufferOffset     = layerSize * i;
        region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, i, 1 };
        region.imageExtent      = { tex->GetWidth(), tex->GetHeight(), 1 };
        vkCmdCopyImageToBuffer(_cmdBuf, tex->Image(), VK_IMAGE_LAYOUT_GENERAL, req.stagingBuf, 1, &region);
    }

    _readbacks.push_back(std::move(req));
}

void GPUContext::Flush()
{
    if (_submitted) return;
    vkEndCommandBuffer(_cmdBuf);

    VkSubmitInfo si{};
    si.sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers    = &_cmdBuf;
    vkQueueSubmit(_dev->Queue(), 1, &si, _fence);
    _submitted = true;
}

void GPUContext::Finish()
{
    if (!_submitted) Flush();
    if (_finished)   return;

    vkWaitForFences(_dev->Device(), 1, &_fence, VK_TRUE, UINT64_MAX);
    vkResetFences(_dev->Device(), 1, &_fence);
    _finished = true;

    // Fire readback callbacks
    for (auto& req : _readbacks) {
        void* mapped = nullptr;
        vkMapMemory(_dev->Device(), req.stagingMem, 0, req.stagingSize, 0, &mapped);

        if (req.kind == ReadbackRequest::Kind::Buffer) {
            req.bufCb(mapped);
        } else if (req.kind == ReadbackRequest::Kind::Texture2D) {
            int pitch = (int)(req.bpp * req.width);
            req.tex2dCb(mapped, pitch);
        } else {
            int pitch     = (int)(req.bpp * req.width);
            int layerSize = pitch * (int)req.height;
            for (uint32_t i = 0; i < req.arraySize; ++i) {
                req.tex2dArrayCb(static_cast<uint8_t*>(mapped) + (size_t)layerSize * i, pitch, (int)i);
            }
        }

        vkUnmapMemory(_dev->Device(), req.stagingMem);
        vkDestroyBuffer(_dev->Device(), req.stagingBuf, nullptr);
        vkFreeMemory(_dev->Device(), req.stagingMem, nullptr);
    }
    _readbacks.clear();
}

bool GPUContext::IsComplete()
{
    if (_finished)   return true;
    if (!_submitted) return false;
    return vkGetFenceStatus(_dev->Device(), _fence) == VK_SUCCESS;
}
#pragma endregion GPUContext

#pragma region GPUDevice
GPUDevice::GPUDevice()
{
    // --- Instance ---
    VkApplicationInfo appInfo{};
    appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.apiVersion = VK_API_VERSION_1_2;

    VkInstanceCreateInfo instCI{};
    instCI.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    instCI.pApplicationInfo = &appInfo;

    if (vkCreateInstance(&instCI, nullptr, &_instance) != VK_SUCCESS)
        return;

    // --- Physical device: pick first device with a compute queue ---
    uint32_t devCount = 0;
    vkEnumeratePhysicalDevices(_instance, &devCount, nullptr);
    if (devCount == 0) return;
    std::vector<VkPhysicalDevice> physDevs(devCount);
    vkEnumeratePhysicalDevices(_instance, &devCount, physDevs.data());

    for (auto pd : physDevs) {
        uint32_t qCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(pd, &qCount, nullptr);
        std::vector<VkQueueFamilyProperties> qProps(qCount);
        vkGetPhysicalDeviceQueueFamilyProperties(pd, &qCount, qProps.data());
        for (uint32_t i = 0; i < qCount; ++i) {
            if (qProps[i].queueFlags & VK_QUEUE_COMPUTE_BIT) {
                _physDev = pd;
                _queueFamily = i;
                break;
            }
        }
        if (_physDev != VK_NULL_HANDLE) break;
    }
    if (_physDev == VK_NULL_HANDLE) return;

    // --- Logical device ---
    float priority = 1.0f;
    VkDeviceQueueCreateInfo qCI{};
    qCI.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qCI.queueFamilyIndex = _queueFamily;
    qCI.queueCount = 1;
    qCI.pQueuePriorities = &priority;

    VkDeviceCreateInfo devCI{};
    devCI.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    devCI.queueCreateInfoCount = 1;
    devCI.pQueueCreateInfos = &qCI;

    if (vkCreateDevice(_physDev, &devCI, nullptr, &_device) != VK_SUCCESS)
        return;

    vkGetDeviceQueue(_device, _queueFamily, 0, &_queue);

    // --- Command pool ---
    VkCommandPoolCreateInfo poolCI{};
    poolCI.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolCI.queueFamilyIndex = _queueFamily;
    poolCI.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    vkCreateCommandPool(_device, &poolCI, nullptr, &_cmdPool);

    CreateSetLayouts();
}

void GPUDevice::CreateSetLayouts()
{
    // One layout per descriptor set; each allows up to kMaxSlots bindings.
    static const VkDescriptorType kTypes[kNumSets] = {
        VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,          // set 0: CBV
        VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,          // set 1: SRV buffer
        VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,           // set 2: SRV texture
        VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,          // set 3: UAV buffer
        VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,           // set 4: UAV texture
    };

    for (uint32_t s = 0; s < kNumSets; ++s) {
        std::vector<VkDescriptorSetLayoutBinding> bindings(kMaxSlots);
        for (uint32_t b = 0; b < kMaxSlots; ++b) {
            bindings[b].binding = b;
            bindings[b].descriptorType = kTypes[s];
            bindings[b].descriptorCount = 1;
            bindings[b].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
            bindings[b].pImmutableSamplers = nullptr;
        }
        VkDescriptorSetLayoutCreateInfo lCI{};
        lCI.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        lCI.bindingCount = kMaxSlots;
        lCI.pBindings = bindings.data();
        vkCreateDescriptorSetLayout(_device, &lCI, nullptr, &_setLayouts[s]);
    }
}

VkBuffer GPUDevice::AllocBuffer(VkDeviceSize size, VkBufferUsageFlags usage,
    VkMemoryPropertyFlags memProps, VkDeviceMemory& outMem)
{
    VkBufferCreateInfo bCI{};
    bCI.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bCI.size = size;
    bCI.usage = usage;
    bCI.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VkBuffer buf = VK_NULL_HANDLE;
    vkCreateBuffer(_device, &bCI, nullptr, &buf);

    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(_device, buf, &req);

    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = req.size;
    allocInfo.memoryTypeIndex = FindMemoryType(_physDev, req.memoryTypeBits, memProps);
    vkAllocateMemory(_device, &allocInfo, nullptr, &outMem);
    vkBindBufferMemory(_device, buf, outMem, 0);
    return buf;
}

void GPUDevice::ExecOneShot(const std::function<void(VkCommandBuffer)>& fn)
{
    VkCommandBufferAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.commandPool = _cmdPool;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = 1;

    VkCommandBuffer cb = VK_NULL_HANDLE;
    vkAllocateCommandBuffers(_device, &allocInfo, &cb);

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cb, &beginInfo);

    fn(cb);

    vkEndCommandBuffer(cb);

    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cb;

    VkFence fence = VK_NULL_HANDLE;
    VkFenceCreateInfo fCI{};
    fCI.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    vkCreateFence(_device, &fCI, nullptr, &fence);

    vkQueueSubmit(_queue, 1, &si, fence);
    vkWaitForFences(_device, 1, &fence, VK_TRUE, UINT64_MAX);

    vkDestroyFence(_device, fence, nullptr);
    vkFreeCommandBuffers(_device, _cmdPool, 1, &cb);
}

void GPUDevice::UploadBuffer(VkBuffer dst, const void* src, VkDeviceSize size)
{
    VkDeviceMemory stagingMem = VK_NULL_HANDLE;
    VkBuffer staging = AllocBuffer(size,
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        stagingMem);

    void* mapped = nullptr;
    vkMapMemory(_device, stagingMem, 0, size, 0, &mapped);
    std::memcpy(mapped, src, size);
    vkUnmapMemory(_device, stagingMem);

    ExecOneShot([&](VkCommandBuffer cb) {
        VkBufferCopy region{ 0, 0, size };
        vkCmdCopyBuffer(cb, staging, dst, 1, &region);
        });

    vkDestroyBuffer(_device, staging, nullptr);
    vkFreeMemory(_device, stagingMem, nullptr);
}

void GPUDevice::TransitionImageLayout(VkImage img, uint32_t layers,
    VkImageLayout from, VkImageLayout to)
{
    ExecOneShot([&](VkCommandBuffer cb) {
        VkImageMemoryBarrier barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.oldLayout = from;
        barrier.newLayout = to;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = img;
        barrier.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layers };
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        vkCmdPipelineBarrier(cb,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            0, 0, nullptr, 0, nullptr, 1, &barrier);
        });
}

void GPUDevice::UploadImageLayer(VkImage dst, uint32_t layer,
    uint32_t width, uint32_t height,
    const void* src, VkDeviceSize rowBytes)
{
    VkDeviceSize size = rowBytes * height;
    VkDeviceMemory stagingMem = VK_NULL_HANDLE;
    VkBuffer staging = AllocBuffer(size,
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        stagingMem);

    void* mapped = nullptr;
    vkMapMemory(_device, stagingMem, 0, size, 0, &mapped);
    std::memcpy(mapped, src, (size_t)size);
    vkUnmapMemory(_device, stagingMem);

    ExecOneShot([&](VkCommandBuffer cb) {
        // Transition layer to TRANSFER_DST
        VkImageMemoryBarrier toTransfer{};
        toTransfer.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        toTransfer.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        toTransfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        toTransfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toTransfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toTransfer.image = dst;
        toTransfer.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, layer, 1 };
        toTransfer.srcAccessMask = 0;
        toTransfer.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(cb,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
            0, 0, nullptr, 0, nullptr, 1, &toTransfer);

        VkBufferImageCopy region{};
        region.bufferOffset = 0;
        region.bufferRowLength = 0;
        region.bufferImageHeight = 0;
        region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, layer, 1 };
        region.imageExtent = { width, height, 1 };
        vkCmdCopyBufferToImage(cb, staging, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        });

    vkDestroyBuffer(_device, staging, nullptr);
    vkFreeMemory(_device, stagingMem, nullptr);
}

void GPUDevice::Release()
{
    if (_device) {
        vkDeviceWaitIdle(_device);
        for (auto& sl : _setLayouts) {
            if (sl) vkDestroyDescriptorSetLayout(_device, sl, nullptr);
            sl = VK_NULL_HANDLE;
        }
        if (_cmdPool) vkDestroyCommandPool(_device, _cmdPool, nullptr);
        vkDestroyDevice(_device, nullptr);
        _cmdPool = VK_NULL_HANDLE;
        _device = VK_NULL_HANDLE;
    }
    if (_instance) {
        vkDestroyInstance(_instance, nullptr);
        _instance = VK_NULL_HANDLE;
    }
}

IGPUBufferPtr GPUDevice::CreateConstantBuffer(size_t size, const void* data)
{
    // Constant buffer: single element of `size` bytes
    return MakeRef<GPUBuffer>(this, static_cast<uint32_t>(size), 1u, data);
}

IGPUBufferPtr GPUDevice::CreateStructuredBuffer(size_t elementSize, size_t elementCount, const void* data)
{
    return MakeRef<GPUBuffer>(this, static_cast<uint32_t>(elementSize), static_cast<uint32_t>(elementCount), data);
}

IGPUTexture2DPtr GPUDevice::CreateTexture2D(uint32_t w, uint32_t h, GPUTextureFormat fmt, const void* data)
{
    const void* slices[1] = { data };
    return MakeRef<GPUTexture2D>(this, w, h, 1u, fmt, data ? slices : nullptr);
}

IGPUTexture2DPtr GPUDevice::CreateTexture2DArray(uint32_t w, uint32_t h, uint32_t n, GPUTextureFormat fmt, const void* data[])
{
    return MakeRef<GPUTexture2D>(this, w, h, n, fmt, data);
}

IGPUComputeShaderPtr GPUDevice::CreateComputeShader(const void* bytecode, size_t size)
{
    return MakeRef<GPUComputeShader>(this, bytecode, size);
}

IGPUContextPtr GPUDevice::CreateContext()
{
    return MakeRef<GPUContext>(this);
}
#pragma endregion GPUDevice


IGPUDevicePtr CreateGPUDevice()
{
    auto ret = MakeRef<GPUDevice>();
    if (!ret->Valid())
        return nullptr;
    return ret;
}

} // namespace ist::vulkan

namespace ist {

    IGPUDevicePtr CreateGPUDevice_Vulkan()
    {
        return vulkan::CreateGPUDevice();
    }

} // namespace ist
