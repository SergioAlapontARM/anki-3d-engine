// Copyright (C) 2009-2022, Panagiotis Christopoulos Charitos and contributors.
// All rights reserved.
// Code licensed under the BSD License.
// http://www.anki3d.org/LICENSE

#include <ThirdParty/FidelityFX/fsr2/ffx-fsr2-api/ffx_fsr2.h>
#include <ThirdParty/FidelityFX/fsr2/ffx-fsr2-api/ffx_fsr2_private.h>
#include <Anki/Gr/Vulkan/fsr2_vk/vk/AnkiFsr2.h>

#include <string.h>
#include <math.h>
#include <stdlib.h>

#include <AnKi/Gr.h>
#include <AnKi/Renderer/Renderer.h>
#include <AnKi/Gr/Vulkan/Common.h>
#include <AnKi/Gr/Vulkan/GrUpscalerImpl.h>
#include <AnKi/Gr/Vulkan/GrManagerImpl.h>
#include <AnKi/Gr/CommandBuffer.h>

namespace anki {

// LUTs

// Data collected from shader files. Search for "#define FSR2_BIND_"
class Fsr2ResourceBinding
{
public:
	CString m_name;
	U32 m_binding;
};

static Fsr2ResourceBinding g_PrepareInputColorUAVBindings[] = {
	{"rw_ReconstructedPrevNearestDepth", 2}, {"rw_prepared_input_color", 3}, {"rw_luma_history", 4}};
static Fsr2ResourceBinding g_PrepareInputColorSRVBindings[] = {{"r_input_color_jittered", 0}, {"r_exposure", 1}};
static Fsr2ResourceBinding g_PrepareInputColorConstBindings[] = {{"cbFSR2", 5}};

static Fsr2ResourceBinding g_DepthClipUAVBindings[] = {{"rw_depth_clip", 3}};
static Fsr2ResourceBinding g_DepthClipSRVBindings[] = {
	{"r_ReconstructedPrevNearestDepth", 0}, {"r_dilated_motion_vectors", 1}, {"r_dilatedDepth", 2}};
static Fsr2ResourceBinding g_DepthClipConstBindings[] = {{"cbFSR2", 4}};

static Fsr2ResourceBinding g_ReconstructPrevDepthUAVBindings[] = {
	{"rw_ReconstructedPrevNearestDepth", 2}, {"rw_dilated_motion_vectors", 3}, {"rw_dilatedDepth", 4}};
static Fsr2ResourceBinding g_ReconstructPrevDepthSRVBindings[] = {{"r_motion_vectors", 0}, {"r_depth", 1}};
static Fsr2ResourceBinding g_ReconstructPrevDepthConstBindings[] = {{"cbFSR2", 5}};

static Fsr2ResourceBinding g_LockUAVBindings[] = {{"rw_lock_status", 3}, {"rw_reactive_max", 4}};
static Fsr2ResourceBinding g_LockSRVBindings[] = {
	{"r_reactive_mask", 0}, {"r_lock_status", 1}, {"r_prepared_input_color", 2}};
static Fsr2ResourceBinding g_LockConstBindings[] = {{"cbFSR2", 5}};

static Fsr2ResourceBinding g_AccumulateUAVBindings[] = {
	{"rw_internal_upscaled_color", 12}, {"rw_lock_status", 13}, {"rw_upscaled_output", 14}};
static Fsr2ResourceBinding g_AccumulateSRVBindings[] = {{"r_exposure", 0},
														{"r_transparency_and_composition_mask", 1},
														{"r_dilated_motion_vectors", 2},
														{"r_internal_upscaled_color", 3},
														{"r_lock_status", 4},
														{"r_depth_clip", 5},
														{"r_prepared_input_color", 6},
														{"r_luma_history", 7},
														{"r_lanczos_lut", 8},
														{"r_upsample_maximum_bias_lut", 9},
														{"r_reactive_max", 10},
														{"r_imgMips", 11}};
static Fsr2ResourceBinding g_AccumulateConstBindings[] = {{"cbFSR2", 15}};

static Fsr2ResourceBinding g_AccumulateSharpenUAVBindings[] = {{"rw_internal_upscaled_color", 12},
															   {"rw_lock_status", 13}};
static Fsr2ResourceBinding g_AccumulateSharpenSRVBindings[] = {{"r_exposure", 0},
															   {"r_transparency_and_composition_mask", 1},
															   {"r_dilated_motion_vectors", 2},
															   {"r_internal_upscaled_color", 3},
															   {"r_lock_status", 4},
															   {"r_depth_clip", 5},
															   {"r_prepared_input_color", 6},
															   {"r_luma_history", 7},
															   {"r_lanczos_lut", 8},
															   {"r_upsample_maximum_bias_lut", 9},
															   {"r_reactive_max", 10},
															   {"r_imgMips", 11}};
static Fsr2ResourceBinding g_AccumulateSharpenConstBindings[] = {{"cbFSR2", 15}};

static Fsr2ResourceBinding g_RcasUAVBindings[] = {{"rw_upscaled_output", 2}};
static Fsr2ResourceBinding g_RcasSRVBindings[] = {{"r_exposure", 0}, {"r_rcas_input", 1}};
static Fsr2ResourceBinding g_RcasConstBindings[] = {{"cbFSR2", 3}, {"cbRCAS", 4}};

static Fsr2ResourceBinding g_ComputeLuminancePyramidUAVBindings[] = {
	{"rw_spd_global_atomic", 1}, {"rw_img_mip_shading_change", 2}, {"rw_img_mip_5", 3}, {"rw_exposure", 4}};
static Fsr2ResourceBinding g_ComputeLuminancePyramidSRVBindings[] = {{"r_input_color_jittered", 0}};
static Fsr2ResourceBinding g_ComputeLuminancePyramidConstBindings[] = {{"cbFSR2", 5}, {"cbSPD", 6}};

static Fsr2ResourceBinding g_GenerateReactiveUAVBindings[] = {{"rw_output_reactive_mask", 2}};
static Fsr2ResourceBinding g_GenerateReactiveSRVBindings[] = {{"r_input_color_pre_alpha", 0},
															  {"r_input_color_post_alpha", 1}};
static Fsr2ResourceBinding g_GenerateReactiveConstBindings[] = {{"cbGenerateReactive", 3}, {"cbFSR2", 4}};

class Fsr2PipelineReflection
{
public:
	U32 m_uavCount = 0;
	U32 m_srvCount = 0;
	U32 m_constCount = 0;
	Fsr2ResourceBinding* m_uavBindings = nullptr;
	Fsr2ResourceBinding* m_srvBindings = nullptr;
	Fsr2ResourceBinding* m_constBindings = nullptr;
};
static Fsr2PipelineReflection g_shaderReflection[] = {
	// FFX_FSR2_PASS_PREPARE_INPUT_COLOR
	{3, 2, 1, g_PrepareInputColorUAVBindings, g_PrepareInputColorSRVBindings, g_PrepareInputColorConstBindings},
	// FFX_FSR2_PASS_DEPTH_CLIP
	{1, 3, 1, g_DepthClipUAVBindings, g_DepthClipSRVBindings, g_DepthClipConstBindings},
	// FFX_FSR2_PASS_RECONSTRUCT_PREVIOUS_DEPTH
	{3, 2, 1, g_ReconstructPrevDepthUAVBindings, g_ReconstructPrevDepthSRVBindings, g_ReconstructPrevDepthConstBindings},
	// FFX_FSR2_PASS_LOCK
	{2, 3, 1, g_LockUAVBindings, g_LockSRVBindings, g_LockConstBindings},
	// FFX_FSR2_PASS_ACCUMULATE
	{3, 12, 1, g_AccumulateUAVBindings, g_AccumulateSRVBindings, g_AccumulateConstBindings},
	// FFX_FSR2_PASS_ACCUMULATE_SHARPEN
	{2, 12, 1, g_AccumulateSharpenUAVBindings, g_AccumulateSharpenSRVBindings, g_AccumulateSharpenConstBindings},
	// FFX_FSR2_PASS_RCAS
	{1, 2, 2, g_RcasUAVBindings, g_RcasSRVBindings, g_RcasConstBindings},
	// FFX_FSR2_PASS_COMPUTE_LUMINANCE_PYRAMID
	{4, 1, 2, g_ComputeLuminancePyramidUAVBindings, g_ComputeLuminancePyramidSRVBindings, g_ComputeLuminancePyramidConstBindings},
	// FFX_FSR2_PASS_GENERATE_REACTIVE
	{1, 2, 1, g_GenerateReactiveUAVBindings, g_GenerateReactiveSRVBindings, g_GenerateReactiveConstBindings}
};

// Utilities

static Format ffxGetFormatFromSurfaceFormat(FfxSurfaceFormat fmt)
{
	switch(fmt)
	{

	case(FFX_SURFACE_FORMAT_R32G32B32A32_TYPELESS):
		return Format::R32G32B32A32_SFLOAT;
	case(FFX_SURFACE_FORMAT_R32G32B32A32_FLOAT):
		return Format::R32G32B32A32_SFLOAT;
	case(FFX_SURFACE_FORMAT_R16G16B16A16_FLOAT):
		return Format::R16G16B16A16_SFLOAT;
	case(FFX_SURFACE_FORMAT_R32G32_FLOAT):
		return Format::R32G32_SFLOAT;
	case(FFX_SURFACE_FORMAT_R32_UINT):
		return Format::R32_UINT;
	case(FFX_SURFACE_FORMAT_R8G8B8A8_TYPELESS):
		return Format::R8G8B8A8_UNORM;
	case(FFX_SURFACE_FORMAT_R8G8B8A8_UNORM):
		return Format::R8G8B8A8_UNORM;
	case(FFX_SURFACE_FORMAT_R11G11B10_FLOAT):
		return Format::B10G11R11_UFLOAT_PACK32;
	case(FFX_SURFACE_FORMAT_R16G16_FLOAT):
		return Format::R16G16_SFLOAT;
	case(FFX_SURFACE_FORMAT_R16G16_UINT):
		return Format::R16G16_UINT;
	case(FFX_SURFACE_FORMAT_R16_FLOAT):
		return Format::R16_SFLOAT;
	case(FFX_SURFACE_FORMAT_R16_UINT):
		return Format::R16_UINT;
	case(FFX_SURFACE_FORMAT_R16_UNORM):
		return Format::R16_UNORM;
	case(FFX_SURFACE_FORMAT_R16_SNORM):
		return Format::R16_SNORM;
	case(FFX_SURFACE_FORMAT_R8_UNORM):
		return Format::R8_UNORM;
	case(FFX_SURFACE_FORMAT_R32_FLOAT):
		return Format::R32_SFLOAT;
	default:
		return Format::NONE;
	}
}

static TextureUsageBit ffxGetAnkiTextureUsageFromResourceUsage(FfxResourceUsage flags)
{
	TextureUsageBit usageFlags(TextureUsageBit::TRANSFER_DESTINATION | TextureUsageBit::ALL_SAMPLED
							   | TextureUsageBit::IMAGE_COMPUTE_READ);
	if(flags & FFX_RESOURCE_USAGE_RENDERTARGET)
	{
		usageFlags |= TextureUsageBit::ALL_FRAMEBUFFER_ATTACHMENT;
	}
	if(flags & FFX_RESOURCE_USAGE_UAV)
	{
		usageFlags |= TextureUsageBit::IMAGE_COMPUTE_READ | TextureUsageBit::IMAGE_COMPUTE_WRITE;
	}

	return usageFlags;
}

static BufferUsageBit ffxGetAnkiBufferUsageFlagsFromResourceUsage(FfxResourceUsage flags)
{
	if(flags & FFX_RESOURCE_USAGE_UAV)
	{
		return BufferUsageBit::STORAGE_COMPUTE_READ | BufferUsageBit::STORAGE_COMPUTE_WRITE | BufferUsageBit::ALL_TRANSFER;
	}
	return BufferUsageBit::ALL_UNIFORM;
}

static TextureUsageBit ffxGetTextureUsageFromResourceState(FfxResourceStates state)
{
	switch(state)
	{

	case(FFX_RESOURCE_STATE_GENERIC_READ):
		return TextureUsageBit::SAMPLED_COMPUTE | TextureUsageBit::IMAGE_COMPUTE_READ;
	case(FFX_RESOURCE_STATE_UNORDERED_ACCESS):
		return TextureUsageBit::IMAGE_COMPUTE_READ | TextureUsageBit::IMAGE_COMPUTE_WRITE;
	case(FFX_RESOURCE_STATE_COMPUTE_READ):
		return TextureUsageBit::SAMPLED_COMPUTE;
	case FFX_RESOURCE_STATE_COPY_SRC:
		return TextureUsageBit::ALL_GRAPHICS;
	case FFX_RESOURCE_STATE_COPY_DEST:
		return TextureUsageBit::TRANSFER_DESTINATION;
	default:
		return TextureUsageBit::ALL_GRAPHICS;
	}
}

static BufferUsageBit ffxGetBufferUsageFromResourceState(FfxResourceStates state)
{
	switch(state)
	{
	case(FFX_RESOURCE_STATE_GENERIC_READ):
		return BufferUsageBit::STORAGE_COMPUTE_READ | BufferUsageBit::TEXTURE_COMPUTE_READ;
	case(FFX_RESOURCE_STATE_UNORDERED_ACCESS):
		return BufferUsageBit::STORAGE_COMPUTE_READ | BufferUsageBit::STORAGE_COMPUTE_WRITE;
	case(FFX_RESOURCE_STATE_COMPUTE_READ):
		return BufferUsageBit::STORAGE_COMPUTE_READ | BufferUsageBit::TEXTURE_COMPUTE_READ;
	case FFX_RESOURCE_STATE_COPY_SRC:
		return BufferUsageBit::TRANSFER_SOURCE;
	case FFX_RESOURCE_STATE_COPY_DEST:
		return BufferUsageBit::TRANSFER_DESTINATION;
	default:
		return BufferUsageBit::ALL_GRAPHICS;
	}
}

static FfxSurfaceFormat ffxGetSurfaceFormat(Format fmt)
{
	switch(fmt)
	{

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

// Anki-Fsr2 bindings
// prototypes for functions in the interface
FfxErrorCode ankiFsr2GetDeviceCapabilities(FfxFsr2Interface* backendInterface, FfxDeviceCapabilities* deviceCapabilities, FfxDevice device);
FfxErrorCode ankiFsr2CreateDevice(FfxFsr2Interface* backendInterface, FfxDevice device);
FfxErrorCode ankiFsr2DestroyDevice(FfxFsr2Interface* backendInterface, FfxDevice device);
FfxErrorCode ankiFsr2CreateResource(FfxFsr2Interface* backendInterface, const FfxCreateResourceDescription* desc,
							  FfxResourceInternal* outResource);
FfxErrorCode ankiFsr2RegisterResource(FfxFsr2Interface* backendInterface, const FfxResource* inResource, FfxResourceInternal* outResourceInternal);
FfxErrorCode ankiFsr2UnregisterResources(FfxFsr2Interface* backendInterface);
FfxResourceDescription ankiFsr2GetResourceDescriptor(FfxFsr2Interface* backendInterface, FfxResourceInternal resource);
FfxErrorCode ankiFsr2DestroyResource(FfxFsr2Interface* backendInterface, FfxResourceInternal resource);
FfxErrorCode ankiFsr2CreatePipeline(FfxFsr2Interface* backendInterface, FfxFsr2Pass passId, const FfxPipelineDescription* desc, FfxPipelineState* outPass);
FfxErrorCode ankiFsr2DestroyPipeline(FfxFsr2Interface* backendInterface, FfxPipelineState* pipeline);
FfxErrorCode ankiFsr2ScheduleRenderJob(FfxFsr2Interface* backendInterface, const FfxRenderJobDescription* job);
FfxErrorCode ankiFsr2ExecuteRenderJobs(FfxFsr2Interface* backendInterface, FfxCommandList commandList);

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

// store for resources and resourceViews
class AnkiFsr2Resource
{
public:
#	ifdef _DEBUG
	char m_resourceName[64] = {};
#	endif
	TexturePtr m_imageResource;
	BufferPtr m_bufferResource;
	TextureViewPtr m_allMipsImageView;
	TextureViewPtr m_singleMipImageViews[ANKI_FSR2_MAX_IMAGE_VIEWS];

	FfxResourceDescription resourceDescription;
	FfxResourceStates state;
	bool undefined = false;
};

class AnkiFsr2Material
{
public:
	ShaderProgramResourcePtr m_prog;
	ShaderProgramPtr m_grProg;
	FfxPipelineState m_refl;
	FfxFsr2Pass m_pass;
};

class AnkiFsr2BackendCtx
{
public:
    uint32_t                renderJobCount = 0;
    FfxRenderJobDescription renderJobs[ANKI_FSR2_MAX_RENDERJOBS] = {};

    uint32_t                nextStaticResource = 0;
    uint32_t                nextDynamicResource = 0;
	AnkiFsr2Resource		resources[ANKI_FSR2_MAX_RESOURCE_COUNT] = {};
	Renderer*				m_r = nullptr;

	// Shaders
	AnkiFsr2Material m_shaders[FfxFsr2Pass::FFX_FSR2_PASS_COUNT];
};

FFX_API size_t ffxFsr2GetScratchMemorySize()
{
    return FFX_ALIGN_UP(sizeof(AnkiFsr2BackendCtx), sizeof(uint64_t));
}

FfxErrorCode ffxFsr2GetInterface(
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
	FFX_RETURN_ON_ERROR(scratchBufferSize >= ffxFsr2GetScratchMemorySize(),
        FFX_ERROR_INSUFFICIENT_MEMORY);

	// Before we start working with the context, since the memory was weirdly allocated, do a new in place to make sure
	// we call all required constructors
	new(scratchBuffer) AnkiFsr2BackendCtx();

    outInterface->fpGetDeviceCapabilities = ankiFsr2GetDeviceCapabilities;
    outInterface->fpCreateDevice = ankiFsr2CreateDevice;
    outInterface->fpDestroyDevice = ankiFsr2DestroyDevice;
    outInterface->fpCreateResource = ankiFsr2CreateResource;
    outInterface->fpRegisterResource = ankiFsr2RegisterResource;
    outInterface->fpUnregisterResources = ankiFsr2UnregisterResources;
    outInterface->fpGetResourceDescription = ankiFsr2GetResourceDescriptor;
    outInterface->fpDestroyResource = ankiFsr2DestroyResource;
    outInterface->fpCreatePipeline = ankiFsr2CreatePipeline;
    outInterface->fpDestroyPipeline = ankiFsr2DestroyPipeline;
    outInterface->fpScheduleRenderJob = ankiFsr2ScheduleRenderJob;
    outInterface->fpExecuteRenderJobs = ankiFsr2ExecuteRenderJobs;
    outInterface->scratchBuffer = scratchBuffer;
    outInterface->scratchBufferSize = scratchBufferSize;

    AnkiFsr2BackendCtx* context = reinterpret_cast<AnkiFsr2BackendCtx*>(scratchBuffer);
	context->m_r = renderer;

    return FFX_OK;
}

FFX_API FfxDevice ffxGetDevice(GrManager* grManager)
{
	return FFX_API reinterpret_cast<FfxDevice>(grManager);
}

FFX_API FfxCommandList ffxGetCommandList(const CommandBufferPtr& cmdBuf)
{
	return FFX_API reinterpret_cast<FfxDevice>(cmdBuf.get());
}

FfxResource ffxGetTextureResource(const TexturePtr& texture,
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

FfxErrorCode ankiFsr2RegisterResource(
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

    AnkiFsr2Resource* backendResource = &backendContext->resources[outFfxResourceInternal->internalIndex];

    backendResource->resourceDescription = inFfxResource->description;
    backendResource->state = inFfxResource->state;

#ifdef _DEBUG
    size_t retval = 0;
	wcstombs_s(&retval, backendResource->m_resourceName, sizeof(backendResource->m_resourceName), inFfxResource->name,
			   sizeof(backendResource->m_resourceName));
	if(retval >= 64)
	{
		backendResource->m_resourceName[63] = '\0';
	}
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
FfxErrorCode ankiFsr2UnregisterResources(FfxFsr2Interface* backendInterface)
{
    FFX_ASSERT(NULL != backendInterface);

    AnkiFsr2BackendCtx* backendContext = reinterpret_cast<AnkiFsr2BackendCtx*>(backendInterface->scratchBuffer);

    backendContext->nextDynamicResource = ANKI_FSR2_MAX_RESOURCE_COUNT - 1;

    return FFX_OK;
}

FfxErrorCode ankiFsr2GetDeviceCapabilities(FfxFsr2Interface* backendInterface,
										   FfxDeviceCapabilities* deviceCapabilities, [[maybe_unused]] FfxDevice device)
{
	const AnkiFsr2BackendCtx& backendContext = *reinterpret_cast<AnkiFsr2BackendCtx*>(backendInterface->scratchBuffer);
	const GpuDeviceCapabilities& caps = backendContext.m_r->getGrManager().getDeviceCapabilities();
	// no shader model in vulkan so assume the minimum
	deviceCapabilities->minimumSupportedShaderModel = FFX_SHADER_MODEL_5_1;
	deviceCapabilities->waveLaneCountMin = caps.m_minSubgroupSize;
	deviceCapabilities->waveLaneCountMax = caps.m_maxSubgroupSize;
	deviceCapabilities->fp16Supported = caps.m_rayTracingEnabled;
	deviceCapabilities->raytracingSupported = false;
    return FFX_OK;
}

FfxErrorCode ankiFsr2CreateDevice(FfxFsr2Interface* backendInterface, [[maybe_unused]] FfxDevice device)
{
	AnkiFsr2BackendCtx& backendContext = *reinterpret_cast<AnkiFsr2BackendCtx*>(backendInterface->scratchBuffer);

	const ShaderProgramResourceVariant* variant;
	ResourceManager& resManager = backendContext.m_r->getResourceManager();

#	define ANKI_LOAD_FSR2_SHADER_PROGRAM(PASS, SHADER_PATH)														\
		if(resManager.loadResource(SHADER_PATH, backendContext.m_shaders[PASS].m_prog) != Error::NONE)			\
		{																										\
			ANKI_GR_LOGE("Failed to load Fsr2 shader");															\
		}																										\
		backendContext.m_shaders[PASS].m_pass = PASS;															\
		backendContext.m_shaders[PASS].m_prog->getOrCreateVariant(variant);										\
		backendContext.m_shaders[PASS].m_grProg = variant->getProgram();

	ANKI_LOAD_FSR2_SHADER_PROGRAM(FfxFsr2Pass::FFX_FSR2_PASS_PREPARE_INPUT_COLOR,
								  "ShaderBinaries/Fsr2PrepareInputColor.ankiprogbin")
	ANKI_LOAD_FSR2_SHADER_PROGRAM(FfxFsr2Pass::FFX_FSR2_PASS_DEPTH_CLIP, "ShaderBinaries/Fsr2DepthClip.ankiprogbin")
	ANKI_LOAD_FSR2_SHADER_PROGRAM(FfxFsr2Pass::FFX_FSR2_PASS_RECONSTRUCT_PREVIOUS_DEPTH,
								  "ShaderBinaries/Fsr2ReconstructPreviousDepth.ankiprogbin")
	ANKI_LOAD_FSR2_SHADER_PROGRAM(FfxFsr2Pass::FFX_FSR2_PASS_LOCK, "ShaderBinaries/Fsr2Lock.ankiprogbin")
	ANKI_LOAD_FSR2_SHADER_PROGRAM(FfxFsr2Pass::FFX_FSR2_PASS_ACCUMULATE, "ShaderBinaries/Fsr2Accumulate.ankiprogbin")
	ANKI_LOAD_FSR2_SHADER_PROGRAM(FfxFsr2Pass::FFX_FSR2_PASS_RCAS, "ShaderBinaries/Fsr2Rcas.ankiprogbin")
	ANKI_LOAD_FSR2_SHADER_PROGRAM(FfxFsr2Pass::FFX_FSR2_PASS_COMPUTE_LUMINANCE_PYRAMID,
								  "ShaderBinaries/Fsr2ComputeLuminancePyramid.ankiprogbin")
	ANKI_LOAD_FSR2_SHADER_PROGRAM(FfxFsr2Pass::FFX_FSR2_PASS_GENERATE_REACTIVE,
								  "ShaderBinaries/Fsr2GenerateReactive.ankiprogbin")

#	undef ANKI_LOAD_FSR2_SHADER_PROGRAM

	backendContext.nextStaticResource = 0;
	backendContext.nextDynamicResource = static_cast<U32>(ANKI_FSR2_MAX_RESOURCE_COUNT - 1);

    return FFX_OK;
}

FfxErrorCode ankiFsr2DestroyDevice([[maybe_unused]] FfxFsr2Interface* backendInterface,
								   [[maybe_unused]] FfxDevice device)
{
	// Memory will be deallocated by the user so at least call the destructor to safely release resources hold by the context
	AnkiFsr2BackendCtx* backendContext = reinterpret_cast<AnkiFsr2BackendCtx*>(backendInterface->scratchBuffer);
	backendContext->~AnkiFsr2BackendCtx();
	return FFX_OK;
}

Error ankiFsr2CreateBufferResource(const FfxCreateResourceDescription& createResourceDescription,
						   AnkiFsr2BackendCtx& backendContext, AnkiFsr2Resource& res)
{
	Renderer& renderer = *backendContext.m_r;
	GrManager& grManager = renderer.getGrManager();

	// Create buffer
	BufferInitInfo buffInit("Fsr2Buffer");
#	ifdef _DEBUG
	buffInit.setName(res.m_resourceName);
#	endif
	buffInit.m_mapAccess = BufferMapAccessBit::READ;
	buffInit.m_size = createResourceDescription.resourceDescription.width;
	buffInit.m_usage = ffxGetAnkiBufferUsageFlagsFromResourceUsage(createResourceDescription.usage);
	res.m_bufferResource = grManager.newBuffer(buffInit);

	if(createResourceDescription.initData)
	{
		void* bufferAddr = res.m_bufferResource->map(0, buffInit.m_size, BufferMapAccessBit::READ);
		memcpy(bufferAddr, createResourceDescription.initData, createResourceDescription.initDataSize);
		res.m_bufferResource->unmap();
	}
	return Error::NONE;
}

Error ankiFsr2CreateTextureResource(const FfxCreateResourceDescription& createResourceDescription,
							AnkiFsr2BackendCtx& backendContext, AnkiFsr2Resource& res)
{
	Renderer& renderer = *backendContext.m_r;
	GrManager& grManager = renderer.getGrManager();

	// Create texture resource
	TextureInitInfo texInit;
#	ifdef _DEBUG
	texInit.setName(res.m_resourceName);
#	endif
	texInit.m_width = createResourceDescription.resourceDescription.width;
	texInit.m_height = createResourceDescription.resourceDescription.height;
	texInit.m_depth = (createResourceDescription.resourceDescription.type == FFX_RESOURCE_TYPE_TEXTURE3D) ?
		createResourceDescription.resourceDescription.depth : 1;
	texInit.m_layerCount = 1;
	texInit.m_format = ffxGetFormatFromSurfaceFormat(createResourceDescription.resourceDescription.format);
	texInit.m_usage = ffxGetAnkiTextureUsageFromResourceUsage(createResourceDescription.usage);
	texInit.m_usage |=
		createResourceDescription.initData ? TextureUsageBit::TRANSFER_DESTINATION : TextureUsageBit::NONE;
	texInit.m_type = static_cast<TextureType>(createResourceDescription.resourceDescription.type - 1u);
	texInit.m_mipmapCount = static_cast<U8>(res.resourceDescription.mipCount);
	texInit.m_samples = 1u;
	res.m_imageResource = grManager.newTexture(texInit);

	// Create view/s
	TextureViewInitInfo viewInit(res.m_imageResource, TextureSubresourceInfo(TextureSurfaceInfo(0, 0, 0, 0)),
								 "Fsr2Texture");

	viewInit.m_mipmapCount = texInit.m_mipmapCount;
	res.m_allMipsImageView = grManager.newTextureView(viewInit);

	if(res.resourceDescription.mipCount == 1)
	{
		res.m_singleMipImageViews[0] = res.m_allMipsImageView;
	}
	else
	{
		ANKI_ASSERT(!createResourceDescription.initData
					&& "We don't need to handle mipmap initialization with data here");
		viewInit.m_mipmapCount = 1;
		for(U32 mip = 0; mip < res.resourceDescription.mipCount; ++mip)
		{
			viewInit.m_firstMipmap = mip;
			res.m_singleMipImageViews[mip] = grManager.newTextureView(viewInit);
		}
	}

	// Init with data if neeeded
	if(createResourceDescription.initData)
	{
		TransferGpuAllocatorHandle handle;
		ANKI_CHECK(renderer.getResourceManager().getTransferGpuAllocator().allocate(createResourceDescription.initDataSize, handle));
		void* data = handle.getMappedMemory();
		ANKI_ASSERT(data);
		memcpy(data, createResourceDescription.initData, createResourceDescription.initDataSize);


		const TextureSurfaceInfo surf(0, 0, 0, 0);
		CommandBufferInitInfo cmdbInit;
		cmdbInit.m_flags = CommandBufferFlag::GENERAL_WORK | CommandBufferFlag::SMALL_BATCH;
		CommandBufferPtr cmdb = grManager.newCommandBuffer(cmdbInit);

		cmdb->setTextureSurfaceBarrier(res.m_imageResource, TextureUsageBit::NONE,
									   TextureUsageBit::TRANSFER_DESTINATION,
									   surf);
		cmdb->copyBufferToTextureView(handle.getBuffer(), handle.getOffset(), handle.getRange(),
									  res.m_singleMipImageViews[0]);
		const TextureUsageBit initialUsage = ffxGetTextureUsageFromResourceState(createResourceDescription.initalState);
		cmdb->setTextureSurfaceBarrier(res.m_imageResource, TextureUsageBit::TRANSFER_DESTINATION, initialUsage, surf);
		FencePtr fence;
		cmdb->flush({}, &fence);

		renderer.getResourceManager().getTransferGpuAllocator().release(handle, fence);

		res.state = createResourceDescription.initalState;
		res.undefined = false;
	}
	return Error::NONE;
}

// create a internal resource that will stay alive until effect gets shut down
FfxErrorCode ankiFsr2CreateResource(
    FfxFsr2Interface* backendInterface, 
    const FfxCreateResourceDescription* createResourceDescription,
    FfxResourceInternal* outResource)
{
	FFX_ASSERT(NULL != backendInterface);
	FFX_ASSERT(NULL != createResourceDescription);
	FFX_ASSERT(NULL != outResource);

	AnkiFsr2BackendCtx& backendContext = *reinterpret_cast<AnkiFsr2BackendCtx*>(backendInterface->scratchBuffer);

	FFX_ASSERT(backendContext.nextStaticResource + 1 < backendContext.nextDynamicResource);
	outResource->internalIndex = backendContext.nextStaticResource++;
	AnkiFsr2Resource& res = backendContext.resources[outResource->internalIndex];
	res.resourceDescription = createResourceDescription->resourceDescription;
	res.state = createResourceDescription->initalState;
	res.undefined = true;

	if(res.resourceDescription.mipCount == 0)
	{
		res.resourceDescription.mipCount =	1u + log2(FFX_MAXIMUM(FFX_MAXIMUM(createResourceDescription->resourceDescription.width,
													createResourceDescription->resourceDescription.height),
													createResourceDescription->resourceDescription.depth));
	}
#	ifdef _DEBUG
	size_t retval = 0;
	wcstombs_s(&retval, res.m_resourceName, sizeof(res.m_resourceName), createResourceDescription->name,
			   sizeof(res.m_resourceName));
	if(retval >= 64)
		res.m_resourceName[63] = '\0';
#	endif

	switch(createResourceDescription->resourceDescription.type)
	{
	case FFX_RESOURCE_TYPE_BUFFER:
	{
		if(ankiFsr2CreateBufferResource(*createResourceDescription, backendContext, res) != Error::NONE)
		{
			return FFX_ERROR_BACKEND_API_ERROR;
		}
		break;
	}
	case FFX_RESOURCE_TYPE_TEXTURE1D:
	case FFX_RESOURCE_TYPE_TEXTURE2D:
	case FFX_RESOURCE_TYPE_TEXTURE3D:
	{
		if(ankiFsr2CreateTextureResource(*createResourceDescription, backendContext, res) != Error::NONE)
		{
			return FFX_ERROR_BACKEND_API_ERROR;
		}
		break;
	}
	default:;
	}

	return FFX_OK;
}

FfxResourceDescription ankiFsr2GetResourceDescriptor(FfxFsr2Interface* backendInterface, FfxResourceInternal resource)
{
	FFX_ASSERT(NULL != backendInterface);
	AnkiFsr2BackendCtx& backendContext = *reinterpret_cast<AnkiFsr2BackendCtx*>(backendInterface->scratchBuffer);
	if(resource.internalIndex != -1)
	{
		const FfxResourceDescription& desc = backendContext.resources[resource.internalIndex].resourceDescription;
		return desc;
	}
	return FfxResourceDescription();
}

FfxErrorCode ankiFsr2CreatePipeline(FfxFsr2Interface* backendInterface, FfxFsr2Pass pass, const FfxPipelineDescription* pipelineDescription, FfxPipelineState* outPipeline)
{
	(void)pipelineDescription;

	AnkiFsr2BackendCtx& backendContext = *reinterpret_cast<AnkiFsr2BackendCtx*>(backendInterface->scratchBuffer);

	const Fsr2PipelineReflection& refl = g_shaderReflection[pass];

	outPipeline->srvCount = refl.m_srvCount;
	outPipeline->uavCount = refl.m_uavCount;
	outPipeline->constCount = refl.m_constCount;

	for(uint32_t srvIndex = 0; srvIndex < outPipeline->srvCount; ++srvIndex)
	{
		outPipeline->srvResourceBindings[srvIndex].slotIndex = refl.m_srvBindings[srvIndex].m_binding;
		strcpy(outPipeline->srvResourceBindings[srvIndex].name, refl.m_srvBindings[srvIndex].m_name.cstr());
	}
	for(uint32_t uavIndex = 0; uavIndex < outPipeline->uavCount; ++uavIndex)
	{
		outPipeline->uavResourceBindings[uavIndex].slotIndex = refl.m_uavBindings[uavIndex].m_binding;
		strcpy(outPipeline->uavResourceBindings[uavIndex].name, refl.m_uavBindings[uavIndex].m_name.cstr());
	}
	for(uint32_t cbIndex = 0; cbIndex < outPipeline->constCount; ++cbIndex)
	{
		outPipeline->cbResourceBindings[cbIndex].slotIndex = refl.m_constBindings[cbIndex].m_binding;
		strcpy(outPipeline->cbResourceBindings[cbIndex].name, refl.m_constBindings[cbIndex].m_name.cstr());
	}

	outPipeline->pipeline = reinterpret_cast<FfxPipeline>(&backendContext.m_shaders[pass]);

	return FFX_OK;
}

FfxErrorCode ankiFsr2ScheduleRenderJob(FfxFsr2Interface* backendInterface, const FfxRenderJobDescription* job)
{
    FFX_ASSERT(NULL != backendInterface);
    FFX_ASSERT(NULL != job);

    AnkiFsr2BackendCtx& backendContext = *reinterpret_cast<AnkiFsr2BackendCtx*>(backendInterface->scratchBuffer);

    FFX_ASSERT(backendContext.renderJobCount < ANKI_FSR2_MAX_RENDERJOBS);

    backendContext.renderJobs[backendContext.renderJobCount] = *job;

    if (job->jobType == FFX_RENDER_JOB_COMPUTE) {

        // needs to copy SRVs and UAVs in case they are on the stack only
        FfxComputeJobDescription* computeJob = &backendContext.renderJobs[backendContext.renderJobCount].computeJobDescriptor;
        const uint32_t numConstBuffers = job->computeJobDescriptor.pipeline.constCount;
        for (uint32_t currentRootConstantIndex = 0; currentRootConstantIndex < numConstBuffers; ++currentRootConstantIndex)
        {
            computeJob->cbs[currentRootConstantIndex].uint32Size = job->computeJobDescriptor.cbs[currentRootConstantIndex].uint32Size;
            memcpy(computeJob->cbs[currentRootConstantIndex].data, job->computeJobDescriptor.cbs[currentRootConstantIndex].data, computeJob->cbs[currentRootConstantIndex].uint32Size * sizeof(uint32_t));
        }
    }

    backendContext.renderJobCount++;

    return FFX_OK;
}

void ankiFsr2AddBarrier(CommandBufferPtr& cmdBuffer, AnkiFsr2BackendCtx& backendContext, FfxResourceInternal& resource,
				FfxResourceStates newState)
{	
	AnkiFsr2Resource& ffxResource = backendContext.resources[resource.internalIndex];

	if(ffxResource.state == newState && !ffxResource.undefined)
	{
		return;
	}

	FfxResourceStates& curState = backendContext.resources[resource.internalIndex].state;
	if(ffxResource.resourceDescription.type == FFX_RESOURCE_TYPE_BUFFER)
	{
		const BufferUsageBit prevUsage =
			ffxResource.undefined ? BufferUsageBit::NONE : ffxGetBufferUsageFromResourceState(ffxResource.state);
		const BufferUsageBit nextUsage = ffxGetBufferUsageFromResourceState(newState);
		cmdBuffer->setBufferBarrier(ffxResource.m_bufferResource, prevUsage, nextUsage, 0,
									ffxResource.m_bufferResource->getSize());
	}
	else
	{
		const TextureUsageBit prevUsage = ffxResource.undefined ? TextureUsageBit::NONE : ffxGetTextureUsageFromResourceState(ffxResource.state);
		const TextureUsageBit nextUsage = ffxGetTextureUsageFromResourceState(newState);
		TextureSubresourceInfo info(TextureSurfaceInfo(0, 0, 0, 0));
		info.m_mipmapCount = ffxResource.resourceDescription.mipCount;
		cmdBuffer->setTextureBarrier(ffxResource.m_imageResource, prevUsage, nextUsage, info);
	}

	ffxResource.undefined = false;
	curState = newState;
}

static FfxErrorCode ankiFsr2ExecuteRenderJobCompute(AnkiFsr2BackendCtx& backendContext, FfxRenderJobDescription& job,
											CommandBufferPtr& cmdBuffer)
{
	FfxPipelineState& pipeline = job.computeJobDescriptor.pipeline;
    // bind uavs
	for(U32 uav = 0; uav < pipeline.uavCount; ++uav)
    {
        const AnkiFsr2Resource& ffxResource = backendContext.resources[job.computeJobDescriptor.uavs[uav].internalIndex];
		ankiFsr2AddBarrier(cmdBuffer, backendContext, job.computeJobDescriptor.uavs[uav], FFX_RESOURCE_STATE_UNORDERED_ACCESS);

		const U32 set = 1;
		const U32 binding = job.computeJobDescriptor.pipeline.uavResourceBindings[uav].slotIndex;
		const TextureViewPtr& textureView = ffxResource.m_singleMipImageViews[job.computeJobDescriptor.uavMip[uav]];
		cmdBuffer->bindImage(set, binding, textureView);
    }

    // bind srvs
    for (uint32_t srv = 0; srv < job.computeJobDescriptor.pipeline.srvCount; ++srv)
    {
		const AnkiFsr2Resource& ffxResource = backendContext.resources[job.computeJobDescriptor.srvs[srv].internalIndex];
		ankiFsr2AddBarrier(cmdBuffer, backendContext, job.computeJobDescriptor.srvs[srv], FFX_RESOURCE_STATE_COMPUTE_READ);

		const U32 set = 1;
		const U32 binding = job.computeJobDescriptor.pipeline.srvResourceBindings[srv].slotIndex;
		const TextureViewPtr& textureView = ffxResource.m_allMipsImageView;
		cmdBuffer->bindTexture(set, binding, textureView);
    }

    // update ubos
    for (uint32_t i = 0; i < job.computeJobDescriptor.pipeline.constCount; ++i)
    {
		StagingGpuMemoryToken token;
		const PtrSize reqSize = job.computeJobDescriptor.cbs[i].uint32Size * sizeof(uint32_t);
		const void* uniformData = job.computeJobDescriptor.cbs[i].data;
		const U32 set = 1;
		const U32 binding = job.computeJobDescriptor.pipeline.cbResourceBindings[i].slotIndex;
		void* dstPtr = backendContext.m_r->getStagingGpuMemory().allocateFrame(reqSize, StagingGpuMemoryType::UNIFORM, token);
		memcpy(dstPtr, uniformData, reqSize);
		cmdBuffer->bindUniformBuffer(set, binding, token.m_buffer, token.m_offset, token.m_range);
    }

	// Bind samplers
	const RendererPrecreatedSamplers& samplers = backendContext.m_r->getSamplers();
	cmdBuffer->bindSampler(0, 0, samplers.m_nearestNearestClamp);
	cmdBuffer->bindSampler(0, 1, samplers.m_trilinearClamp);

	// Bind shader
	const AnkiFsr2Material& shader = *reinterpret_cast<const AnkiFsr2Material*>(pipeline.pipeline);
	cmdBuffer->bindShaderProgram(shader.m_grProg);
    
    // dispatch
	const U32 dispatchGroupsX = job.computeJobDescriptor.dimensions[0];
	const U32 dispatchGroupsY = job.computeJobDescriptor.dimensions[1];
	const U32 dispatchGroupsZ = job.computeJobDescriptor.dimensions[2];
	cmdBuffer->dispatchCompute(dispatchGroupsX, dispatchGroupsY, dispatchGroupsZ);

	return FFX_OK;
}

static FfxErrorCode ankiFsr2ExecuteRenderJobClearFloat(AnkiFsr2BackendCtx& backendContext, FfxRenderJobDescription& job,
											   CommandBufferPtr& cmdBuffer)
{
	const U32 idx = job.clearJobDescriptor.target.internalIndex;
	AnkiFsr2Resource& ffxResource = backendContext.resources[idx];

	if(ffxResource.resourceDescription.type != FFX_RESOURCE_TYPE_BUFFER)
	{
		ankiFsr2AddBarrier(cmdBuffer, backendContext, job.clearJobDescriptor.target, FFX_RESOURCE_STATE_COPY_DEST);
		ClearValue clearVal;
		FfxClearFloatJobDescription& clearDesc = job.clearJobDescriptor;
		clearVal.m_colorf = {
			clearDesc.color[0],
			clearDesc.color[1],
			clearDesc.color[2],
			clearDesc.color[3]
		};
		cmdBuffer->clearTextureView(ffxResource.m_allMipsImageView, clearVal);
	}

	return FFX_OK;
}

FfxErrorCode ankiFsr2ExecuteRenderJobs(FfxFsr2Interface* backendInterface, FfxCommandList commandList)
{
    FFX_ASSERT(NULL != backendInterface);

    AnkiFsr2BackendCtx& backendContext = *reinterpret_cast<AnkiFsr2BackendCtx*>(backendInterface->scratchBuffer);

    FfxErrorCode errorCode = FFX_OK;

    // execute all renderjobs
    for (uint32_t i = 0; i < backendContext.renderJobCount; ++i)
    {
        FfxRenderJobDescription& renderJob = backendContext.renderJobs[i];
        CommandBufferPtr cmdBuff = CommandBufferPtr(reinterpret_cast<CommandBuffer*>(commandList));

        switch (renderJob.jobType)
        {
        case FFX_RENDER_JOB_CLEAR_FLOAT:
        {
			errorCode = ankiFsr2ExecuteRenderJobClearFloat(backendContext, renderJob, cmdBuff);
            break;
        }
        case FFX_RENDER_JOB_COPY:
        {
			ANKI_ASSERT(0 && "With our integration no Copy jobs should be needed");
			return Error::FUNCTION_FAILED;

            break;
        }
        case FFX_RENDER_JOB_COMPUTE:
        {
			errorCode = ankiFsr2ExecuteRenderJobCompute(backendContext, renderJob, cmdBuff);
            break;
        }
        default:;
        }
    }
    // check the execute function returned cleanly.
    FFX_RETURN_ON_ERROR(errorCode == FFX_OK, FFX_ERROR_BACKEND_API_ERROR);
    backendContext.renderJobCount = 0;
    return FFX_OK;
}

FfxErrorCode ankiFsr2DestroyResource([[maybe_unused]] FfxFsr2Interface* backendInterface,
									 [[maybe_unused]] FfxResourceInternal resource)
{
	return FFX_OK;
}

FfxErrorCode ankiFsr2DestroyPipeline([[maybe_unused]] FfxFsr2Interface* backendInterface,
									 [[maybe_unused]] FfxPipelineState* pipeline)
{
	return FFX_OK;
}

} // end namespace anki
