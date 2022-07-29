// Copyright (C) 2009-2022, Panagiotis Christopoulos Charitos and contributors.
// All rights reserved.
// Code licensed under the BSD License.
// http://www.anki3d.org/LICENSE

#include <ThirdParty/FidelityFX/fsr2/ffx-fsr2-api/ffx_fsr2.h>
#include <Anki/Gr/Vulkan/fsr2_vk/vk/AnkiFsr2.h>

#if ANKI_ENABLE_CUSTOM_FSR2_BACKEND

#include "shaders/ffx_fsr2_shaders_vk.h"  // include all the precompiled VK shaders for the FSR2 passes
#include <ThirdParty/FidelityFX/fsr2/ffx-fsr2-api/ffx_fsr2_private.h>
#include <string.h>
#include <math.h>
#include <stdlib.h>

#include <AnKi/Renderer/Renderer.h>
#include <AnKi/Gr/Vulkan/Common.h>
#include <AnKi/Gr/Vulkan/GrUpscalerImpl.h>
#include <AnKi/Gr/Vulkan/GrManagerImpl.h>
#include <AnKi/Gr/CommandBuffer.h>

namespace anki {

// prototypes for functions in the interface
FfxErrorCode getDeviceCapabilitiesVK(FfxFsr2Interface* backendInterface, FfxDeviceCapabilities* deviceCapabilities, FfxDevice device);
FfxErrorCode createDeviceVK(FfxFsr2Interface* backendInterface, FfxDevice device);
FfxErrorCode destroyDeviceVK(FfxFsr2Interface* backendInterface, FfxDevice device);
FfxErrorCode createResourceVK(FfxFsr2Interface* backendInterface, const FfxCreateResourceDescription* desc, FfxResourceInternal* outResource);
FfxErrorCode registerResourceVK(FfxFsr2Interface* backendInterface, const FfxResource* inResource, FfxResourceInternal* outResourceInternal);
FfxErrorCode unregisterResourcesVK(FfxFsr2Interface* backendInterface);
FfxResourceDescription getResourceDescriptorVK(FfxFsr2Interface* backendInterface, FfxResourceInternal resource);
FfxErrorCode destroyResourceVK(FfxFsr2Interface* backendInterface, FfxResourceInternal resource);
FfxErrorCode createPipelineVK(FfxFsr2Interface* backendInterface, FfxFsr2Pass passId, const FfxPipelineDescription* desc, FfxPipelineState* outPass);
FfxErrorCode destroyPipelineVK(FfxFsr2Interface* backendInterface, FfxPipelineState* pipeline);
FfxErrorCode scheduleRenderJobVK(FfxFsr2Interface* backendInterface, const FfxRenderJobDescription* job);
FfxErrorCode executeRenderJobsVK(FfxFsr2Interface* backendInterface, FfxCommandList commandList);

#define ANKI_FSR2_MAX_QUEUED_FRAMES              ( 4)
#define ANKI_FSR2_MAX_RESOURCE_COUNT             (64)
#define ANKI_FSR2_MAX_STAGING_RESOURCE_COUNT     ( 8)
#define ANKI_FSR2_MAX_BARRIERS                   (16)
#define ANKI_FSR2_MAX_RENDERJOBS                 (32)
#define ANKI_FSR2_MAX_IMAGE_COPY_MIPS            (32)
#define ANKI_FSR2_MAX_SAMPLERS                   ( 2)
#define ANKI_FSR2_MAX_UNIFORM_BUFFERS            ( 4)
#define ANKI_FSR2_MAX_IMAGE_VIEWS                (32)
#define ANKI_FSR2_MAX_BUFFERED_DESCRIPTORS       (FFX_ANKI_FSR2_PASS_COUNT * ANKI_FSR2_MAX_QUEUED_FRAMES)
#define ANKI_FSR2_UBO_RING_BUFFER_SIZE           (ANKI_FSR2_MAX_BUFFERED_DESCRIPTORS * ANKI_FSR2_MAX_UNIFORM_BUFFERS)
#define ANKI_FSR2_UBO_MEMORY_BLOCK_SIZE          (ANKI_FSR2_UBO_RING_BUFFER_SIZE * 256)

typedef struct AnkiFsr2BackendCtx {

    // store for resources and resourceViews
    typedef struct Resource
    {
#ifdef _DEBUG
        char                    resourceName[64] = {};
#endif
		TexturePtr m_imageResource;
		BufferPtr m_bufferResource;
        TextureViewPtr m_allMipsImageView;
		TextureViewPtr m_singleMipImageViews[ANKI_FSR2_MAX_IMAGE_VIEWS];

        FfxResourceDescription  resourceDescription;
        FfxResourceStates       state;
        bool                    undefined;
    } Resource;
                            
    uint32_t                renderJobCount = 0;
    FfxRenderJobDescription renderJobs[ANKI_FSR2_MAX_RENDERJOBS] = {};

    uint32_t                nextStaticResource = 0;
    uint32_t                nextDynamicResource = 0;
    uint32_t                stagingResourceCount = 0;
    Resource                resources[ANKI_FSR2_MAX_RESOURCE_COUNT] = {};
    FfxResourceInternal     stagingResources[ANKI_FSR2_MAX_STAGING_RESOURCE_COUNT] = {};

    uint32_t                numDeviceExtensions = 0;
    VkExtensionProperties*  extensionProperties = nullptr;
	Renderer*				m_r = nullptr;

	// Shaders
	ShaderProgramResourcePtr m_progs[FfxFsr2Pass::FFX_FSR2_PASS_COUNT];
	ShaderProgramPtr m_grProgs[FfxFsr2Pass::FFX_FSR2_PASS_COUNT];
} AnkiFsr2BackendCtx;

FFX_API size_t ffxFsr2GetScratchMemorySizeVK(Renderer& renderer)
{
    uint32_t numExtensions = 0;

	const GrManagerImpl& managerImpl = static_cast<const GrManagerImpl&>(renderer.getGrManager());
	VkPhysicalDevice physicalDevice = managerImpl.getPhysicalDevice();
	if(physicalDevice)
	{
		vkEnumerateDeviceExtensionProperties(physicalDevice, nullptr, &numExtensions, nullptr);
	}

    return FFX_ALIGN_UP(sizeof(AnkiFsr2BackendCtx) + sizeof(VkExtensionProperties) * numExtensions, sizeof(uint64_t));
}

FfxErrorCode ffxFsr2GetInterfaceVK(
    FfxFsr2Interface* outInterface,
    void* scratchBuffer,
    size_t scratchBufferSize,
    Renderer* renderer)
{
    FFX_RETURN_ON_ERROR(
        outInterface,
        FFX_ERROR_INVALID_POINTER);
    FFX_RETURN_ON_ERROR(
        scratchBuffer,
        FFX_ERROR_INVALID_POINTER);
	FFX_RETURN_ON_ERROR(scratchBufferSize >= ffxFsr2GetScratchMemorySizeVK(*renderer),
        FFX_ERROR_INSUFFICIENT_MEMORY);

    outInterface->fpGetDeviceCapabilities = getDeviceCapabilitiesVK;
    outInterface->fpCreateDevice = createDeviceVK;
    outInterface->fpDestroyDevice = destroyDeviceVK;
    outInterface->fpCreateResource = createResourceVK;
    outInterface->fpRegisterResource = registerResourceVK;
    outInterface->fpUnregisterResources = unregisterResourcesVK;
    outInterface->fpGetResourceDescription = getResourceDescriptorVK;
    outInterface->fpDestroyResource = destroyResourceVK;
    outInterface->fpCreatePipeline = createPipelineVK;
    outInterface->fpDestroyPipeline = destroyPipelineVK;
    outInterface->fpScheduleRenderJob = scheduleRenderJobVK;
    outInterface->fpExecuteRenderJobs = executeRenderJobsVK;
    outInterface->scratchBuffer = scratchBuffer;
    outInterface->scratchBufferSize = scratchBufferSize;

    AnkiFsr2BackendCtx* context = reinterpret_cast<AnkiFsr2BackendCtx*>(scratchBuffer);
	context->m_r = renderer;

    return FFX_OK;
}

FFX_API FfxDevice ffxGetDeviceVK(GrManager* grManager)
{
	return FFX_API reinterpret_cast<FfxDevice>(grManager);
}

VkFormat getVKFormatFromSurfaceFormat(FfxSurfaceFormat fmt)
{
    switch (fmt) {

    case(FFX_SURFACE_FORMAT_R32G32B32A32_TYPELESS):
        return VK_FORMAT_R32G32B32A32_SFLOAT;
    case(FFX_SURFACE_FORMAT_R32G32B32A32_FLOAT):
        return VK_FORMAT_R32G32B32A32_SFLOAT;
    case(FFX_SURFACE_FORMAT_R16G16B16A16_FLOAT):
        return VK_FORMAT_R16G16B16A16_SFLOAT;
    case(FFX_SURFACE_FORMAT_R32G32_FLOAT):
        return VK_FORMAT_R32G32_SFLOAT;
    case(FFX_SURFACE_FORMAT_R32_UINT):
        return VK_FORMAT_R32_UINT;
    case(FFX_SURFACE_FORMAT_R8G8B8A8_TYPELESS):
        return VK_FORMAT_R8G8B8A8_UNORM;
    case(FFX_SURFACE_FORMAT_R8G8B8A8_UNORM):
        return VK_FORMAT_R8G8B8A8_UNORM;
    case(FFX_SURFACE_FORMAT_R11G11B10_FLOAT):
        return VK_FORMAT_B10G11R11_UFLOAT_PACK32;
    case(FFX_SURFACE_FORMAT_R16G16_FLOAT):
        return VK_FORMAT_R16G16_SFLOAT;
    case(FFX_SURFACE_FORMAT_R16G16_UINT):
        return VK_FORMAT_R16G16_UINT;
    case(FFX_SURFACE_FORMAT_R16_FLOAT):
        return VK_FORMAT_R16_SFLOAT;
    case(FFX_SURFACE_FORMAT_R16_UINT):
        return VK_FORMAT_R16_UINT;
    case(FFX_SURFACE_FORMAT_R16_UNORM):
        return VK_FORMAT_R16_UNORM;
    case(FFX_SURFACE_FORMAT_R16_SNORM):
        return VK_FORMAT_R16_SNORM;
    case(FFX_SURFACE_FORMAT_R8_UNORM):
        return VK_FORMAT_R8_UNORM;
    case(FFX_SURFACE_FORMAT_R32_FLOAT):
        return VK_FORMAT_R32_SFLOAT;
    default:
        return VK_FORMAT_UNDEFINED;
    }
}

VkImageUsageFlags getVKImageUsageFlagsFromResourceUsage(FfxResourceUsage flags)
{
    VkImageUsageFlags ret = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (flags & FFX_RESOURCE_USAGE_RENDERTARGET) ret |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if (flags & FFX_RESOURCE_USAGE_UAV) ret |= (VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    return ret;
}

VkBufferUsageFlags getVKBufferUsageFlagsFromResourceUsage(FfxResourceUsage flags)
{
    if (flags & FFX_RESOURCE_USAGE_UAV)
        return VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    else
        return VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
}

VkImageType getVKImageTypeFromResourceType(FfxResourceType type)
{
    switch (type) {
    case(FFX_RESOURCE_TYPE_TEXTURE1D):
        return VK_IMAGE_TYPE_1D;
    case(FFX_RESOURCE_TYPE_TEXTURE2D):
        return VK_IMAGE_TYPE_2D;
    case(FFX_RESOURCE_TYPE_TEXTURE3D):
        return VK_IMAGE_TYPE_3D;
    default:
        return VK_IMAGE_TYPE_MAX_ENUM;
    }
}

VkImageLayout getVKImageLayoutFromResourceState(FfxResourceStates state)
{
    switch (state) {

    case(FFX_RESOURCE_STATE_GENERIC_READ):
        return VK_IMAGE_LAYOUT_GENERAL;
    case(FFX_RESOURCE_STATE_UNORDERED_ACCESS):
        return VK_IMAGE_LAYOUT_GENERAL;
    case(FFX_RESOURCE_STATE_COMPUTE_READ):
        return VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    case FFX_RESOURCE_STATE_COPY_SRC:
        return VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    case FFX_RESOURCE_STATE_COPY_DEST:
        return VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    default:
        return VK_IMAGE_LAYOUT_GENERAL;
    }
}

VkPipelineStageFlags getVKPipelineStageFlagsFromResourceState(FfxResourceStates state)
{
    switch (state) {

    case(FFX_RESOURCE_STATE_GENERIC_READ):
    case(FFX_RESOURCE_STATE_UNORDERED_ACCESS):
    case(FFX_RESOURCE_STATE_COMPUTE_READ):
        return VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    case FFX_RESOURCE_STATE_COPY_SRC:
    case FFX_RESOURCE_STATE_COPY_DEST:
        return VK_PIPELINE_STAGE_TRANSFER_BIT;
    default:
        return VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    }
}

VkAccessFlags getVKAccessFlagsFromResourceState(FfxResourceStates state)
{
    switch (state) {

    case(FFX_RESOURCE_STATE_GENERIC_READ):
        return VK_ACCESS_SHADER_READ_BIT;
    case(FFX_RESOURCE_STATE_UNORDERED_ACCESS):
        return VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    case(FFX_RESOURCE_STATE_COMPUTE_READ):
        return VK_ACCESS_SHADER_READ_BIT;
    case FFX_RESOURCE_STATE_COPY_SRC:
        return VK_ACCESS_TRANSFER_READ_BIT;
    case FFX_RESOURCE_STATE_COPY_DEST:
        return VK_ACCESS_TRANSFER_WRITE_BIT;
    default:
        return VK_ACCESS_SHADER_READ_BIT;
    }
}

FfxSurfaceFormat ffxGetSurfaceFormat(Format fmt)
{
    switch (fmt) {

    case(Format::R32G32B32A32_SFLOAT):
        return FFX_SURFACE_FORMAT_R32G32B32A32_FLOAT;
	case(Format::R16G16B16A16_SFLOAT):
        return FFX_SURFACE_FORMAT_R16G16B16A16_FLOAT;
	case(Format::R32G32_SFLOAT):
        return FFX_SURFACE_FORMAT_R32G32_FLOAT;
	case(Format::R32_UINT):
        return FFX_SURFACE_FORMAT_R32_UINT;
	case(Format::R8G8B8A8_UNORM):
        return FFX_SURFACE_FORMAT_R8G8B8A8_UNORM;
	case(Format::B10G11R11_UFLOAT_PACK32):
        return FFX_SURFACE_FORMAT_R11G11B10_FLOAT;
	case(Format::R16G16_SFLOAT):
        return FFX_SURFACE_FORMAT_R16G16_FLOAT;
	case(Format::R16G16_UINT):
        return FFX_SURFACE_FORMAT_R16G16_UINT;
	case(Format::R16_SFLOAT):
        return FFX_SURFACE_FORMAT_R16_FLOAT;
	case(Format::R16_UINT):
        return FFX_SURFACE_FORMAT_R16_UINT;
	case(Format::R16_UNORM):
        return FFX_SURFACE_FORMAT_R16_UNORM;
	case(Format::R16_SNORM):
        return FFX_SURFACE_FORMAT_R16_SNORM;
	case(Format::R8_UNORM):
        return FFX_SURFACE_FORMAT_R8_UNORM;
	case(Format::R32_SFLOAT):
        return FFX_SURFACE_FORMAT_R32_FLOAT;
    default:
        return FFX_SURFACE_FORMAT_UNKNOWN;
    }
}

FfxResource ffxGetTextureResourceVK(FfxFsr2Context* context, const TexturePtr& texture,
									const TextureViewPtr& textureView, uint32_t width,
									uint32_t height, wchar_t* name, FfxResourceStates state)
{
    FfxResource resource = {};
	resource.resource = reinterpret_cast<void*>(texture.get());
    resource.state = state;
	resource.descriptorData = reinterpret_cast<uint64_t>(textureView.get());
    resource.description.flags = FFX_RESOURCE_FLAGS_NONE;
    resource.description.type = FFX_RESOURCE_TYPE_TEXTURE2D;
    resource.description.width = width;
    resource.description.height = height;
    resource.description.depth = 1;
    resource.description.mipCount = 1;
	resource.description.format = ffxGetSurfaceFormat(texture->getFormat());

    switch(texture->getFormat())
    {
	case Format::D16_UNORM:
	case Format::D32_SFLOAT:
	case Format::D16_UNORM_S8_UINT:
	case Format::D24_UNORM_S8_UINT:
	case Format::D32_SFLOAT_S8_UINT:
    {
        resource.isDepth = true;
        break;
    }
    default:
    {
        resource.isDepth = false;
        break;
    }
    }

#ifdef _DEBUG
    if (name) {
        wcscpy_s(resource.name, name);
    }
#endif

    return resource;
}

FfxErrorCode registerResourceVK(
    FfxFsr2Interface* backendInterface,
    const FfxResource* inFfxResource,
    FfxResourceInternal* outFfxResourceInternal
)
{
    FFX_ASSERT(NULL != backendInterface);

    AnkiFsr2BackendCtx* backendContext = (AnkiFsr2BackendCtx*)(backendInterface->scratchBuffer);

    if (inFfxResource->resource == nullptr) {

        outFfxResourceInternal->internalIndex = FFX_FSR2_RESOURCE_IDENTIFIER_NULL;
        return FFX_OK;
    }

    FFX_ASSERT(backendContext->nextDynamicResource > backendContext->nextStaticResource);
    outFfxResourceInternal->internalIndex = backendContext->nextDynamicResource--;

    AnkiFsr2BackendCtx::Resource* backendResource = &backendContext->resources[outFfxResourceInternal->internalIndex];

    backendResource->resourceDescription = inFfxResource->description;
    backendResource->state = inFfxResource->state;
    backendResource->undefined = false;

#ifdef _DEBUG
    size_t retval = 0;
    wcstombs_s(&retval, backendResource->resourceName, sizeof(backendResource->resourceName), inFfxResource->name, sizeof(backendResource->resourceName));
    if (retval >= 64) backendResource->resourceName[63] = '\0';
#endif

    if (inFfxResource->description.type == FFX_RESOURCE_TYPE_BUFFER)
    {
		Buffer* buffer = reinterpret_cast<Buffer*>(inFfxResource->resource);

        backendResource->m_bufferResource = BufferPtr(buffer);
    }
    else
    {
		Texture* image = reinterpret_cast<Texture*>(inFfxResource->resource);
		TextureView* imageView = reinterpret_cast<TextureView*>(inFfxResource->descriptorData);

        backendResource->m_imageResource = TexturePtr(image);
 
        if(image && imageView)
		{
            backendResource->m_allMipsImageView = TextureViewPtr(imageView);
			backendResource->m_singleMipImageViews[0] = TextureViewPtr(imageView);
        }
    }
    
    return FFX_OK;
}

// dispose dynamic resources: This should be called at the end of the frame
FfxErrorCode unregisterResourcesVK(FfxFsr2Interface* backendInterface)
{
    FFX_ASSERT(NULL != backendInterface);

    AnkiFsr2BackendCtx* backendContext = (AnkiFsr2BackendCtx*)(backendInterface->scratchBuffer);

    backendContext->nextDynamicResource = ANKI_FSR2_MAX_RESOURCE_COUNT - 1;

    return FFX_OK;
}

FfxErrorCode getDeviceCapabilitiesVK(FfxFsr2Interface* backendInterface, FfxDeviceCapabilities* deviceCapabilities, FfxDevice device)
{
	AnkiFsr2BackendCtx* backendContext = reinterpret_cast<AnkiFsr2BackendCtx*>(backendInterface->scratchBuffer);
	const GpuDeviceCapabilities& caps = backendContext->m_gr->getDeviceCapabilities();
	// no shader model in vulkan so assume the minimum
	deviceCapabilities->minimumSupportedShaderModel = FFX_SHADER_MODEL_5_1;
	deviceCapabilities->waveLaneCountMin = caps.m_minSubgroupSize;
	deviceCapabilities->waveLaneCountMax = caps.m_maxSubgroupSize;
	deviceCapabilities->fp16Supported = caps.m_rayTracingEnabled;
	deviceCapabilities->raytracingSupported = false;
    return FFX_OK;
}

FfxErrorCode createDeviceVK(FfxFsr2Interface* backendInterface, FfxDevice device)
{
	AnkiFsr2BackendCtx* backendContext = reinterpret_cast<AnkiFsr2BackendCtx*>(backendInterface->scratchBuffer);
	const ShaderProgramResourceVariant* variant;
	ResourceManager& resManager = backendContext->m_r->getResourceManager();

#	define ANKI_LOAD_FSR2_SHADER_PROGRAM(PASS, SHADER_PATH)														\
		if(resManager.loadResource(SHADER_PATH, backendContext->m_progs[PASS]) != Error::NONE)					\
		{																										\
			ANKI_GR_LOGE("Failed to load Fsr2 shader");															\
		}																										\
		backendContext->m_progs[PASS]->getOrCreateVariant(variant);	\
		backendContext->m_grProgs[PASS] = variant->getProgram();

	ANKI_LOAD_FSR2_SHADER_PROGRAM(FfxFsr2Pass::FFX_FSR2_PASS_PREPARE_INPUT_COLOR, "ShaderBinaries/Fsr2ComputeLuminancePyramid.ankiprogbin")
	ANKI_LOAD_FSR2_SHADER_PROGRAM(FfxFsr2Pass::FFX_FSR2_PASS_DEPTH_CLIP,
								  "ShaderBinaries/.ankiprogbin")
	ANKI_LOAD_FSR2_SHADER_PROGRAM(FfxFsr2Pass::FFX_FSR2_PASS_RECONSTRUCT_PREVIOUS_DEPTH, "ShaderBinaries/.ankiprogbin")
	ANKI_LOAD_FSR2_SHADER_PROGRAM(FfxFsr2Pass::FFX_FSR2_PASS_LOCK, "ShaderBinaries/.ankiprogbin")
	ANKI_LOAD_FSR2_SHADER_PROGRAM(FfxFsr2Pass::FFX_FSR2_PASS_ACCUMULATE, "ShaderBinaries/.ankiprogbin")
	ANKI_LOAD_FSR2_SHADER_PROGRAM(FfxFsr2Pass::FFX_FSR2_PASS_RCAS, "ShaderBinaries/.ankiprogbin")
	ANKI_LOAD_FSR2_SHADER_PROGRAM(FfxFsr2Pass::FFX_FSR2_PASS_COMPUTE_LUMINANCE_PYRAMID, "ShaderBinaries/.ankiprogbin")
	ANKI_LOAD_FSR2_SHADER_PROGRAM(FfxFsr2Pass::FFX_FSR2_PASS_GENERATE_REACTIVE, "ShaderBinaries/.ankiprogbin")

#	undef ANKI_LOAD_FSR2_SHADER_PROGRAM

    return FFX_ERROR_BACKEND_API_ERROR; // FFX_OK;
}

FfxErrorCode destroyDeviceVK(FfxFsr2Interface* backendInterface, FfxDevice device)
{

	return FFX_ERROR_BACKEND_API_ERROR; // FFX_OK;
}

// create a internal resource that will stay alive until effect gets shut down
FfxErrorCode createResourceVK(
    FfxFsr2Interface* backendInterface, 
    const FfxCreateResourceDescription* createResourceDescription,
    FfxResourceInternal* outResource)
{

	return FFX_ERROR_BACKEND_API_ERROR; // FFX_OK;
}

FfxResourceDescription getResourceDescriptorVK(FfxFsr2Interface* backendInterface, FfxResourceInternal resource)
{
	ANKI_ASSERT(0 && "TO IMPLEMENT!");
    FfxResourceDescription desc = {};
    return desc;
}

FfxErrorCode createPipelineVK(FfxFsr2Interface* backendInterface, FfxFsr2Pass pass, const FfxPipelineDescription* pipelineDescription, FfxPipelineState* outPipeline)
{

	return FFX_ERROR_BACKEND_API_ERROR; // FFX_OK;
}

FfxErrorCode scheduleRenderJobVK(FfxFsr2Interface* backendInterface, const FfxRenderJobDescription* job)
{
    FFX_ASSERT(NULL != backendInterface);
    FFX_ASSERT(NULL != job);

    AnkiFsr2BackendCtx* backendContext = (AnkiFsr2BackendCtx*)backendInterface->scratchBuffer;

    FFX_ASSERT(backendContext->renderJobCount < ANKI_FSR2_MAX_RENDERJOBS);

    backendContext->renderJobs[backendContext->renderJobCount] = *job;

    if (job->jobType == FFX_RENDER_JOB_COMPUTE) {

        // needs to copy SRVs and UAVs in case they are on the stack only
        FfxComputeJobDescription* computeJob = &backendContext->renderJobs[backendContext->renderJobCount].computeJobDescriptor;
        const uint32_t numConstBuffers = job->computeJobDescriptor.pipeline.constCount;
        for (uint32_t currentRootConstantIndex = 0; currentRootConstantIndex < numConstBuffers; ++currentRootConstantIndex)
        {
            computeJob->cbs[currentRootConstantIndex].uint32Size = job->computeJobDescriptor.cbs[currentRootConstantIndex].uint32Size;
            memcpy(computeJob->cbs[currentRootConstantIndex].data, job->computeJobDescriptor.cbs[currentRootConstantIndex].data, computeJob->cbs[currentRootConstantIndex].uint32Size * sizeof(uint32_t));
        }
    }

    backendContext->renderJobCount++;

    return FFX_OK;
}

void addBarrier(AnkiFsr2BackendCtx* backendContext, FfxResourceInternal* resource, FfxResourceStates newState)
{
	ANKI_ASSERT(0 && "TO IMPLEMENT!");
}

void flushBarriers(AnkiFsr2BackendCtx* backendContext, VkCommandBuffer vkCommandBuffer)
{
	ANKI_ASSERT(0 && "TO IMPLEMENT!");
}

static FfxErrorCode executeRenderJobCompute(AnkiFsr2BackendCtx* backendContext, FfxRenderJobDescription* job, VkCommandBuffer vkCommandBuffer)
{
    //uint32_t               imageInfoIndex = 0;
    //uint32_t               bufferInfoIndex = 0;
    //uint32_t               descriptorWriteIndex = 0;
    //VkDescriptorImageInfo  imageInfos[FSR2_MAX_IMAGE_VIEWS];
    //VkDescriptorBufferInfo bufferInfos[FSR2_MAX_UNIFORM_BUFFERS];
    //VkWriteDescriptorSet   writeDatas[FSR2_MAX_IMAGE_VIEWS + FSR2_MAX_UNIFORM_BUFFERS];

    //AnkiFsr2BackendCtx::PipelineLayout* pipelineLayout = reinterpret_cast<AnkiFsr2BackendCtx::PipelineLayout*>(job->computeJobDescriptor.pipeline.rootSignature);

    // bind uavs
    //for (uint32_t uav = 0; uav < job->computeJobDescriptor.pipeline.uavCount; ++uav)
    //{
    //    addBarrier(backendContext, &job->computeJobDescriptor.uavs[uav], FFX_RESOURCE_STATE_UNORDERED_ACCESS);
	//
    //    AnkiFsr2BackendCtx::Resource ffxResource = backendContext->resources[job->computeJobDescriptor.uavs[uav].internalIndex];
	//
    //    writeDatas[descriptorWriteIndex] = {};
    //    writeDatas[descriptorWriteIndex].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    //    writeDatas[descriptorWriteIndex].dstSet = pipelineLayout->descriptorSets[pipelineLayout->descriptorSetIndex];
    //    writeDatas[descriptorWriteIndex].descriptorCount = 1;
    //    writeDatas[descriptorWriteIndex].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    //    writeDatas[descriptorWriteIndex].pImageInfo = &imageInfos[imageInfoIndex];
    //    writeDatas[descriptorWriteIndex].dstBinding = job->computeJobDescriptor.pipeline.uavResourceBindings[uav].slotIndex;
    //    writeDatas[descriptorWriteIndex].dstArrayElement = 0;
	//
    //    imageInfos[imageInfoIndex] = {};
    //    imageInfos[imageInfoIndex].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    //    imageInfos[imageInfoIndex].imageView = ffxResource.singleMipImageViews[job->computeJobDescriptor.uavMip[uav]];
	//
    //    imageInfoIndex++;
    //    descriptorWriteIndex++;
    //}

    // bind srvs
    //for (uint32_t srv = 0; srv < job->computeJobDescriptor.pipeline.srvCount; ++srv)
    //{
    //    addBarrier(backendContext, &job->computeJobDescriptor.srvs[srv], FFX_RESOURCE_STATE_COMPUTE_READ);
	//
    //    AnkiFsr2BackendCtx::Resource ffxResource = backendContext->resources[job->computeJobDescriptor.srvs[srv].internalIndex];
	//
    //    writeDatas[descriptorWriteIndex] = {};
    //    writeDatas[descriptorWriteIndex].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    //    writeDatas[descriptorWriteIndex].dstSet = pipelineLayout->descriptorSets[pipelineLayout->descriptorSetIndex];
    //    writeDatas[descriptorWriteIndex].descriptorCount = 1;
    //    writeDatas[descriptorWriteIndex].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    //    writeDatas[descriptorWriteIndex].pImageInfo = &imageInfos[imageInfoIndex];
    //    writeDatas[descriptorWriteIndex].dstBinding = job->computeJobDescriptor.pipeline.srvResourceBindings[srv].slotIndex;
    //    writeDatas[descriptorWriteIndex].dstArrayElement = 0;
	//
    //    imageInfos[imageInfoIndex] = {};
    //    imageInfos[imageInfoIndex].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    //    imageInfos[imageInfoIndex].imageView = ffxResource.allMipsImageView;
	//
    //    imageInfoIndex++;
    //    descriptorWriteIndex++;
    //}

    // update ubos
    //for (uint32_t i = 0; i < job->computeJobDescriptor.pipeline.constCount; ++i)
    //{
    //    writeDatas[descriptorWriteIndex] = {};
    //    writeDatas[descriptorWriteIndex].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    //    writeDatas[descriptorWriteIndex].dstSet = pipelineLayout->descriptorSets[pipelineLayout->descriptorSetIndex];
    //    writeDatas[descriptorWriteIndex].descriptorCount = 1;
    //    writeDatas[descriptorWriteIndex].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    //    writeDatas[descriptorWriteIndex].pBufferInfo = &bufferInfos[bufferInfoIndex];
    //    writeDatas[descriptorWriteIndex].dstBinding = job->computeJobDescriptor.pipeline.cbResourceBindings[i].slotIndex;
    //    writeDatas[descriptorWriteIndex].dstArrayElement = 0;
	//
    //    bufferInfos[bufferInfoIndex] = accquireDynamicUBO(backendContext, job->computeJobDescriptor.cbs[i].uint32Size * sizeof(uint32_t), job->computeJobDescriptor.cbs[i].data);
	//
    //    bufferInfoIndex++;
    //    descriptorWriteIndex++;
    //}

    // insert all the barriers 
    //flushBarriers(backendContext, vkCommandBuffer);

    // update all uavs and srvs
    //backendContext->vkFunctionTable.vkUpdateDescriptorSets(backendContext->device, descriptorWriteIndex, writeDatas, 0, nullptr);

    // bind pipeline
	//ANKI_VK_LOGI("ANKI: Call to vkCmdBindPipeline(), SRV: %d UAV: %d Const: %d",
	//			 job->computeJobDescriptor.pipeline.srvCount, job->computeJobDescriptor.pipeline.uavCount,
	//			 job->computeJobDescriptor.pipeline.constCount);
    //backendContext->vkFunctionTable.vkCmdBindPipeline(vkCommandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, reinterpret_cast<VkPipeline>(job->computeJobDescriptor.pipeline.pipeline));
	//ANKI_VK_LOGI("ANKI: END Call to vkCmdBindPipeline()");

    // bind descriptor sets 
    //VkDescriptorSet sets[] = {
    //    backendContext->samplerDescriptorSet,
    //    pipelineLayout->descriptorSets[pipelineLayout->descriptorSetIndex],
    //};

    //backendContext->vkFunctionTable.vkCmdBindDescriptorSets(vkCommandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipelineLayout->pipelineLayout, 0, 2, sets, 0, nullptr);

    // dispatch
    //backendContext->vkFunctionTable.vkCmdDispatch(vkCommandBuffer, job->computeJobDescriptor.dimensions[0], job->computeJobDescriptor.dimensions[1], job->computeJobDescriptor.dimensions[2]);

    // move to another descriptor set for the next compute render job so that we don't overwrite descriptors in-use
    //pipelineLayout->descriptorSetIndex++;

    //if (pipelineLayout->descriptorSetIndex >= FSR2_MAX_QUEUED_FRAMES)
    //    pipelineLayout->descriptorSetIndex = 0;

	return FFX_ERROR_BACKEND_API_ERROR; // FFX_OK;
}

static FfxErrorCode executeRenderJobCopy(AnkiFsr2BackendCtx* backendContext, FfxRenderJobDescription* job, VkCommandBuffer vkCommandBuffer)
{
	return FFX_ERROR_BACKEND_API_ERROR; // FFX_OK;
}

static FfxErrorCode executeRenderJobClearFloat(AnkiFsr2BackendCtx* backendContext, FfxRenderJobDescription* job, VkCommandBuffer vkCommandBuffer)
{
	return FFX_ERROR_BACKEND_API_ERROR; // FFX_OK;
}

FfxErrorCode executeRenderJobsVK(FfxFsr2Interface* backendInterface, FfxCommandList commandList)
{
    FFX_ASSERT(NULL != backendInterface);

    AnkiFsr2BackendCtx* backendContext = (AnkiFsr2BackendCtx*)backendInterface->scratchBuffer;

    FfxErrorCode errorCode = FFX_OK;

    // execute all renderjobs
    for (uint32_t i = 0; i < backendContext->renderJobCount; ++i)
    {
        FfxRenderJobDescription* renderJob = &backendContext->renderJobs[i];
        VkCommandBuffer vkCommandBuffer = reinterpret_cast<VkCommandBuffer>(commandList);

		ANKI_VK_LOGI("ANKI: Call to ExecuteRenderJobsVK(), Job: %d", renderJob->jobType);

        switch (renderJob->jobType)
        {
        case FFX_RENDER_JOB_CLEAR_FLOAT:
        {
            errorCode = executeRenderJobClearFloat(backendContext, renderJob, vkCommandBuffer);
            break;
        }
        case FFX_RENDER_JOB_COPY:
        {
            errorCode = executeRenderJobCopy(backendContext, renderJob, vkCommandBuffer);
            break;
        }
        case FFX_RENDER_JOB_COMPUTE:
        {
            errorCode = executeRenderJobCompute(backendContext, renderJob, vkCommandBuffer);
            break;
        }
        default:;
        }
    }

    // check the execute function returned cleanly.
    FFX_RETURN_ON_ERROR(
        errorCode == FFX_OK,
        FFX_ERROR_BACKEND_API_ERROR);

    backendContext->renderJobCount = 0;

    return FFX_OK;
}

FfxErrorCode destroyResourceVK(FfxFsr2Interface* backendInterface, FfxResourceInternal resource)
{
	return FFX_ERROR_BACKEND_API_ERROR; // FFX_OK;
}

FfxErrorCode destroyPipelineVK(FfxFsr2Interface* backendInterface, FfxPipelineState* pipeline)
{
	return FFX_ERROR_BACKEND_API_ERROR; // FFX_OK;
}

} // end namespace anki

#endif
