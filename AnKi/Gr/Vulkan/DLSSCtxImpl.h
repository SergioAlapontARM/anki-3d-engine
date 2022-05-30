// Copyright (C) 2009-2022, Panagiotis Christopoulos Charitos and contributors.
// All rights reserved.
// Code licensed under the BSD License.
// http://www.anki3d.org/LICENSE

#pragma once

#include <AnKi/Gr/DLSSCtx.h>
#include <AnKi/Gr/Vulkan/VulkanObject.h>
#include <AnKi/Gr/Vulkan/GpuMemoryManager.h>

// Ngx Sdk forward declarations
struct NVSDK_NGX_Parameter;
struct NVSDK_NGX_Handle;

namespace anki {

/// @addtogroup vulkan
/// @{

class DLSSRecommendedSettings
{
public:
	F32 m_recommendedSharpness = 0.01f;
	UVec2 m_recommendedOptimalRenderSize = { ~(0u), ~(0u) };
	UVec2 m_dynamicMaximumRenderSize = { ~(0u), ~(0u) };
	UVec2 m_dynamicMinimumRenderSize = { ~(0u), ~(0u) };
	DLSSQualityMode m_qualityMode;
};

class DLSSCtxImpl final : public DLSSCtx, public VulkanObject<DLSSCtx, DLSSCtxImpl>
{
public:
	DLSSCtxImpl(GrManager* manager, CString name)
		: DLSSCtx(manager, name)
		, m_ngxInitialized(false)
		, m_ngxParameters(nullptr)
		, m_dlssFeature(nullptr)
	{
	}

	~DLSSCtxImpl();

	ANKI_USE_RESULT Error init(const DLSSCtxInitInfo& init);

	Bool isNgxInitialized() const { return m_ngxInitialized; }

	void upscale(CommandBufferPtr cmdb,
		const TexturePtr srcRt,
		const TexturePtr dstRt,
		const TexturePtr mvRt,
		const TexturePtr depthRt,
		const TexturePtr exposure,
		const Bool resetAccumulation,
		const F32 sharpness,
		const Vec2& jitterOffset,
		const Vec2& mVScale);

private:

	void shutdown();

	Error createDLSSFeature(const UVec2& srcRes, const UVec2& dstRes, const DLSSQualityMode mode);

	void releaseDLSSFeature();

	Error queryOptimalSettings(const UVec2& displayRes, const DLSSQualityMode mode, DLSSRecommendedSettings* outRecommendedSettings);

	Bool m_ngxInitialized;
	NVSDK_NGX_Parameter* m_ngxParameters;
	NVSDK_NGX_Handle* m_dlssFeature;
	DLSSRecommendedSettings m_recommendedSettings;
};
/// @}

} // end namespace anki