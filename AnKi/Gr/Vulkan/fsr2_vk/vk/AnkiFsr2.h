// Copyright (C) 2009-2022, Panagiotis Christopoulos Charitos and contributors.
// All rights reserved.
// Code licensed under the BSD License.
// http://www.anki3d.org/LICENSE

#pragma once

#define ANKI_ENABLE_CUSTOM_FSR2_BACKEND 0

#if ANKI_ENABLE_CUSTOM_FSR2_BACKEND

#include <AnKi/Gr/Vulkan/Common.h>
#include <ThirdParty/FidelityFX/fsr2/ffx-fsr2-api/ffx_fsr2_interface.h>
#include <AnKi/Renderer/RendererObject.h>

namespace anki {

	class GrManager;

    FFX_API size_t ffxFsr2GetScratchMemorySizeVK(VkPhysicalDevice physicalDevice);

    FFX_API FfxErrorCode ffxFsr2GetInterfaceVK(
        FfxFsr2Interface* outInterface,
        void* scratchBuffer,
        size_t scratchBufferSize,
		Renderer* renderer);

    FFX_API FfxDevice ffxGetDeviceVK(GrManager* grManager);

	FFX_API FfxCommandList ffxGetCommandListVK(VkCommandBuffer cmdBuf);


	FFX_API FfxResource ffxGetTextureResourceVK(FfxFsr2Context* context, const TexturePtr& texture,
												const TextureViewPtr& textureView,
												uint32_t width, uint32_t height,
												wchar_t* name = nullptr,
												FfxResourceStates state = FFX_RESOURCE_STATE_COMPUTE_READ);

} // end namespace anki

#endif
